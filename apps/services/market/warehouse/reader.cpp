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

wire::DatasetChange
change_of(const Store& store, const VersionHead& version, const VersionHead& base)
{
    const auto counts = store.compare(version, base);
    wire::DatasetChange out;
    out.mutable_dataset()->set_provider(store.provider());
    out.mutable_dataset()->set_version(version.identity);
    out.mutable_dataset()->set_created_ms(version.created_ns / 1'000'000);
    out.set_base(base.identity);
    out.set_added(counts.added);
    out.set_replaced(counts.replaced);
    out.set_removed(counts.removed);
    return out;
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
    const auto identity =
        query.dataset().version().empty() ? store.current_identity() : query.dataset().version();
    wire::DatasetVersion result;
    result.mutable_dataset()->set_provider(store.provider());
    if (identity.empty())
        return result;
    const auto top = store.head(identity);
    result.mutable_dataset()->set_version(top.identity);
    result.mutable_dataset()->set_created_ms(top.created_ns / 1'000'000);
    result.set_parent(top.parent);
    const auto first = query.after_key().empty() ? std::string{} : shard_of(query.after_key());
    // Only the shards this page can list are read.
    for (const auto& [name, digest] : top.shards) {
        const auto slash = name.find('/');
        const auto end = name.find('/', slash + 1);
        if (name < first || (!wanted.empty() && wanted != name.substr(0, slash))
            || (!query.exchange().empty()
                && query.exchange() != name.substr(slash + 1, end - slash - 1)))
            continue;
        const auto partitions = store.shard(name, digest);
        for (auto at = partitions.upper_bound(query.after_key()); at != partitions.end(); ++at) {
            const auto& [key, part] = *at;
            if (result.partitions_size() == page_) {
                result.set_next_key(result.partitions(result.partitions_size() - 1).key());
                break;
            }
            auto* item = result.add_partitions();
            item->set_key(key);
            item->set_table(protocol::table_of(key.substr(0, slash)));
            item->set_sha256(part.sha256);
            item->set_rows(part.rows);
            for (const auto& day : part.days)
                item->add_days(day);
            item->set_taken_from(part.source);
        }
        if (!result.next_key().empty())
            break;
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
    const auto [part, path] = store.locate(query.dataset().version(), query.key());
    const auto size = fs::file_size(path);
    if (query.offset() > size)
        throw std::invalid_argument("dataset offset exceeds the partition size");
    wire::DatasetFile result;
    result.set_sha256(part.sha256);
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
        {"daily",
            "calendar",
            "contracts",
            "minutes",
            "settlement",
            "observations",
            "price_ticks"}) {
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
            const std::string day = std::string_view(table) == "price_ticks" ? "effective_from"
                : std::string_view(table) == "contracts"                     ? "list_date"
                                                                             : "day";
            const std::string last =
                std::string_view(table) == "price_ticks" ? "effective_until - 1" : day;
            const auto dates = engine.run(
                "SELECT min(" + day + "), max(" + last + ") FROM read_parquet([" + files + "])");
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

wire::DatasetVersions Reader::versions(const wire::ListDatasetVersions& query) const
{
    require_provider(query.provider());
    const Store store(source_directory(directory_, query.provider()), query.provider());
    auto next = query.after_version().empty() ? store.current_identity()
                                              : store.head(query.after_version()).parent;
    wire::DatasetVersions result;
    while (!next.empty() && result.versions_size() < 20) {
        const auto version = store.head(next);
        *result.add_versions() = change_of(
            store,
            version,
            version.parent.empty() ? VersionHead{} : store.head(version.parent));
        next = version.parent;
    }
    if (!next.empty())
        result.set_next_version(result.versions().rbegin()->dataset().version());
    return result;
}

wire::DatasetChange Reader::change(const wire::CompareDatasetVersions& query) const
{
    require_provider(query.provider());
    const Store store(source_directory(directory_, query.provider()), query.provider());
    return change_of(store, store.head(query.version()), store.head(query.base()));
}

wire::TableRows Reader::rows(const wire::GetTableRows& query) const
{
    const auto table = protocol::table_name(query.table());
    if (table != "contracts" && table != "calendar" && table != "settlement"
        && table != "price_ticks")
        throw std::invalid_argument("only reference tables can be browsed");
    const bool dated = table != "contracts";
    if (dated) {
        require_day(query.first_day());
        require_day(query.last_day());
    }
    const auto store = open_store(directory_, query.dataset());
    const auto version = selected(store, query.dataset().version());
    wire::TableRows result;
    for (const auto& column : columns(table))
        result.add_columns(column.name);
    const auto prefix = table + '/' + query.exchange();
    std::string files;
    for (const auto& [key, part] : version.partitions) {
        if (key != prefix && !key.starts_with(prefix + '/'))
            continue;
        const auto period = key.substr(std::min(key.size(), prefix.size() + 1));
        if (dated && table != "price_ticks"
            && (period < query.first_day().substr(0, period.size())
                || period > query.last_day().substr(0, period.size())))
            continue;
        files += (files.empty() ? "" : ",") + parquet::literal(store.file(version, key));
    }
    if (files.empty())
        return result;
    const auto filter = table == "price_ticks" ? " WHERE effective_from <= DATE '"
            + query.last_day() + "' AND effective_until > DATE '" + query.first_day() + "'"
        : dated
        ? " WHERE day BETWEEN DATE '" + query.first_day() + "' AND DATE '" + query.last_day() + "'"
        : std::string{};
    parquet::Engine engine;
    engine.run(
        "CREATE TEMP VIEW t AS SELECT * FROM read_parquet([" + files
        + "])"
        // Both days were validated as YYYY-MM-DD.
        + filter);
    result.set_total(
        engine.run("SELECT count(*) FROM t")->GetValue(0, 0).GetValue<std::uint32_t>());
    const auto rows = engine.run(
        std::string("SELECT * FROM t ORDER BY ")
        + (table == "price_ticks" ? "effective_from, product, "
                : dated           ? "day, "
                                  : "")
        + (table == "calendar" ? "exchange" : "contract") + " LIMIT 100 OFFSET "
        + std::to_string(query.offset()));
    for (duckdb::idx_t row = 0; row < rows->RowCount(); ++row) {
        auto* out = result.add_rows();
        for (duckdb::idx_t column = 0; column < rows->ColumnCount(); ++column) {
            const auto value = rows->GetValue(column, row);
            out->add_values(value.IsNull() ? std::string{} : value.ToString());
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
