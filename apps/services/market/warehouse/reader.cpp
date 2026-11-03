#include "reader.hpp"
#include "parquet_engine.hpp"
#include <asterion/base/error.hpp>
#include <algorithm>
#include <fstream>
#include <set>
#include <regex>

namespace asterion::market_data::warehouse {
namespace fs = std::filesystem;
namespace wire = dataset::proto;

namespace {

std::uint32_t limit(std::uint32_t requested, std::uint32_t standard, std::uint32_t maximum)
{
    if (requested > maximum)
        throw std::invalid_argument("dataset read limit is too large");
    return requested ? requested : standard;
}

Version selected(const Store& store, const std::string& identity)
{
    return identity.empty() ? store.current() : store.version(identity);
}

} // namespace

wire::Sources Reader::sources(const wire::SourcesQuery& query) const
{
    const auto count = limit(query.limit(), 64, 128);
    if (!query.after_provider().empty())
        require_provider(query.after_provider());
    std::set<std::string> providers;
    if (fs::exists(directory_ / "warehouse/current"))
        providers.insert("tushare");
    const auto sources = directory_ / "sources";
    if (fs::is_directory(sources)) {
        for (const auto& entry : fs::directory_iterator(sources)) {
            if (entry.is_directory() && fs::exists(entry.path() / "warehouse/current"))
                providers.insert(entry.path().filename().string());
        }
    }
    wire::Sources result;
    for (auto at = providers.upper_bound(query.after_provider()); at != providers.end(); ++at) {
        if (result.sources_size() == static_cast<int>(count)) {
            result.set_next_provider(result.sources(result.sources_size() - 1).provider());
            break;
        }
        const auto version = Store(source_directory(directory_, *at), *at).current();
        auto* source = result.add_sources();
        source->set_provider(*at);
        source->set_dataset_version(version.identity);
        source->set_created_ns(version.created_ns);
    }
    return result;
}

wire::Version Reader::version(const wire::VersionQuery& query) const
{
    const auto count = limit(query.limit(), 128, 512);
    if (!query.after_key().empty()) {
        require_key(query.after_key());
        require_identity(query.dataset_version());
    }
    if (!query.table().empty())
        columns(query.table());
    const Store store(source_directory(directory_, query.provider()), query.provider());
    const auto version = selected(store, query.dataset_version());
    wire::Version result;
    result.set_provider(version.provider);
    result.set_dataset_version(version.identity);
    result.set_parent(version.parent);
    result.set_created_ns(version.created_ns);
    for (auto at = version.partitions.upper_bound(query.after_key());
        at != version.partitions.end();
        ++at) {
        const auto& [key, part] = *at;
        const auto slash = key.find('/');
        const auto table = key.substr(0, slash);
        const auto exchange = key.substr(slash + 1, key.find('/', slash + 1) - slash - 1);
        if ((!query.table().empty() && query.table() != table)
            || (!query.exchange().empty() && query.exchange() != exchange))
            continue;
        if (result.partitions_size() == static_cast<int>(count)) {
            result.set_next_key(result.partitions(result.partitions_size() - 1).key());
            break;
        }
        auto* item = result.add_partitions();
        item->set_key(key);
        item->set_sha256(part.sha256);
        item->set_rows(part.rows);
        for (const auto& day : part.days)
            item->add_days(day);
        item->set_source(part.source);
    }
    if (result.ByteSizeLong() > 1024 * 1024)
        throw Error(
            ErrorCode::resource_exhausted,
            "dataset page is too large; request fewer partitions");
    return result;
}

wire::File Reader::file(const wire::FileQuery& query) const
{
    const auto count = limit(query.limit(), 1024 * 1024, 1024 * 1024);
    require_identity(query.dataset_version());
    const Store store(source_directory(directory_, query.provider()), query.provider());
    const auto version = store.version(query.dataset_version());
    const auto path = store.file(version, query.key());
    const auto size = fs::file_size(path);
    if (query.offset() > size)
        throw std::invalid_argument("dataset offset exceeds the partition size");
    wire::File result;
    result.set_provider(version.provider);
    result.set_dataset_version(version.identity);
    result.set_key(query.key());
    result.set_sha256(version.partitions.at(query.key()).sha256);
    result.set_offset(query.offset());
    result.set_size(size);
    auto* data = result.mutable_data();
    data->resize(std::min<std::uint64_t>(count, size - query.offset()));
    std::ifstream input(path, std::ios::binary);
    input.seekg(static_cast<std::streamoff>(query.offset()));
    if (!input || !input.read(data->data(), static_cast<std::streamsize>(data->size())))
        throw Error(ErrorCode::operation_failed, "cannot read dataset partition");
    return result;
}

wire::Summary Reader::summary(const wire::SummaryQuery& query) const
{
    const Store store(source_directory(directory_, query.provider()), query.provider());
    const auto version = selected(store, query.dataset_version());
    wire::Summary result;
    result.set_provider(version.provider);
    result.set_dataset_version(version.identity);
    result.set_created_ns(version.created_ns);
    for (const auto* table :
        {"daily", "calendar", "contracts", "minutes", "settlement", "observations"}) {
        auto* item = result.add_tables();
        item->set_table(table);
        std::string files;
        for (const auto& [key, part] : version.partitions) {
            if (!key.starts_with(std::string(table) + '/'))
                continue;
            files += (files.empty() ? "" : ",") + parquet::literal(store.file(version, key));
            item->set_partitions(item->partitions() + 1);
            item->set_rows(item->rows() + part.rows);
        }
        if (files.empty())
            continue;
        parquet::Engine engine;
        const std::string day = std::string_view(table) == "contracts" ? "list_date" : "day";
        const auto dates = engine.run(
            "SELECT min(" + day + "), max(" + day + ") FROM read_parquet([" + files + "])");
        if (!dates->GetValue(0, 0).IsNull())
            item->set_first_day(dates->GetValue(0, 0).ToString());
        if (!dates->GetValue(1, 0).IsNull())
            item->set_last_day(dates->GetValue(1, 0).ToString());
    }
    return result;
}

wire::Coverage Reader::coverage(const wire::CoverageQuery& query) const
{
    const auto count = limit(query.limit(), 64, 128);
    require_identity(query.dataset_version());
    if (!query.after_product().empty()
        && !std::regex_match(query.after_product(), std::regex("[A-Z]{3,5}/[a-z]{1,3}")))
        throw std::invalid_argument("invalid coverage product cursor");
    const Store store(source_directory(directory_, query.provider()), query.provider());
    const auto version = store.version(query.dataset_version());
    wire::Coverage result;
    result.set_provider(version.provider);
    result.set_dataset_version(version.identity);
    std::string daily, calendar;
    for (const auto& [key, part] : version.partitions) {
        auto* files = key.starts_with("daily/") ? &daily
            : key.starts_with("calendar/")      ? &calendar
                                                : nullptr;
        if (files)
            *files += (files->empty() ? "" : ",") + parquet::literal(store.file(version, key));
    }
    if (daily.empty())
        return result;
    parquet::Engine engine;
    engine.run("CREATE TEMP VIEW daily AS SELECT * FROM read_parquet([" + daily + "])");
    engine.run(
        calendar.empty()
            ? "CREATE TEMP VIEW calendar AS SELECT NULL::VARCHAR AS exchange, NULL::DATE AS day," " false AS open WHERE false"
            : "CREATE TEMP VIEW calendar AS SELECT * FROM read_parquet([" + calendar + "])");
    const auto rows = engine.run(
        "WITH products AS (SELECT DISTINCT exchange, product FROM daily WHERE exchange || '/' || product > '"
        + query.after_product() + "' ORDER BY exchange, product LIMIT " + std::to_string(count + 1)
        + "), d AS (SELECT exchange, product, strftime(day, '%Y-%m') AS month," " count(DISTINCT day) AS days FROM daily JOIN products USING (exchange, product) GROUP BY ALL)," " c AS (SELECT exchange, strftime(day, '%Y-%m') AS month, count(DISTINCT day) AS open_days" " FROM calendar WHERE open GROUP BY ALL)" " SELECT d.exchange, d.product, d.month, d.days, coalesce(c.open_days, 0)" " FROM d LEFT JOIN c USING (exchange, month) ORDER BY 1, 2, 3");
    wire::ProductCoverage* product = nullptr;
    for (duckdb::idx_t row = 0; row < rows->RowCount(); ++row) {
        const auto exchange = rows->GetValue(0, row).ToString();
        const auto name = rows->GetValue(1, row).ToString();
        if (!product || product->exchange() != exchange || product->product() != name) {
            if (result.products_size() == static_cast<int>(count)) {
                result.set_next_product(product->exchange() + '/' + product->product());
                break;
            }
            product = result.add_products();
            product->set_exchange(exchange);
            product->set_product(name);
        }
        auto* month = product->add_months();
        month->set_month(rows->GetValue(2, row).ToString());
        month->set_days(rows->GetValue(3, row).GetValue<std::uint32_t>());
        month->set_open_days(rows->GetValue(4, row).GetValue<std::uint32_t>());
    }
    return result;
}

} // namespace asterion::market_data::warehouse
