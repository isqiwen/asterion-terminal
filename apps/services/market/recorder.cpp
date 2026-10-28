#include "recorder.hpp"
#include "parquet_engine.hpp"
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/logger.hpp>
#include <cstdio>

namespace asterion::market_data {
namespace {

namespace fs = std::filesystem;
constexpr std::int64_t flush_interval_ms = 30'000;
constexpr std::size_t flush_rows = 200'000;
constexpr std::size_t writes_behind = 4;

// Prices are DECIMAL(18,8): exactly Decimal::raw(), readable by any Parquet tool.
constexpr auto
    tick_schema = "segment BIGINT NOT NULL, received_ms BIGINT NOT NULL, source_ms BIGINT," " venue VARCHAR NOT NULL, symbol VARCHAR NOT NULL, action_day VARCHAR NOT NULL," " update_time VARCHAR NOT NULL, out_of_order BOOLEAN NOT NULL," " last DECIMAL(18,8), volume BIGINT NOT NULL, open_interest DECIMAL(18,8)," " average_price DECIMAL(18,8)," " bid DECIMAL(18,8), bid_quantity BIGINT NOT NULL," " ask DECIMAL(18,8), ask_quantity BIGINT NOT NULL," " bid_price_2 DECIMAL(18,8), bid_quantity_2 BIGINT," " bid_price_3 DECIMAL(18,8), bid_quantity_3 BIGINT," " bid_price_4 DECIMAL(18,8), bid_quantity_4 BIGINT," " bid_price_5 DECIMAL(18,8), bid_quantity_5 BIGINT," " ask_price_2 DECIMAL(18,8), ask_quantity_2 BIGINT," " ask_price_3 DECIMAL(18,8), ask_quantity_3 BIGINT," " ask_price_4 DECIMAL(18,8), ask_quantity_4 BIGINT," " ask_price_5 DECIMAL(18,8), ask_quantity_5 BIGINT," " open DECIMAL(18,8), high DECIMAL(18,8), low DECIMAL(18,8)," " upper_limit DECIMAL(18,8), lower_limit DECIMAL(18,8)," " previous_settlement DECIMAL(18,8), previous_close DECIMAL(18,8)";

constexpr auto segment_schema =
    "instance VARCHAR NOT NULL, segment BIGINT NOT NULL, first_received_ms BIGINT NOT NULL," " last_received_ms BIGINT NOT NULL, ticks BIGINT NOT NULL, \"end\" VARCHAR";

duckdb::Value price(const std::optional<Decimal>& value)
{
    return value ? duckdb::Value::DECIMAL(value->raw(), 18, 8)
                 : duckdb::Value(duckdb::LogicalType::DECIMAL(18, 8));
}

duckdb::Value quantity(const std::optional<std::int64_t>& value)
{
    return value ? duckdb::Value::BIGINT(*value) : duckdb::Value(duckdb::LogicalType::BIGINT);
}

void write_ticks(const fs::path& path, const std::vector<Recorder::Row>& rows)
{
    parquet::Engine engine;
    engine.run(std::string("CREATE TEMP TABLE ticks(") + tick_schema + ")");
    {
        duckdb::Appender appender(*engine.connection, "ticks");
        for (const auto& [segment, out_of_order, q] : rows) {
            appender.BeginRow();
            appender.Append<std::int64_t>(static_cast<std::int64_t>(segment));
            appender.Append<std::int64_t>(q.received_ms);
            appender.Append(
                q.source_ms ? duckdb::Value::BIGINT(q.source_ms)
                            : duckdb::Value(duckdb::LogicalType::BIGINT));
            appender.Append(duckdb::Value(q.instrument.venue));
            appender.Append(duckdb::Value(q.instrument.symbol));
            appender.Append(duckdb::Value(q.action_day));
            appender.Append(duckdb::Value(q.update_time));
            appender.Append<bool>(out_of_order);
            appender.Append(price(q.last));
            appender.Append<std::int64_t>(q.volume);
            appender.Append(price(q.open_interest));
            appender.Append(price(q.average_price));
            appender.Append(price(q.bid));
            appender.Append<std::int64_t>(q.bid_quantity);
            appender.Append(price(q.ask));
            appender.Append<std::int64_t>(q.ask_quantity);
            for (const auto* levels : {&q.bid_levels, &q.ask_levels})
                for (const auto& level : *levels) {
                    appender.Append(price(level.price));
                    appender.Append(quantity(level.quantity));
                }
            for (const auto* value :
                {&q.open,
                    &q.high,
                    &q.low,
                    &q.upper_limit,
                    &q.lower_limit,
                    &q.previous_settlement,
                    &q.previous_close})
                appender.Append(price(*value));
            appender.EndRow();
        }
        appender.Close();
    }
    parquet::publish(engine, "ticks", path);
}

void write_segments(
    const fs::path& path,
    const std::string& instance,
    const std::vector<Recorder::Segment>& segments)
{
    parquet::Engine engine;
    engine.run(std::string("CREATE TEMP TABLE segments(") + segment_schema + ")");
    {
        duckdb::Appender appender(*engine.connection, "segments");
        for (const auto& segment : segments) {
            appender.BeginRow();
            appender.Append(duckdb::Value(instance));
            appender.Append<std::int64_t>(static_cast<std::int64_t>(segment.number));
            appender.Append<std::int64_t>(segment.first_ms);
            appender.Append<std::int64_t>(segment.last_ms);
            appender.Append<std::int64_t>(static_cast<std::int64_t>(segment.ticks));
            appender.Append(
                segment.end.empty() ? duckdb::Value(duckdb::LogicalType::VARCHAR)
                                    : duckdb::Value(segment.end));
            appender.EndRow();
        }
        appender.Close();
    }
    parquet::publish(engine, "segments", path);
}

} // namespace

Recorder::Recorder(fs::path root, std::string instance)
    : root_(std::move(root)),
      instance_(std::move(instance))
{}

Recorder::~Recorder()
{
    try {
        while (!pending_.empty())
            collect();
    } catch (...) {
        // Destruction never throws; close() reports a failed write.
    }
}

void Recorder::observe(const MarketQuote& quote, bool out_of_order)
{
    if (quote.trading_day.size() != 8)
        return;
    auto& day = segments_[quote.trading_day];
    if (day.empty() || day.back().number != segment_)
        day.push_back({segment_, quote.received_ms, quote.received_ms, 0, {}});
    auto& segment = day.back();
    segment.last_ms = quote.received_ms;
    ++segment.ticks;
    changed_.insert(quote.trading_day);
    observed_ = true;
    rows_.push_back({segment_, out_of_order, quote});
}

void Recorder::interrupt(std::string_view reason)
{
    if (!observed_)
        return;
    for (auto& [day, segments] : segments_)
        if (!segments.empty() && segments.back().number == segment_) {
            segments.back().end = reason;
            changed_.insert(day);
        }
    ++segment_;
    observed_ = false;
    // What a segment observed is on disk soon after it ends.
    due_ = true;
}

void Recorder::poll(std::int64_t now_ms)
{
    while (!pending_.empty()
        && pending_.front().wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        collect();
    if (!flushed_ms_)
        flushed_ms_ = now_ms;
    if (due_ || now_ms - flushed_ms_ >= flush_interval_ms || rows_.size() >= flush_rows)
        flush(now_ms);
}

void Recorder::close(std::string_view reason, std::int64_t now_ms)
{
    interrupt(reason);
    flush(now_ms);
    while (!pending_.empty())
        collect();
}

void Recorder::flush(std::int64_t now_ms)
{
    flushed_ms_ = now_ms;
    due_ = false;
    if (rows_.empty() && changed_.empty())
        return;
    std::map<std::string, std::vector<Row>> days;
    for (auto& row : rows_) {
        auto day = row.quote.trading_day;
        days[day].push_back(std::move(row));
    }
    rows_.clear();
    for (const auto& day : changed_)
        days[day];
    changed_.clear();
    for (auto& [day, rows] : days) {
        while (pending_.size() >= writes_behind)
            collect();
        char part[24];
        std::snprintf(part, sizeof(part), "%06llu", static_cast<unsigned long long>(++part_));
        const auto partition = "trading_day=" + day;
        auto ticks = rows.empty()
            ? fs::path{}
            : root_ / "ticks" / partition / (instance_ + "-" + part + ".parquet");
        auto segments = root_ / "segments" / partition / (instance_ + ".parquet");
        auto promise = std::make_shared<std::promise<Written>>();
        pending_.push_back(promise->get_future());
        writer_.submit([promise,
                           day,
                           instance = instance_,
                           ticks = std::move(ticks),
                           segments = std::move(segments),
                           listed = segments_[day],
                           rows = std::move(rows)](std::stop_token) {
            Written written{day, {}, rows.size(), 0, 0};
            try {
                if (!ticks.empty()) {
                    create_directories_durably(ticks.parent_path());
                    write_ticks(ticks, rows);
                    written.bytes += fs::file_size(ticks);
                    ++written.files;
                }
                create_directories_durably(segments.parent_path());
                write_segments(segments, instance, listed);
            } catch (const std::exception& error) {
                written.error = error.what();
            }
            promise->set_value(std::move(written));
        });
    }
}

void Recorder::collect()
{
    auto written = pending_.front().get();
    pending_.pop_front();
    if (!written.error.empty()) {
        status_.error = written.error;
        status_.lost += written.rows;
        log_process_event(
            "market-data",
            LogLevel::error,
            "recording.write_failed",
            {{"trading_day", written.trading_day},
                {"rows", written.rows},
                {"message", written.error}});
        return;
    }
    status_.trading_day = written.trading_day;
    status_.rows += written.rows;
    status_.files += written.files;
    status_.bytes += written.bytes;
    status_.written_ms = flushed_ms_;
}

} // namespace asterion::market_data
