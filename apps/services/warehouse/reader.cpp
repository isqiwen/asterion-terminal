#include "reader.hpp"
#include "freshness.hpp"
#include "parquet_engine.hpp"
#include <asterion/protocol/warehouse.hpp>
#include <asterion/base/error.hpp>
#include <algorithm>
#include <fstream>
#include <map>
#include <ranges>
#include <regex>
#include <sstream>
#include <set>

namespace asterion::warehouse {
namespace fs = std::filesystem;
namespace wire = ::warehouse;

namespace {

Store dataset(const fs::path& directory)
{
    return Store(source_directory(directory, "research"), "research");
}

Version selected(const Store& store, const std::string& identity)
{
    return identity.empty() ? store.current() : store.version(identity);
}

wire::DatasetChange
change_of(const Store& store, const VersionHead& version, const VersionHead& base)
{
    const auto counts = store.compare(version, base);
    wire::DatasetChange out;
    out.mutable_dataset()->set_version(version.identity);
    out.mutable_dataset()->set_created_ms(version.created_ns / 1'000'000);
    out.set_base(base.identity);
    out.set_added(counts.added);
    out.set_replaced(counts.replaced);
    out.set_removed(counts.removed);
    for (const auto& table : counts.tables)
        out.add_tables(protocol::table_of(table));
    return out;
}

wire::Dataset dataset_of(const Version& version)
{
    wire::Dataset out;
    out.set_version(version.identity);
    out.set_created_ms(version.created_ns / 1'000'000);
    return out;
}

// What a contract holds of one table, before it is measured against its listed life.
struct Held
{
    std::string first, last;
    std::uint32_t days = 0;
};

struct Listing
{
    std::string list, delist;
    Held daily, minutes;
};

// `stored` is null when every open day from the first to the last stored one has bars.
void measure(
    wire::SeriesCoverage& out,
    const Held& held,
    const std::vector<std::string>* open,
    const std::string& start,
    const std::string& through,
    const std::set<std::string>* stored,
    const std::set<std::string>& answered)
{
    out.set_first_day(held.first);
    out.set_last_day(held.last);
    out.set_days(held.days);
    if (!open)
        return;
    const auto begin = std::lower_bound(open->begin(), open->end(), start);
    const auto end = std::upper_bound(begin, open->end(), through);
    out.set_open_days(static_cast<std::uint32_t>(end - begin));
    if (held.first.empty() && answered.empty())
        return;
    bool in_gap = false;
    for (auto at = begin; at != end; ++at) {
        const auto& day = *at;
        const bool inside = !held.first.empty() && day >= held.first && day <= held.last;
        if (inside && (!stored || stored->contains(day))) {
            in_gap = false;
        } else if (answered.contains(day)) {
            out.set_empty_days(out.empty_days() + 1);
            in_gap = false;
        } else if (held.first.empty() || day < held.first) {
            in_gap = false;
        } else if (day > held.last) {
            out.set_pending_days(out.pending_days() + 1);
        } else {
            out.set_gap_days(out.gap_days() + 1);
            if (!in_gap)
                out.add_gaps()->set_first(day);
            out.mutable_gaps(out.gaps_size() - 1)->set_last(day);
            in_gap = true;
        }
    }
}

// Contract names come from stored data.
std::string quoted(const std::string& text)
{
    std::string out = "'";
    for (const char c : text) {
        if (c == '\'')
            out += '\'';
        out += c;
    }
    return out + "'";
}

using ProductCoverages = std::map<std::string, std::vector<wire::ContractCoverage>>;

// Every contract of the version, or of one EXCHANGE/product, by product.
ProductCoverages contract_coverage(
    const fs::path& directory,
    const Store& store,
    const Version& version,
    const std::string& wanted)
{
    const auto exchange_of = [](const std::string& id) {
        return id.substr(0, id.find('/'));
    };
    const auto product_of = [](const std::string& id) {
        return id.substr(0, id.find('/', id.find('/') + 1));
    };
    const auto wanted_exchange = exchange_of(wanted);
    const auto included = [&](const std::string& exchange) {
        return wanted.empty() || exchange == wanted_exchange;
    };
    std::map<std::string, Listing> contracts;
    std::map<std::string, std::set<std::string>> minute_days, answered_minutes, answered_daily;
    // Daily files by exchange and year, so that one query never spans a whole history.
    std::map<std::string, std::map<std::string, std::string>> daily;
    std::string listed;
    for (const auto& [key, part] : version.partitions) {
        if (key.starts_with("minutes/")) {
            std::istringstream fields(key);
            std::string table, exchange, day, product, delivery;
            for (auto* field : {&table, &exchange, &day, &product, &delivery})
                std::getline(fields, *field, '/');
            const auto contract = exchange + '/' + product + '/' + delivery;
            if (wanted.empty() || product_of(contract) == wanted)
                minute_days[contract].insert(day);
        } else if (key.starts_with("daily/")) {
            const auto exchange = key.substr(6, key.find('/', 6) - 6);
            if (included(exchange)) {
                auto& files = daily[exchange][key.substr(0, key.find('-'))];
                files += (files.empty() ? "" : ",") + parquet::literal(store.file(version, key));
            }
        } else if (key.starts_with("contracts/") && included(key.substr(10))) {
            listed += (listed.empty() ? "" : ",") + parquet::literal(store.file(version, key));
        }
    }
    const auto only = wanted.empty() ? std::string{}
                                     : " WHERE exchange = " + quoted(wanted_exchange)
            + " AND product = " + quoted(wanted.substr(wanted.find('/') + 1));
    // Before the engine: this read takes the same one-query lane.
    const auto open = open_days(store, version, yesterday());
    parquet::Engine engine;
    if (!listed.empty()) {
        const auto rows = engine.run(
            "SELECT contract, strftime(list_date, '%Y-%m-%d'), strftime(delist_date, '%Y-%m-%d')" " FROM read_parquet(["
            + listed + "])" + only);
        for (duckdb::idx_t row = 0; row < rows->RowCount(); ++row) {
            auto& entry = contracts[rows->GetValue(0, row).ToString()];
            entry.list = rows->GetValue(1, row).ToString();
            entry.delist = rows->GetValue(2, row).ToString();
        }
    }
    for (const auto& [exchange, groups] : daily)
        for (const auto& [group, files] : groups) {
            const auto rows = engine.run(
                "SELECT contract, strftime(min(day), '%Y-%m-%d'), strftime(max(day), '%Y-%m-%d')," " count(DISTINCT day) FROM read_parquet(["
                + files + "])" + only + " GROUP BY contract");
            for (duckdb::idx_t row = 0; row < rows->RowCount(); ++row) {
                auto& held = contracts[rows->GetValue(0, row).ToString()].daily;
                const auto first = rows->GetValue(1, row).ToString();
                held.first = held.first.empty() ? first : std::min(held.first, first);
                held.last = std::max(held.last, rows->GetValue(2, row).ToString());
                held.days += rows->GetValue(3, row).GetValue<std::uint32_t>();
            }
        }
    for (const auto& [contract, days] : minute_days) {
        auto& held = contracts[contract].minutes;
        held.first = *days.begin();
        held.last = *days.rbegin();
        held.days = static_cast<std::uint32_t>(days.size());
    }
    for (const auto& key : empty_keys(directory, "minutes")) {
        std::istringstream fields(key);
        std::string table, exchange, day, product, delivery;
        for (auto* field : {&table, &exchange, &day, &product, &delivery})
            std::getline(fields, *field, '/');
        const auto contract = exchange + '/' + product + '/' + delivery;
        if (wanted.empty() || product_of(contract) == wanted) {
            answered_minutes[contract].insert(day);
            contracts[contract];
        }
    }
    // daily/EXCHANGE/YYYY-MM-DD
    for (const auto& key : empty_keys(directory, "daily"))
        answered_daily[key.substr(6, key.find('/', 6) - 6)].insert(key.substr(key.size() - 10));
    // Only a contract short of its stored span needs its days read.
    std::map<std::string, std::string> short_contracts;
    for (const auto& [contract, entry] : contracts) {
        const auto days = open.find(exchange_of(contract));
        if (!entry.daily.days || days == open.end())
            continue;
        const auto begin =
            std::lower_bound(days->second.begin(), days->second.end(), entry.daily.first);
        const auto end = std::upper_bound(begin, days->second.end(), entry.daily.last);
        if (entry.daily.days < static_cast<std::uint32_t>(end - begin)) {
            auto& names = short_contracts[exchange_of(contract)];
            names += (names.empty() ? "" : ",") + quoted(contract);
        }
    }
    std::map<std::string, std::set<std::string>> daily_days;
    for (const auto& [exchange, names] : short_contracts) {
        std::string files;
        for (const auto& [group, part] : daily.at(exchange))
            files += (files.empty() ? "" : ",") + part;
        const auto rows = engine.run(
            "SELECT DISTINCT contract, strftime(day, '%Y-%m-%d') FROM read_parquet([" + files
            + "]) WHERE contract IN (" + names + ")");
        for (duckdb::idx_t row = 0; row < rows->RowCount(); ++row)
            daily_days[rows->GetValue(0, row).ToString()].insert(rows->GetValue(1, row).ToString());
    }
    ProductCoverages result;
    for (const auto& [contract, entry] : contracts) {
        const auto exchange = exchange_of(contract);
        const auto days = open.find(exchange);
        const auto* calendar = days == open.end() || days->second.empty() ? nullptr : &days->second;
        const auto latest = calendar ? calendar->back() : yesterday();
        const auto through = entry.delist.empty() ? latest : std::min(latest, entry.delist);
        // Without a listing, a contract's life starts at its first stored or answered day.
        const auto start = [&](const Held& held, const std::set<std::string>* answered) {
            if (!entry.list.empty())
                return entry.list;
            if (!answered || answered->empty())
                return held.first.empty() ? through : held.first;
            return held.first.empty() ? *answered->begin()
                                      : std::min(held.first, *answered->begin());
        };
        wire::ContractCoverage item;
        item.set_contract(contract);
        item.set_list_day(entry.list);
        item.set_delist_day(entry.delist);
        item.set_listed(!entry.list.empty() && entry.list <= latest && latest <= entry.delist);
        const auto stored = daily_days.find(contract);
        measure(
            *item.mutable_daily(),
            entry.daily,
            calendar,
            start(entry.daily, nullptr),
            through,
            stored == daily_days.end() ? nullptr : &stored->second,
            answered_daily[exchange]);
        measure(
            *item.mutable_minutes(),
            entry.minutes,
            calendar,
            start(entry.minutes, &answered_minutes[contract]),
            through,
            &minute_days[contract],
            answered_minutes[contract]);
        result[product_of(contract)].push_back(std::move(item));
    }
    return result;
}

void summarize(
    wire::SeriesSummary& out,
    const std::vector<wire::ContractCoverage>& contracts,
    const wire::SeriesCoverage& (wire::ContractCoverage::*series)() const)
{
    std::vector<std::pair<std::string, std::string>> gaps;
    for (const auto& contract : contracts) {
        const auto& item = (contract.*series)();
        if (item.days()) {
            out.set_held(out.held() + 1);
            out.set_first_day(
                out.first_day().empty() ? item.first_day()
                                        : std::min(out.first_day(), item.first_day()));
            out.set_last_day(std::max(out.last_day(), item.last_day()));
        }
        const bool complete = item.days() && !item.gap_days() && !item.pending_days()
            && item.open_days() <= item.days() + item.empty_days();
        out.set_complete(out.complete() + complete);
        out.set_listed_complete(out.listed_complete() + (complete && contract.listed()));
        out.set_with_gaps(out.with_gaps() + (item.gaps_size() > 0));
        out.set_pending(out.pending() + (item.pending_days() > 0));
        for (const auto& gap : item.gaps())
            gaps.emplace_back(gap.first(), gap.last());
    }
    std::sort(gaps.begin(), gaps.end());
    for (const auto& [first, last] : gaps)
        if (out.gaps_size() && first <= out.gaps(out.gaps_size() - 1).last()) {
            auto* merged = out.mutable_gaps(out.gaps_size() - 1);
            merged->set_last(std::max(merged->last(), last));
        } else {
            auto* added = out.add_gaps();
            added->set_first(first);
            added->set_last(last);
        }
}

} // namespace

wire::DatasetVersion Reader::version(const wire::GetDatasetVersion& query) const
{
    if (!query.after_key().empty()) {
        require_key(query.after_key());
        require_identity(query.dataset().version());
    }
    const auto wanted = query.table() == wire::TABLE_UNSPECIFIED
        ? std::string{}
        : protocol::table_name(query.table());
    const auto store = dataset(directory_);
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
    const auto store = dataset(directory_);
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
    const auto store = dataset(directory_);
    const auto version = selected(store, query.version());
    wire::DatasetSummary result;
    *result.mutable_dataset() = dataset_of(version);
    const auto open = open_days(store, version, yesterday());
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
        if (std::string_view(table) == "calendar" || std::string_view(table) == "contracts"
            || std::string_view(table) == "price_ticks" || std::string_view(table) == "inventory")
            continue;
        for (const auto& [holder, holding] :
            holdings(store, version, table, open, empty_keys(directory_, table))) {
            item->set_missing(item->missing() + holding.missing);
            item->set_source_empty(item->source_empty() + holding.empty);
            item->set_expected_last_day(std::max(item->expected_last_day(), holding.through));
        }
    }
    return result;
}

wire::DatasetVersions Reader::versions(const wire::ListDatasetVersions& query) const
{
    const auto store = dataset(directory_);
    auto next = query.after_version().empty() ? store.current_identity()
                                              : store.head(query.after_version()).parent;
    wire::DatasetVersions result;
    while (!next.empty() && result.versions_size() < 20) {
        const auto version = store.head(next);
        const bool has_parent = !version.parent.empty() && store.has_version(version.parent);
        *result.add_versions() =
            change_of(store, version, has_parent ? store.head(version.parent) : VersionHead{});
        next = has_parent ? version.parent : std::string{};
    }
    if (!next.empty())
        result.set_next_version(result.versions().rbegin()->dataset().version());
    return result;
}

wire::DatasetChange Reader::change(const wire::CompareDatasetVersions& query) const
{
    const auto store = dataset(directory_);
    return change_of(store, store.head(query.version()), store.head(query.base()));
}

wire::TableRows Reader::rows(const wire::GetTableRows& query) const
{
    const auto table = protocol::table_name(query.table());
    if (table != "contracts" && table != "calendar" && table != "settlement"
        && table != "price_ticks" && table != "warehouse_receipts" && table != "member_positions"
        && table != "inventory")
        throw std::invalid_argument("only reference tables can be browsed");
    const auto store = dataset(directory_);
    const auto version = selected(store, query.dataset().version());
    wire::TableRows result;
    for (const auto& column : columns(table))
        result.add_columns(column.name);
    const auto prefix = table + '/' + query.exchange();
    std::string files;
    std::uint64_t total = 0;
    // Keys sort by period, so a page opens only the newest files it reaches.
    for (const auto& [key, part] : version.partitions | std::views::reverse) {
        if (key != prefix && !key.starts_with(prefix + '/'))
            continue;
        if (total < std::uint64_t{query.offset()} + 100)
            files += (files.empty() ? "" : ",") + parquet::literal(store.file(version, key));
        total += part.rows;
    }
    if (files.empty())
        return result;
    result.set_total(static_cast<std::uint32_t>(total));
    parquet::Engine engine;
    engine.run("CREATE TEMP VIEW t AS SELECT * FROM read_parquet([" + files + "])");
    const auto rows = engine.run(
        std::string("SELECT * FROM t ORDER BY ")
        + (table == "price_ticks"      ? "effective_from DESC, product, "
                : table != "contracts" ? "day DESC, "
                                       : "")
        + (table == "calendar" ? "exchange"
                : table == "warehouse_receipts"
                ? "product, warehouse, warehouse_id, area, crop_year, grade, brand, origin, converted, unit"
                : table == "member_positions" ? "product, contract, member"
                : table == "inventory"        ? "product, area, warehouse, grade, unit"
                                              : "contract")
        + " LIMIT 100 OFFSET " + std::to_string(query.offset()));
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
    const auto store = dataset(directory_);
    const auto version = store.version(query.version());
    wire::DatasetCoverage result;
    for (const auto& [name, contracts] : contract_coverage(directory_, store, version, "")) {
        auto* product = result.add_products();
        product->set_product(name);
        product->set_contracts(static_cast<std::uint32_t>(contracts.size()));
        product->set_listed(
            static_cast<std::uint32_t>(
                std::ranges::count_if(contracts, &wire::ContractCoverage::listed)));
        summarize(*product->mutable_daily(), contracts, &wire::ContractCoverage::daily);
        summarize(*product->mutable_minutes(), contracts, &wire::ContractCoverage::minutes);
    }
    return result;
}

wire::ProductContracts Reader::product_coverage(const wire::ProductRef& query) const
{
    require_identity(query.version());
    static const std::regex product("[A-Z]+/[a-zA-Z]{1,3}");
    if (!std::regex_match(query.product(), product))
        throw std::invalid_argument("a product must be EXCHANGE/product");
    const auto store = dataset(directory_);
    const auto version = store.version(query.version());
    wire::ProductContracts result;
    auto found = contract_coverage(directory_, store, version, query.product());
    for (auto& contract : found[query.product()])
        *result.add_contracts() = std::move(contract);
    return result;
}

} // namespace asterion::warehouse
