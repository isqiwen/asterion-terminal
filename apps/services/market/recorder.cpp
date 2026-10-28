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

constexpr auto minute_schema =
    "venue VARCHAR NOT NULL, symbol VARCHAR NOT NULL, start_ms BIGINT NOT NULL," " open DECIMAL(18,8) NOT NULL, high DECIMAL(18,8) NOT NULL, low DECIMAL(18,8) NOT NULL," " close DECIMAL(18,8) NOT NULL, volume BIGINT NOT NULL, average_price DECIMAL(18,8)," " open_interest DECIMAL(18,8), partial BOOLEAN NOT NULL";

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

void write_minutes(const fs::path& path, const std::vector<Recorder::Minute>& minutes)
{
    parquet::Engine engine;
    engine.run(std::string("CREATE TEMP TABLE minutes(") + minute_schema + ")");
    {
        duckdb::Appender appender(*engine.connection, "minutes");
        for (const auto& [instrument, bar] : minutes) {
            appender.BeginRow();
            appender.Append(duckdb::Value(instrument.venue));
            appender.Append(duckdb::Value(instrument.symbol));
            appender.Append<std::int64_t>(bar.start_ms);
            for (const auto* value : {&bar.open, &bar.high, &bar.low, &bar.close})
                appender.Append(price(*value));
            appender.Append<std::int64_t>(bar.volume);
            appender.Append(price(bar.average_price));
            appender.Append(price(bar.open_interest));
            appender.Append<bool>(bar.partial);
            appender.EndRow();
        }
        appender.Close();
    }
    parquet::publish(engine, "minutes", path);
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

void Recorder::observe(const MarketQuote& quote, bool out_of_order, const IntradayBars& minutes)
{
    if (quote.trading_day.size() != 8)
        return;
    // A quote of the next trading day replaces the contract's series: take its
    // last bars first.
    if (const auto* series = minutes.find(quote.instrument);
        series && series->trading_day != quote.trading_day)
        take(quote.instrument, *series);
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

void Recorder::poll(std::int64_t now_ms, const IntradayBars& minutes)
{
    while (!pending_.empty()
        && pending_.front().wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        collect();
    if (!flushed_ms_)
        flushed_ms_ = now_ms;
    if (due_ || now_ms - flushed_ms_ >= flush_interval_ms || rows_.size() >= flush_rows) {
        take_minutes(minutes, false);
        flush(now_ms);
    }
}

void Recorder::close(std::string_view reason, std::int64_t now_ms, const IntradayBars& minutes)
{
    interrupt(reason);
    take_minutes(minutes, true);
    flush(now_ms);
    while (!pending_.empty())
        collect();
}

void Recorder::take(const InstrumentId& id, const IntradaySeries& series)
{
    if (series.bars.empty())
        return;
    auto& mark = written_[id];
    if (mark.trading_day != series.trading_day) {
        // The next trading day completes the last bar of the previous one.
        if (mark.forming && mark.forming->start_ms > mark.start_ms)
            minutes_[mark.trading_day].push_back({id, *mark.forming});
        mark = {series.trading_day, 0, {}};
    }
    // Bars are immutable once a later bar exists; only the last still forms.
    auto first = series.bars.size() - 1;
    while (first > 0 && series.bars[first - 1].start_ms > mark.start_ms)
        --first;
    auto& day = minutes_[series.trading_day];
    for (auto i = first; i + 1 < series.bars.size(); ++i)
        day.push_back({id, series.bars[i]});
    if (first + 1 < series.bars.size())
        mark.start_ms = series.bars[series.bars.size() - 2].start_ms;
    mark.forming = series.bars.back();
}

void Recorder::take_minutes(const IntradayBars& minutes, bool final)
{
    for (const auto& id : minutes.changed_after(minutes_revision_))
        if (const auto* series = minutes.find(id))
            take(id, *series);
    minutes_revision_ = minutes.revision();
    if (!final)
        return;
    // Observation stops inside the forming minute.
    for (auto& [id, mark] : written_)
        if (mark.forming && mark.forming->start_ms > mark.start_ms) {
            mark.start_ms = mark.forming->start_ms;
            mark.forming->partial = true;
            minutes_[mark.trading_day].push_back({id, *std::exchange(mark.forming, std::nullopt)});
        }
}

void Recorder::flush(std::int64_t now_ms)
{
    flushed_ms_ = now_ms;
    due_ = false;
    if (rows_.empty() && changed_.empty() && minutes_.empty())
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
    for (const auto& [day, minutes] : minutes_)
        days[day];
    for (auto& [day, rows] : days) {
        auto bars = std::move(minutes_[day]);
        while (pending_.size() >= writes_behind)
            collect();
        char part[24];
        std::snprintf(part, sizeof(part), "%06llu", static_cast<unsigned long long>(++part_));
        const auto partition = "trading_day=" + day;
        auto ticks = rows.empty()
            ? fs::path{}
            : root_ / "ticks" / partition / (instance_ + "-" + part + ".parquet");
        auto minutes = bars.empty()
            ? fs::path{}
            : root_ / "minutes" / partition / (instance_ + "-" + part + ".parquet");
        auto segments = root_ / "segments" / partition / (instance_ + ".parquet");
        auto promise = std::make_shared<std::promise<Written>>();
        pending_.push_back(promise->get_future());
        writer_.submit([promise,
                           day,
                           instance = instance_,
                           ticks = std::move(ticks),
                           minutes = std::move(minutes),
                           bars = std::move(bars),
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
                if (!minutes.empty()) {
                    create_directories_durably(minutes.parent_path());
                    write_minutes(minutes, bars);
                    written.bytes += fs::file_size(minutes);
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
    minutes_.clear();
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
