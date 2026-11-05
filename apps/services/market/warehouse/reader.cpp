#include "reader.hpp"
#include "parquet_engine.hpp"
#include <asterion/protocol/market.hpp>
#include <asterion/base/error.hpp>
#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>
#include <set>

namespace asterion::market_data::warehouse {
namespace fs = std::filesystem;
namespace wire = ::market;

namespace {

Version selected(const Store& store, const std::string& identity)
{
    return identity.empty() ? store.current() : store.version(identity);
}

Store open_store(const fs::path& directory, const wire::DatasetRef& ref)
{
    return Store(source_directory(directory, ref.provider()), ref.provider());
}

wire::Dataset dataset_of(const Version& version)
{
    wire::Dataset out;
    out.set_provider(version.provider);
    out.set_version(version.identity);
    out.set_created_ms(version.created_ns / 1'000'000);
    return out;
}

} // namespace

wire::Datasets Reader::datasets() const
{
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
    wire::Datasets result;
    for (const auto& provider : providers)
        *result.add_datasets() =
            dataset_of(Store(source_directory(directory_, provider), provider).current());
    return result;
}

wire::DatasetVersion Reader::version(const wire::GetDatasetVersion& query) const
{
    if (!query.after_key().empty()) {
        require_key(query.after_key());
        require_identity(query.dataset().version());
    }
    const auto wanted = query.table() == wire::TABLE_UNSPECIFIED
        ? std::string{}
        : protocol::table_name(query.table());
    const auto store = open_store(directory_, query.dataset());
    const auto version = selected(store, query.dataset().version());
    wire::DatasetVersion result;
    *result.mutable_dataset() = dataset_of(version);
    result.set_parent(version.parent);
    for (auto at = version.partitions.upper_bound(query.after_key());
        at != version.partitions.end();
        ++at) {
        const auto& [key, part] = *at;
        const auto slash = key.find('/');
        const auto table = key.substr(0, slash);
        const auto exchange = key.substr(slash + 1, key.find('/', slash + 1) - slash - 1);
        if ((!wanted.empty() && wanted != table)
            || (!query.exchange().empty() && query.exchange() != exchange))
            continue;
        if (result.partitions_size() == page_) {
            result.set_next_key(result.partitions(result.partitions_size() - 1).key());
            break;
        }
        auto* item = result.add_partitions();
        item->set_key(key);
        item->set_table(protocol::table_of(table));
        item->set_sha256(part.sha256);
        item->set_rows(part.rows);
        for (const auto& day : part.days)
            item->add_days(day);
        item->set_taken_from(part.source);
    }
    if (result.ByteSizeLong() > 1024 * 1024)
        throw Error(
            ErrorCode::resource_exhausted,
            "dataset page is too large; request fewer partitions");
    return result;
}

wire::DatasetFile Reader::file(const wire::ReadDatasetFile& query) const
{
    const std::uint64_t count = 1024 * 1024;
    require_identity(query.dataset().version());
    const auto store = open_store(directory_, query.dataset());
    const auto version = store.version(query.dataset().version());
    const auto path = store.file(version, query.key());
    const auto size = fs::file_size(path);
    if (query.offset() > size)
        throw std::invalid_argument("dataset offset exceeds the partition size");
    wire::DatasetFile result;
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

wire::DatasetSummary Reader::summary(const wire::DatasetRef& query) const
{
    const auto store = open_store(directory_, query);
    const auto version = selected(store, query.version());
    wire::DatasetSummary result;
    *result.mutable_dataset() = dataset_of(version);
    for (const auto* table :
        {"daily", "calendar", "contracts", "minutes", "settlement", "observations"}) {
        auto* item = result.add_tables();
        item->set_table(protocol::table_of(table));
        std::string files;
        // Partitions name the days they hold; only those that do not are read.
        std::set<std::string> days;
        for (const auto& [key, part] : version.partitions) {
            if (!key.starts_with(std::string(table) + '/'))
                continue;
            item->set_partitions(item->partitions() + 1);
            item->set_rows(item->rows() + part.rows);
            if (part.days.empty())
                files += (files.empty() ? "" : ",") + parquet::literal(store.file(version, key));
            days.insert(part.days.begin(), part.days.end());
        }
        if (!files.empty()) {
            parquet::Engine engine;
            const std::string day = std::string_view(table) == "contracts" ? "list_date" : "day";
            const auto dates = engine.run(
                "SELECT min(" + day + "), max(" + day + ") FROM read_parquet([" + files + "])");
            for (const auto column : {0, 1})
                if (!dates->GetValue(column, 0).IsNull())
                    days.insert(dates->GetValue(column, 0).ToString());
        }
        if (!days.empty()) {
            item->set_first_day(*days.begin());
            item->set_last_day(*days.rbegin());
        }
    }
    return result;
}

wire::DatasetCoverage Reader::coverage(const wire::DatasetRef& query) const
{
    require_identity(query.version());
    const auto store = open_store(directory_, query);
    const auto version = store.version(query.version());
    wire::DatasetCoverage result;
    // minutes/<exchange>/<day>/<product>/<month>
    std::map<std::string, std::map<std::string, wire::MinuteMonth>> minutes;
    std::string daily, calendar;
    for (const auto& [key, part] : version.partitions) {
        if (key.starts_with("minutes/")) {
            std::istringstream fields(key);
            std::string table, exchange, day, product, delivery;
            for (auto* field : {&table, &exchange, &day, &product, &delivery})
                std::getline(fields, *field, '/');
            auto& month = minutes[exchange + '/' + product + '/' + delivery][day.substr(0, 7)];
            month.set_days(month.days() + 1);
            month.set_recorded_days(month.recorded_days() + (part.source == "recorder"));
            continue;
        }
        auto* files = key.starts_with("daily/") ? &daily
            : key.starts_with("calendar/")      ? &calendar
                                                : nullptr;
        if (files)
            *files += (files->empty() ? "" : ",") + parquet::literal(store.file(version, key));
    }
    for (auto& [contract, months] : minutes) {
        auto* item = result.add_contracts();
        item->set_contract(contract);
        for (auto& [name, month] : months) {
            month.set_month(name);
            *item->add_months() = std::move(month);
        }
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
        "WITH d AS (SELECT exchange, product, strftime(day, '%Y-%m') AS month," " count(DISTINCT day) AS days FROM daily GROUP BY ALL)," " c AS (SELECT exchange, strftime(day, '%Y-%m') AS month, count(DISTINCT day) AS open_days" " FROM calendar WHERE open GROUP BY ALL)" " SELECT d.exchange, d.product, d.month, d.days, coalesce(c.open_days, 0)" " FROM d LEFT JOIN c USING (exchange, month) ORDER BY 1, 2, 3");
    wire::ProductCoverage* product = nullptr;
    for (duckdb::idx_t row = 0; row < rows->RowCount(); ++row) {
        const auto exchange = rows->GetValue(0, row).ToString();
        const auto name = rows->GetValue(1, row).ToString();
        const auto key = exchange + '/' + name;
        if (!product || product->product() != key) {
            product = result.add_products();
            product->set_product(key);
        }
        auto* month = product->add_months();
        month->set_month(rows->GetValue(2, row).ToString());
        month->set_days(rows->GetValue(3, row).GetValue<std::uint32_t>());
        month->set_open_days(rows->GetValue(4, row).GetValue<std::uint32_t>());
    }
    return result;
}

} // namespace asterion::market_data::warehouse
