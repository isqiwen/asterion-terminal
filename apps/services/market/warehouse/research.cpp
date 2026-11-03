#include "research.hpp"
#include "parquet_engine.hpp"

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

dataset::proto::Comparison compare_minutes(const std::filesystem::path& data)
{
    const Store tushare(source_directory(data, "tushare"), "tushare");
    const Store recorder(source_directory(data, "recorder"), "recorder");
    const auto primary = tushare.current();
    const auto recorded = recorder.current();
    dataset::proto::Comparison result;
    result.set_tushare_version(primary.identity);
    result.set_recorder_version(recorded.identity);
    std::string left, right;
    for (const auto& [key, part] : recorded.partitions) {
        if (!key.starts_with("minutes/") || !primary.partitions.contains(key))
            continue;
        left += (left.empty() ? "" : ",") + parquet::literal(tushare.file(primary, key));
        right += (right.empty() ? "" : ",") + parquet::literal(recorder.file(recorded, key));
    }
    if (left.empty())
        return result;
    // Minutes match on contract, trading day and start; a recorded partial
    // minute may lack observations, so it is counted apart.
    const std::string same = "t.start_ms IS NOT NULL AND r.start_ms IS NOT NULL";
    parquet::Engine engine;
    const auto rows = engine.run(
        "WITH t AS (SELECT * FROM read_parquet([" + left
        + "]))," " r AS (SELECT * FROM read_parquet([" + right
        + "]))" " SELECT coalesce(t.contract, r.contract), coalesce(t.day, r.day)::VARCHAR," " count(*) FILTER (WHERE "
        + same + " AND NOT r.partial)," " count(*) FILTER (WHERE " + same
        + " AND NOT r.partial AND (t.open IS DISTINCT FROM r.open" " OR t.high IS DISTINCT FROM r.high OR t.low IS DISTINCT FROM r.low" " OR t.close IS DISTINCT FROM r.close OR t.volume IS DISTINCT FROM r.volume))," " count(*) FILTER (WHERE r.start_ms IS NULL)," " count(*) FILTER (WHERE t.start_ms IS NULL)," " count(*) FILTER (WHERE "
        + same
        + " AND r.partial)" " FROM t FULL JOIN r ON t.contract = r.contract AND t.day = r.day AND t.start_ms = r.start_ms" " GROUP BY 1, 2 ORDER BY 2 DESC, 1");
    result.set_days(static_cast<std::uint32_t>(rows->RowCount()));
    for (std::size_t row = 0; row < rows->RowCount(); ++row) {
        const auto count = [&](std::size_t column) {
            return static_cast<std::uint32_t>(rows->GetValue(column, row).GetValue<std::int64_t>());
        };
        if (!count(3) && !count(4) && !count(5))
            continue;
        result.set_differing_days(result.differing_days() + 1);
        if (result.differing_size() == 500)
            continue;
        auto* day = result.add_differing();
        day->set_contract(rows->GetValue(0, row).ToString());
        day->set_day(rows->GetValue(1, row).ToString());
        day->set_compared(count(2));
        day->set_differing(count(3));
        day->set_tushare_only(count(4));
        day->set_recorder_only(count(5));
        day->set_partial(count(6));
    }
    return result;
}

} // namespace asterion::market_data::warehouse
