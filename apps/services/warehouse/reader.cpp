#include "reader.hpp"
#include "freshness.hpp"
#include "parquet_engine.hpp"
#include <asterion/futures/trading_days.hpp>
#include <asterion/protocol/warehouse.hpp>
#include <asterion/base/error.hpp>
#include <algorithm>
#include <cctype>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <ranges>
#include <regex>
#include <sstream>
#include <set>
#include <tuple>

namespace asterion::warehouse {
namespace fs = std::filesystem;
namespace wire = ::warehouse;

namespace {

Store dataset(const fs::path& directory)
{
    return Store(source_directory(directory, "research"), "research");
}

struct FileStamp
{
    bool operator==(const FileStamp&) const = default;

    bool exists = false;
    fs::file_time_type mtime{};
    std::uintmax_t size = 0;
};

using EmptyStamp = std::vector<FileStamp>;

EmptyStamp empty_stamp(const fs::path& directory, std::initializer_list<const char*> tables)
{
    EmptyStamp result;
    result.reserve(tables.size());
    for (const auto* table : tables) {
        const auto path = empty_file(directory, table);
        std::error_code ec;
        const bool exists = fs::is_regular_file(path, ec);
        if (ec || !exists) {
            result.push_back({});
            continue;
        }
        const auto mtime = fs::last_write_time(path, ec);
        const auto size = ec ? 0 : fs::file_size(path, ec);
        result.push_back(ec ? FileStamp{} : FileStamp{true, mtime, size});
    }
    return result;
}

template <class Key, class Value>
struct BoundedCache
{
    std::optional<Value> get(const Key& key) const
    {
        for (const auto& [k, v] : entries)
            if (k == key)
                return v;
        return std::nullopt;
    }

    void put(Key key, Value value, std::size_t capacity)
    {
        for (auto& [k, v] : entries)
            if (k == key) {
                v = std::move(value);
                return;
            }
        if (entries.size() >= capacity)
            entries.pop_front();
        entries.emplace_back(std::move(key), std::move(value));
    }

    std::deque<std::pair<Key, Value>> entries;
};

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
            const auto slash = key.find('/', 6);
            const auto exchange = key.substr(6, slash - 6);
            if (included(exchange)) {
                auto& files = daily[exchange][key.substr(0, key.find('-'))];
                files += (files.empty() ? "" : ",") + parquet::literal(store.file(version, key));
                // A stored exchange-day partition already fetched that day; a contract absent
                // from it had no bar from the source rather than a missing acquisition gap.
                if (!part.days.empty())
                    for (const auto& day : part.days)
                        answered_daily[exchange].insert(day);
                else if (slash != std::string::npos && key.size() - slash - 1 == 10)
                    answered_daily[exchange].insert(key.substr(slash + 1));
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
    {
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
    if (!short_contracts.empty()) {
        parquet::Engine engine;
        for (const auto& [exchange, names] : short_contracts) {
            std::string files;
            for (const auto& [group, part] : daily.at(exchange))
                files += (files.empty() ? "" : ",") + part;
            const auto rows = engine.run(
                "SELECT DISTINCT contract, strftime(day, '%Y-%m-%d') FROM read_parquet([" + files
                + "]) WHERE contract IN (" + names + ")");
            for (duckdb::idx_t row = 0; row < rows->RowCount(); ++row)
                daily_days[rows->GetValue(0, row).ToString()].insert(
                    rows->GetValue(1, row).ToString());
        }
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
        const bool settled = (item.days() || item.empty_days()) && !item.gap_days()
            && !item.pending_days() && item.open_days() <= item.days() + item.empty_days();
        out.set_complete(out.complete() + (settled && item.days() > 0));
        out.set_listed_complete(out.listed_complete() + (settled && contract.listed()));
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

wire::DatasetCoverage build_coverage(const ProductCoverages& by_product)
{
    wire::DatasetCoverage result;
    for (const auto& [name, contracts] : by_product) {
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

} // namespace

// Content-addressed versions, heads, diffs, and derived summaries/coverages are immutable
// for a given (version, day, empty-keys stamp); caching them avoids rescanning Parquet and shards.
struct Reader::Cache
{
    using Key = std::tuple<std::string, std::string, EmptyStamp>;
    using CoverageEntry = std::pair<std::shared_ptr<const ProductCoverages>, wire::DatasetCoverage>;

    std::shared_ptr<const Version> version(const Store& store, const std::string& identity)
    {
        if (identity.empty())
            return std::make_shared<Version>(Version{{}, store.provider(), {}, 0, {}});
        require_identity(identity);
        {
            std::lock_guard lock(mutex);
            if (auto found = versions.get(identity))
                return *found;
        }
        auto loaded = std::make_shared<Version>(store.version(identity));
        std::lock_guard lock(mutex);
        versions.put(identity, loaded, 2);
        return loaded;
    }

    VersionHead head(const Store& store, const std::string& identity)
    {
        require_identity(identity);
        {
            std::lock_guard lock(mutex);
            if (auto found = heads.get(identity))
                return *found;
        }
        auto loaded = store.head(identity);
        std::lock_guard lock(mutex);
        heads.put(identity, loaded, 64);
        return loaded;
    }

    wire::DatasetChange
    change(const Store& store, const VersionHead& version, const VersionHead& base)
    {
        const std::pair key{version.identity, base.identity};
        {
            std::lock_guard lock(mutex);
            if (auto found = changes.get(key))
                return *found;
        }
        auto computed = change_of(store, version, base);
        std::lock_guard lock(mutex);
        changes.put(key, computed, 64);
        return computed;
    }

    std::mutex mutex;
    BoundedCache<std::string, std::shared_ptr<const Version>> versions;
    BoundedCache<std::string, VersionHead> heads;
    BoundedCache<std::pair<std::string, std::string>, wire::DatasetChange> changes;
    BoundedCache<Key, wire::DatasetSummary> summaries;
    BoundedCache<Key, CoverageEntry> coverages;
};

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
    const auto store = dataset(directory_);
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
    const auto store = dataset(directory_);
    return cache_->change(
        store,
        cache_->head(store, query.version()),
        cache_->head(store, query.base()));
}

namespace {

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
                "(effective_from <= " + date
                + " AND (effective_until IS NULL OR effective_until >= " + date + "))");
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
    const auto store = dataset(directory_);
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

wire::DatasetCoverage Reader::coverage(const wire::DatasetRef& query) const
{
    require_identity(query.version());
    const Cache::Key cache_key{
        query.version(),
        yesterday(),
        empty_stamp(directory_, {"daily", "minutes"})
    };
    {
        std::lock_guard lock(cache_->mutex);
        if (auto found = cache_->coverages.get(cache_key))
            return found->second;
    }
    const auto store = dataset(directory_);
    const auto version = cache_->version(store, query.version());
    auto by_product =
        std::make_shared<ProductCoverages>(contract_coverage(directory_, store, *version, ""));
    auto result = build_coverage(*by_product);
    std::lock_guard lock(cache_->mutex);
    cache_->coverages.put(cache_key, {std::move(by_product), result}, 2);
    return result;
}

wire::ProductContracts Reader::product_coverage(const wire::ProductRef& query) const
{
    require_identity(query.version());
    static const std::regex product("[A-Z]+/[a-zA-Z]{1,3}");
    if (!query.product().empty() && !std::regex_match(query.product(), product))
        throw std::invalid_argument("a product must be EXCHANGE/product");
    const Cache::Key cache_key{
        query.version(),
        yesterday(),
        empty_stamp(directory_, {"daily", "minutes"})
    };
    std::shared_ptr<const ProductCoverages> cached;
    {
        std::lock_guard lock(cache_->mutex);
        if (auto found = cache_->coverages.get(cache_key))
            cached = found->first;
    }
    wire::ProductContracts result;
    if (cached) {
        if (query.product().empty()) {
            for (const auto& [name, contracts] : *cached)
                for (const auto& contract : contracts)
                    *result.add_contracts() = contract;
        } else if (const auto at = cached->find(query.product()); at != cached->end()) {
            for (const auto& contract : at->second)
                *result.add_contracts() = contract;
        }
        return result;
    }
    const auto store = dataset(directory_);
    const auto version = cache_->version(store, query.version());
    if (query.product().empty()) {
        auto by_product =
            std::make_shared<ProductCoverages>(contract_coverage(directory_, store, *version, ""));
        auto summary = build_coverage(*by_product);
        for (const auto& [name, contracts] : *by_product)
            for (const auto& contract : contracts)
                *result.add_contracts() = contract;
        std::lock_guard lock(cache_->mutex);
        cache_->coverages.put(cache_key, {std::move(by_product), std::move(summary)}, 2);
        return result;
    }
    auto found = contract_coverage(directory_, store, *version, query.product());
    for (auto& [name, contracts] : found)
        for (auto& contract : contracts)
            *result.add_contracts() = std::move(contract);
    return result;
}

} // namespace asterion::warehouse
