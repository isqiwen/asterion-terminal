#include "research.hpp"

namespace asterion::market_data::warehouse {

Version compose_research(const std::filesystem::path& data)
{
    const auto table = [](const std::string& key) {
        return key.substr(0, key.find('/'));
    };
    Store tushare(source_directory(data, "tushare"), "tushare");
    Store recorder(source_directory(data, "recorder"), "recorder");
    Store research(source_directory(data, "research"), "research");
    const auto primary = tushare.current();
    auto writer = research.writer();
    writer.clear();
    for (const auto& [key, part] : primary.partitions)
        writer.adopt(key, tushare, primary);
    for (const auto* provider : {"shfe", "ine", "czce", "dce", "cffex", "gfex"}) {
        Store exchange(source_directory(data, provider), provider);
        const auto version = exchange.current();
        for (const auto& [key, part] : version.partitions)
            if (table(key) == "settlement")
                writer.adopt(key, exchange, version);
    }
    const auto recorded = recorder.current();
    for (const auto& [key, part] : recorded.partitions) {
        if (table(key) != "minutes" || primary.partitions.contains(key))
            continue;
        writer.adopt(key, recorder, recorded);
        const auto segments = "observations" + key.substr(key.find('/'));
        if (recorded.partitions.contains(segments))
            writer.adopt(segments, recorder, recorded);
    }
    return writer.commit();
}

} // namespace asterion::market_data::warehouse
