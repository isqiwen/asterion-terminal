#include "research.hpp"
#include <map>
#include <memory>
#include <set>

namespace asterion::history::warehouse {

const std::vector<std::string>& sources_of(std::string_view table)
{
    static const std::map<std::string_view, std::vector<std::string>> order{
        {"contracts", {"tushare"}},
        {"calendar", {"tushare"}},
        {"daily", {"tushare", "shfe", "ine", "czce", "dce", "cffex", "gfex", "sina"}},
        {"minutes", {"tushare", "sina"}},
        {"settlement", {"shfe", "ine", "czce"}},
        {"price_ticks", {"czce", "gtja"}},
    };
    return order.at(table);
}

Version compose_research(const std::filesystem::path& data, Store& research)
{
    // Sources are read under the writer's lock, so a later composition never
    // publishes older source versions than an earlier one.
    auto writer = research.writer();
    writer.clear();
    std::map<std::string, std::pair<std::unique_ptr<Store>, Version>> sources;
    for (const auto* table :
        {"contracts", "calendar", "daily", "minutes", "settlement", "price_ticks"}) {
        std::set<std::string> taken;
        const auto prefix = std::string(table) + "/";
        for (const auto& provider : sources_of(table)) {
            auto& source = sources[provider];
            if (!source.first) {
                source.first = std::make_unique<Store>(source_directory(data, provider), provider);
                source.second = source.first->current();
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

} // namespace asterion::history::warehouse
