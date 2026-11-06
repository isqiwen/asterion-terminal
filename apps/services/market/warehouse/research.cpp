#include "research.hpp"

namespace asterion::market_data::warehouse {

Version compose_research(const std::filesystem::path& data, Store& research)
{
    const auto table = [](const std::string& key) {
        return key.substr(0, key.find('/'));
    };
    // Sources are read under the writer's lock, so a later composition never
    // publishes older source versions than an earlier one.
    auto writer = research.writer();
    Store tushare(source_directory(data, "tushare"), "tushare");
    Store catalog(source_directory(data, "ctp"), "ctp");
    const auto primary = tushare.current();
    writer.clear();
    for (const auto& [key, part] : primary.partitions)
        writer.adopt(key, tushare, primary);
    for (const auto* provider : {"shfe", "ine", "czce", "dce", "cffex", "gfex", "gtja"}) {
        Store exchange(source_directory(data, provider), provider);
        const auto version = exchange.current();
        for (const auto& [key, part] : version.partitions)
            if (table(key) == "settlement" || table(key) == "price_ticks")
                writer.adopt(key, exchange, version);
    }
    // What CTP itself stated that day outranks a later report of it.
    const auto stated = catalog.current();
    for (const auto& [key, part] : stated.partitions)
        writer.adopt(key, catalog, stated);
    return writer.commit();
}

} // namespace asterion::market_data::warehouse
