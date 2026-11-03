#include "research.hpp"
#include "parquet_engine.hpp"

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
    Store recorder(source_directory(data, "recorder"), "recorder");
    const auto primary = tushare.current();
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

dataset::proto::Differences
minute_differences(const std::filesystem::path& data, const dataset::proto::DifferencesQuery& query)
{
    const auto slash = query.contract().find('/');
    if (slash == std::string::npos)
        throw std::invalid_argument("contract must be EXCHANGE/product/YYYY-MM");
    const auto key = "minutes/" + query.contract().substr(0, slash) + "/" + query.day() + "/"
        + query.contract().substr(slash + 1);
    require_key(key);
    const Store tushare(source_directory(data, "tushare"), "tushare");
    const Store recorder(source_directory(data, "recorder"), "recorder");
    const auto primary = tushare.current();
    const auto recorded = recorder.current();
    dataset::proto::Differences result;
    result.set_contract(query.contract());
    result.set_day(query.day());
    if (!primary.partitions.contains(key) || !recorded.partitions.contains(key))
        return result;
    const auto values = [](const char* side) {
        std::string columns;
        for (const auto* name : {"open", "high", "low", "close", "volume"})
            columns += std::string(", ") + side + "." + name + "::VARCHAR";
        return columns;
    };
    parquet::Engine engine;
    const auto rows = engine.run(
        "WITH t AS (SELECT * FROM read_parquet(" + parquet::literal(tushare.file(primary, key))
        + ")), r AS (SELECT * FROM read_parquet(" + parquet::literal(recorder.file(recorded, key))
        + ")) SELECT coalesce(t.start_ms, r.start_ms), t.start_ms IS NOT NULL," " r.start_ms IS NOT NULL"
        + values("t") + values("r")
        + " FROM t FULL JOIN r ON t.start_ms = r.start_ms WHERE r.partial IS NOT TRUE" " AND (t.start_ms IS NULL OR r.start_ms IS NULL OR t.open IS DISTINCT FROM r.open" " OR t.high IS DISTINCT FROM r.high OR t.low IS DISTINCT FROM r.low" " OR t.close IS DISTINCT FROM r.close OR t.volume IS DISTINCT FROM r.volume)" " ORDER BY 1");
    for (std::size_t row = 0; row < rows->RowCount(); ++row) {
        auto* minute = result.add_minutes();
        minute->set_start_ms(rows->GetValue(0, row).GetValue<std::int64_t>());
        const auto fill = [&](dataset::proto::MinuteValues* side, std::size_t first) {
            const auto text = [&](std::size_t column) {
                const auto cell = rows->GetValue(first + column, row);
                return cell.IsNull() ? std::string{} : Decimal::parse(cell.ToString()).str();
            };
            side->set_open(text(0));
            side->set_high(text(1));
            side->set_low(text(2));
            side->set_close(text(3));
            side->set_volume(text(4));
        };
        if (rows->GetValue(1, row).GetValue<bool>())
            fill(minute->mutable_tushare(), 3);
        if (rows->GetValue(2, row).GetValue<bool>())
            fill(minute->mutable_recorder(), 8);
    }
    return result;
}

} // namespace asterion::market_data::warehouse
