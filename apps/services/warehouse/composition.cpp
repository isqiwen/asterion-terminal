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
        {"warehouse_receipts", {"shfe", "ine", "tushare"}},
        {"member_positions", {"shfe", "ine", "gfex", "tushare"}},
        {"inventory", {"shfe", "tushare"}},
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

Version compose(
    const std::filesystem::path& data,
    Store& unified,
    const SourceVersions& versions,
    SourceCache& sources)
{
    auto writer = unified.writer();
    writer.clear();
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
            if (!source.store)
                source.store = std::make_unique<Store>(source_directory(data, provider), provider);
            const auto& identity = versions.at(provider);
            if (source.version.identity != identity) {
                if (identity.empty()) {
                    source.head = {};
                    source.version = {};
                } else if (source.head.identity.empty()) {
                    auto next_head = source.store->head(identity);
                    Version next_version{
                        next_head.identity,
                        provider,
                        next_head.parent,
                        next_head.created_ns,
                        {}
                    };
                    for (const auto& [name, digest] : next_head.shards)
                        next_version.partitions.merge(source.store->shard(name, digest));
                    source.head = std::move(next_head);
                    source.version = std::move(next_version);
                } else {
                    auto next_head = source.store->head(identity);
                    std::vector<std::map<std::string, Partition>> loaded;
                    for (const auto& [name, digest] : next_head.shards) {
                        const auto found = source.head.shards.find(name);
                        if (found == source.head.shards.end() || found->second != digest)
                            loaded.push_back(source.store->shard(name, digest));
                    }
                    for (const auto& [name, digest] : source.head.shards) {
                        const auto found = next_head.shards.find(name);
                        if (found == next_head.shards.end() || found->second != digest) {
                            for (auto at = source.version.partitions.lower_bound(name);
                                at != source.version.partitions.end()
                                && at->first.starts_with(name);)
                                at = shard_of(at->first) == name
                                    ? source.version.partitions.erase(at)
                                    : std::next(at);
                        }
                    }
                    for (auto& shard_parts : loaded)
                        source.version.partitions.merge(shard_parts);
                    source.head = std::move(next_head);
                    source.version.identity = source.head.identity;
                    source.version.provider = provider;
                    source.version.parent = source.head.parent;
                    source.version.created_ns = source.head.created_ns;
                }
            }
            const auto& version = source.version;
            for (auto at = version.partitions.lower_bound(prefix);
                at != version.partitions.end() && at->first.starts_with(prefix);
                ++at)
                if (taken.insert(at->first).second)
                    writer.adopt(at->first, *source.store, version);
        }
    }
    return writer.commit();
}

Version compose(const std::filesystem::path& data, Store& unified, const SourceVersions& versions)
{
    SourceCache sources;
    return compose(data, unified, versions, sources);
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
