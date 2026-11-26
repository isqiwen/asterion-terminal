#include "reader.hpp"
#include "parquet_engine.hpp"
#include "reader_cache.hpp"
#include <asterion/base/error.hpp>
#include <asterion/futures/trading_days.hpp>
#include <asterion/protocol/warehouse.hpp>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <ranges>
#include <regex>
#include <set>

namespace asterion::warehouse {
namespace fs = std::filesystem;
namespace wire = ::warehouse;

namespace {

wire::Dataset dataset_of(const Version& version)
{
    wire::Dataset out;
    out.set_version(version.identity);
    out.set_created_ms(version.created_ns / 1'000'000);
    return out;
}

std::string_view trim_ascii(std::string_view value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.remove_suffix(1);
    return value;
}

std::string lower_ascii(std::string_view value)
{
    std::string out;
    out.reserve(value.size());
    for (const char c : value)
        out += c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    return out;
}

std::string sql_literal(std::string_view value)
{
    if (value.find('\0') != std::string_view::npos)
        throw std::invalid_argument("invalid filter text");
    std::string out = "'";
    for (const char c : value) {
        out += c;
        if (c == '\'')
            out += '\'';
    }
    return out + "'";
}

std::string sql_like(std::string_view value)
{
    std::string escaped = "%";
    for (const char c : lower_ascii(value)) {
        if (c == '\\' || c == '%' || c == '_')
            escaped += '\\';
        escaped += c;
    }
    escaped += '%';
    return sql_literal(escaped);
}

std::string table_filter_sql(std::string_view table, std::string_view product, std::string_view day)
{
    std::string where;
    const auto append = [&](const std::string& clause) {
        if (!where.empty())
            where += " AND ";
        where += clause;
    };
    if (!product.empty()) {
        if (table == "calendar")
            throw std::invalid_argument("calendar does not support product filtering");
        const bool has_contract = table == "contracts" || table == "settlement"
            || table == "member_positions" || table == "price_ticks";
        static const std::regex compact(R"(^([A-Za-z]+)([0-9]{4})$)");
        std::cmatch match;
        if (std::regex_match(product.begin(), product.end(), match, compact)) {
            const auto prod = sql_literal(lower_ascii(match[1].str()));
            if (has_contract) {
                const auto yymm = match[2].str();
                const auto month = "%/20" + yymm.substr(0, 2) + "-" + yymm.substr(2, 2);
                append(
                    "(lower(product) = " + prod + " AND lower(contract) LIKE " + sql_literal(month)
                    + ")");
            } else {
                append("lower(product) = " + prod);
            }
        } else if (const auto first = product.find('/'); first != std::string_view::npos) {
            const auto second = product.find('/', first + 1);
            if (has_contract && second != std::string_view::npos) {
                append("lower(contract) = " + sql_literal(lower_ascii(product)));
            } else {
                const auto prod = second == std::string_view::npos
                    ? product.substr(first + 1)
                    : product.substr(first + 1, second - first - 1);
                append("lower(product) = " + sql_literal(lower_ascii(prod)));
            }
        } else {
            const auto exact = sql_literal(lower_ascii(product));
            if (table == "contracts") {
                const auto pattern = sql_like(product);
                append(
                    "(lower(product) = " + exact + " OR lower(contract) LIKE " + pattern
                    + " ESCAPE '\\' OR lower(source_instrument) LIKE " + pattern
                    + " ESCAPE '\\' OR lower(name) LIKE " + pattern + " ESCAPE '\\')");
            } else if (has_contract) {
                append(
                    "(lower(product) = " + exact + " OR lower(contract) LIKE " + sql_like(product)
                    + " ESCAPE '\\')");
            } else {
                append("lower(product) = " + exact);
            }
        }
    }
    if (!day.empty()) {
        (void)parse_trading_date(day);
        const auto date = "DATE '" + std::string(day) + "'";
        if (table == "price_ticks") {
            append(
                "(effective_from <= " + date + " AND (effective_until IS NULL OR effective_until > "
                + date + "))");
        } else if (table == "contracts") {
            append(
                "((list_date IS NULL OR list_date <= " + date
                + ") AND (delist_date IS NULL OR delist_date >= " + date + "))");
        } else {
            append("day = " + date);
        }
    }
    return where;
}

void append_rows(wire::TableRows& result, duckdb::MaterializedQueryResult& rows)
{
    for (duckdb::idx_t row = 0; row < rows.RowCount(); ++row) {
        auto* out = result.add_rows();
        for (duckdb::idx_t column = 0; column < rows.ColumnCount(); ++column) {
            const auto value = rows.GetValue(column, row);
            out->add_values(value.IsNull() ? std::string{} : value.ToString());
        }
    }
}

} // namespace

Reader::Reader(fs::path directory, int page)
    : directory_(std::move(directory)),
      page_(page),
      cache_(std::make_shared<Cache>())
{}

wire::DatasetVersion Reader::version(const wire::GetDatasetVersion& query) const
{
    if (!query.after_key().empty()) {
        require_key(query.after_key());
        require_identity(query.dataset().version());
    }
    const auto wanted = query.table() == wire::TABLE_UNSPECIFIED
        ? std::string{}
        : protocol::table_name(query.table());
    const auto store = research_dataset(directory_);
    const auto identity =
        query.dataset().version().empty() ? store.current_identity() : query.dataset().version();
    wire::DatasetVersion result;
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
    const auto store = research_dataset(directory_);
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
    const auto store = research_dataset(directory_);
    const auto identity = query.version().empty() ? store.current_identity() : query.version();
    if (!identity.empty())
        require_identity(identity);
    const auto day = yesterday();
    const Cache::Key cache_key{
        identity,
        day,
        empty_stamp(
            directory_,
            {"daily", "minutes", "settlement", "warehouse_receipts", "member_positions"})
    };
    if (!identity.empty()) {
        std::lock_guard lock(cache_->mutex);
        if (auto found = cache_->summaries.get(cache_key))
            return *found;
    }
    const auto version = cache_->version(store, identity);
    wire::DatasetSummary result;
    *result.mutable_dataset() = dataset_of(*version);
    const auto open = open_days(store, *version, day);
    for (const auto* table :
        {"daily",
            "calendar",
            "contracts",
            "minutes",
            "settlement",
            "price_ticks",
            "warehouse_receipts",
            "member_positions",
            "inventory"}) {
        auto* item = result.add_tables();
        item->set_table(protocol::table_of(table));
        std::string files;
        // Partitions name the days they hold; only those that do not are read.
        std::set<std::string> days;
        const auto prefix = std::string(table) + '/';
        for (auto at = version->partitions.lower_bound(prefix);
            at != version->partitions.end() && at->first.starts_with(prefix);
            ++at) {
            const auto& [key, part] = *at;
            item->set_partitions(item->partitions() + 1);
            item->set_rows(item->rows() + part.rows);
            if (part.days.empty())
                files += (files.empty() ? "" : ",") + parquet::literal(store.file(*version, key));
            days.insert(part.days.begin(), part.days.end());
        }
        if (!files.empty()) {
            parquet::Engine engine;
            const std::string day_col = std::string_view(table) == "price_ticks" ? "effective_from"
                : std::string_view(table) == "contracts"                         ? "list_date"
                                                                                 : "day";
            const std::string last =
                std::string_view(table) == "price_ticks" ? "effective_until - 1" : day_col;
            const auto dates = engine.run(
                "SELECT min(" + day_col + "), max(" + last + ") FROM read_parquet([" + files
                + "])");
            for (const auto column : {0, 1})
                if (!dates->GetValue(column, 0).IsNull())
                    days.insert(dates->GetValue(column, 0).ToString());
        }
        if (!days.empty()) {
            item->set_first_day(*days.begin());
            item->set_last_day(*days.rbegin());
        }
        if (std::string_view(table) == "calendar" || std::string_view(table) == "contracts"
            || std::string_view(table) == "price_ticks" || std::string_view(table) == "inventory")
            continue;
        for (const auto& [holder, holding] :
            holdings(store, *version, table, open, empty_keys(directory_, table))) {
            item->set_missing(item->missing() + holding.missing);
            item->set_source_empty(item->source_empty() + holding.empty);
            item->set_expected_last_day(std::max(item->expected_last_day(), holding.through));
        }
    }
    if (!identity.empty()) {
        std::lock_guard lock(cache_->mutex);
        cache_->summaries.put(cache_key, result, 4);
    }
    return result;
}

wire::DatasetVersions Reader::versions(const wire::ListDatasetVersions& query) const
{
    const auto store = research_dataset(directory_);
    auto next = query.after_version().empty() ? store.current_identity()
                                              : cache_->head(store, query.after_version()).parent;
    wire::DatasetVersions result;
    while (!next.empty() && result.versions_size() < 20) {
        const auto version = cache_->head(store, next);
        const bool has_parent = !version.parent.empty() && store.has_version(version.parent);
        *result.add_versions() = cache_->change(
            store,
            version,
            has_parent ? cache_->head(store, version.parent) : VersionHead{});
        next = has_parent ? version.parent : std::string{};
    }
    if (!next.empty())
        result.set_next_version(result.versions().rbegin()->dataset().version());
    return result;
}

wire::DatasetChange Reader::change(const wire::CompareDatasetVersions& query) const
{
    const auto store = research_dataset(directory_);
    return cache_->change(
        store,
        cache_->head(store, query.version()),
        cache_->head(store, query.base()));
}

wire::TableRows Reader::rows(const wire::GetTableRows& query) const
{
    const auto table = protocol::table_name(query.table());
    if (table != "contracts" && table != "calendar" && table != "settlement"
        && table != "price_ticks" && table != "warehouse_receipts" && table != "member_positions"
        && table != "inventory")
        throw std::invalid_argument("only reference tables can be browsed");
    const auto product = trim_ascii(query.product());
    const auto day = trim_ascii(query.day());
    const auto where = table_filter_sql(table, product, day);
    const auto store = research_dataset(directory_);
    const auto identity =
        query.dataset().version().empty() ? store.current_identity() : query.dataset().version();
    const auto version = cache_->version(store, identity);
    wire::TableRows result;
    for (const auto& column : columns(table))
        result.add_columns(column.name);
    const auto prefix = table + '/' + query.exchange();
    const bool daily_partitions = table == "settlement" || table == "warehouse_receipts"
        || table == "member_positions" || table == "inventory";
    const auto order_by = std::string(
                              table == "price_ticks"     ? "effective_from DESC, product, "
                                  : table != "contracts" ? "day DESC, "
                                                         : "")
        + (table == "calendar" ? "exchange"
                : table == "warehouse_receipts"
                ? "product, warehouse, warehouse_id, warehouse_type, area, crop_year, grade, brand, origin, converted, unit"
                : table == "member_positions" ? "product, contract, member"
                : table == "inventory" ? "product, area, warehouse, warehouse_type, grade, unit"
                                       : "contract");
    if (where.empty()) {
        std::string files;
        std::uint64_t total = 0;
        std::uint64_t skipped = 0;
        // Daily reports have disjoint dates, so earlier pages need not be opened or sorted.
        for (const auto& [key, part] : version->partitions | std::views::reverse) {
            if (key != prefix && !key.starts_with(prefix + '/'))
                continue;
            if (daily_partitions && total + part.rows <= query.offset())
                skipped += part.rows;
            else if (total < std::uint64_t{query.offset()} + 100)
                files += (files.empty() ? "" : ",") + parquet::literal(store.file(*version, key));
            total += part.rows;
        }
        result.set_total(static_cast<std::uint32_t>(total));
        if (files.empty())
            return result;
        parquet::Engine engine;
        engine.run("CREATE TEMP VIEW t AS SELECT * FROM read_parquet([" + files + "])");
        const auto rows = engine.run(
            "SELECT * FROM t ORDER BY " + order_by + " LIMIT 100 OFFSET "
            + std::to_string(query.offset() - skipped));
        append_rows(result, *rows);
        return result;
    }
    std::vector<std::string> matched;
    const auto day_key = daily_partitions && !day.empty() ? prefix + '/' + std::string(day)
        : table == "calendar" && !day.empty() ? prefix + '/' + std::string(day.substr(0, 4))
                                              : std::string{};
    for (const auto& [key, part] : version->partitions | std::views::reverse) {
        if (key != prefix && !key.starts_with(prefix + '/'))
            continue;
        if (!day_key.empty() && key != day_key)
            continue;
        matched.push_back(key);
    }
    if (matched.empty())
        return result;
    parquet::Engine engine;
    // Daily partitions are disjoint and descending; scan in bounded batches to stay within the
    // read-lane memory limit while preserving exact totals and cross-batch pages.
    const std::size_t batch_size = daily_partitions ? 120 : matched.size();
    std::uint64_t total = 0;
    const std::uint64_t target_begin = query.offset();
    const std::uint64_t target_end = target_begin + 100;
    for (std::size_t at = 0; at < matched.size(); at += batch_size) {
        std::string batch_files;
        for (std::size_t i = at; i < std::min(matched.size(), at + batch_size); ++i)
            batch_files += (batch_files.empty() ? "" : ",")
                + parquet::literal(store.file(*version, matched[i]));
        engine.run(
            "CREATE OR REPLACE TEMP VIEW t AS SELECT * FROM read_parquet([" + batch_files + "])");
        const auto count =
            static_cast<std::uint64_t>(engine.run("SELECT COUNT(*) FROM t WHERE " + where)
                    ->GetValue(0, 0)
                    .GetValue<std::int64_t>());
        if (count > 0 && total + count > target_begin && total < target_end) {
            const auto local_offset = target_begin > total ? target_begin - total : 0;
            const auto need =
                std::min(target_end - std::max(target_begin, total), count - local_offset);
            const auto rows = engine.run(
                "SELECT * FROM t WHERE " + where + " ORDER BY " + order_by + " LIMIT "
                + std::to_string(need) + " OFFSET " + std::to_string(local_offset));
            append_rows(result, *rows);
        }
        total += count;
    }
    result.set_total(static_cast<std::uint32_t>(total));
    return result;
}

} // namespace asterion::warehouse
