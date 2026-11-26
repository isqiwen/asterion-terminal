#include "reader_cache.hpp"
#include "parquet_engine.hpp"
#include <algorithm>
#include <ranges>
#include <regex>
#include <set>
#include <sstream>

namespace asterion::warehouse {
namespace fs = std::filesystem;
namespace wire = ::warehouse;

namespace {

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
    const auto store = research_dataset(directory_);
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
    const auto store = research_dataset(directory_);
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
