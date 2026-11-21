#include "composition.hpp"
#include <map>
#include <memory>
#include <set>

namespace asterion::warehouse {
namespace {

bool same_price_ticks(const std::vector<Row>& stored, const std::vector<PriceTick>& expected)
{
    if (stored.size() != expected.size())
        return false;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto& row = stored[i];
        const auto& item = expected[i];
        if (row[0] != Cell{item.exchange} || row[1] != Cell{item.product} || row[2] != item.contract
            || row[3] != Cell{item.effective_from} || row[4] != Cell{item.effective_until}
            || !std::holds_alternative<std::string>(row[5])
            || Decimal::parse(std::get<std::string>(row[5])) != item.price_tick
            || row[6] != Cell{item.source_url})
            return false;
    }
    return true;
}

} // namespace

const std::vector<std::string>& sources_of(std::string_view table)
{
    static const std::map<std::string_view, std::vector<std::string>> order{
        {"contracts", {"tushare"}},
        {"calendar", {"tushare"}},
        {"daily", {"tushare", "shfe", "ine", "czce", "dce", "cffex", "gfex", "sina"}},
        {"minutes", {"tushare", "sina"}},
        {"settlement", {"shfe", "ine", "czce", "gfex", "tushare"}},
        {"price_ticks", {"verified", "czce", "gtja"}},
        {"warehouse_receipts", {"tushare"}},
        {"member_positions", {"shfe", "ine", "tushare"}},
        {"inventory", {"tushare"}},
    };
    return order.at(table);
}

SourceVersions source_versions(const std::filesystem::path& data)
{
    SourceVersions versions;
    for (const auto* table :
        {"contracts",
            "calendar",
            "daily",
            "minutes",
            "settlement",
            "price_ticks",
            "warehouse_receipts",
            "member_positions",
            "inventory"})
        for (const auto& provider : sources_of(table))
            if (!versions.contains(provider))
                versions[provider] =
                    Store(source_directory(data, provider), provider).current_identity();
    return versions;
}

Version compose(const std::filesystem::path& data, Store& unified, const SourceVersions& versions)
{
    auto writer = unified.writer();
    writer.clear();
    std::map<std::string, std::pair<std::unique_ptr<Store>, Version>> sources;
    for (const auto* table :
        {"contracts",
            "calendar",
            "daily",
            "minutes",
            "settlement",
            "price_ticks",
            "warehouse_receipts",
            "member_positions",
            "inventory"}) {
        std::set<std::string> taken;
        const auto prefix = std::string(table) + "/";
        for (const auto& provider : sources_of(table)) {
            auto& source = sources[provider];
            if (!source.first) {
                source.first = std::make_unique<Store>(source_directory(data, provider), provider);
                const auto& identity = versions.at(provider);
                source.second = identity.empty() ? Version{} : source.first->version(identity);
            }
            const auto& version = source.second;
            for (auto at = version.partitions.lower_bound(prefix);
                at != version.partitions.end() && at->first.starts_with(prefix);
                ++at)
                if (taken.insert(at->first).second)
                    writer.adopt(at->first, *source.first, version);
        }
    }
    return writer.commit();
}

std::string
publish_verified_price_ticks(const std::filesystem::path& data, std::span<const PriceTick> ticks)
{
    std::map<std::string, std::vector<PriceTick>> grouped;
    for (const auto& item : ticks)
        grouped[item.exchange].push_back(item);
    Store store(source_directory(data, "verified"), "verified");
    const auto current = store.current();
    bool unchanged = current.partitions.size() == grouped.size();
    for (const auto& [exchange, items] : grouped) {
        const auto key = "price_ticks/" + exchange;
        if (!unchanged || !current.partitions.contains(key)
            || !same_price_ticks(store.read(current, key), items)) {
            unchanged = false;
            break;
        }
    }
    if (unchanged)
        return current.identity;
    auto writer = store.writer();
    writer.clear();
    for (const auto& [exchange, items] : grouped)
        writer.put("price_ticks/" + exchange, rows(items));
    return writer.commit().identity;
}

} // namespace asterion::warehouse
