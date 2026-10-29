#include "reader.hpp"
#include "database.hpp"
#include "parquet_engine.hpp"
#include <asterion/domain/market.hpp>
#include <asterion/foundation/decimal.hpp>
#include <asterion/foundation/error.hpp>
#include <map>
#include <set>
#include <regex>

namespace asterion::recording {
namespace {

namespace fs = std::filesystem;
namespace wire = recorder::proto;

const std::set<std::string> exchanges{"SHFE", "DCE", "CZCE", "CFFEX", "INE", "GFEX"};

std::string text(const duckdb::Value& value)
{
    return value.IsNull() ? std::string{} : Decimal::parse(value.ToString()).str();
}

std::string day_of(const fs::path& partition)
{
    const auto name = partition.filename().string();
    static const std::regex pattern("trading_day=([0-9]{8})");
    std::smatch match;
    return std::regex_match(name, match, pattern) ? match[1].str() : std::string{};
}

} // namespace

Reader::Reader(fs::path recordings)
    : root_(std::move(recordings)),
      database_(open_database("256 MiB"))
{}

wire::Days Reader::days()
{
    std::map<std::string, wire::Day> days;
    std::set<std::string> unmerged;
    for (const auto* table : {"ticks", "minutes"}) {
        const auto folder = root_ / table;
        if (!fs::is_directory(folder))
            continue;
        for (const auto& partition : fs::directory_iterator(folder)) {
            const auto day = day_of(partition.path());
            if (day.empty() || !partition.is_directory())
                continue;
            auto& entry = days[day];
            entry.set_trading_day(day);
            for (const auto& file : fs::directory_iterator(partition.path())) {
                if (file.path().extension() != ".parquet" || !file.is_regular_file())
                    continue;
                if (!exchanges.contains(file.path().stem().string()))
                    unmerged.insert(day);
                if (std::string_view(table) == "ticks")
                    entry.set_tick_files(entry.tick_files() + 1);
                else
                    entry.set_minute_files(entry.minute_files() + 1);
                entry.set_bytes(entry.bytes() + file.file_size());
            }
        }
    }
    if (fs::is_directory(root_ / "segments")) {
        duckdb::Connection connection(*database_);
        const auto result = query(
            connection,
            "SELECT trading_day::VARCHAR, instance, segment, first_received_ms," " last_received_ms, ticks, \"end\" FROM read_parquet("
                + parquet::literal(root_ / "segments" / "*" / "*.parquet")
                + ", hive_partitioning = true, hive_types = {'trading_day': VARCHAR})" " ORDER BY trading_day, first_received_ms");
        for (duckdb::idx_t row = 0; row < result->RowCount(); ++row) {
            const auto day = result->GetValue(0, row).ToString();
            auto& entry = days[day];
            entry.set_trading_day(day);
            auto* segment = entry.add_segments();
            segment->set_instance(result->GetValue(1, row).ToString());
            segment->set_number(result->GetValue(2, row).GetValue<std::int64_t>());
            segment->set_first_received_ms(result->GetValue(3, row).GetValue<std::int64_t>());
            segment->set_last_received_ms(result->GetValue(4, row).GetValue<std::int64_t>());
            segment->set_ticks(result->GetValue(5, row).GetValue<std::int64_t>());
            if (!result->GetValue(6, row).IsNull())
                segment->set_end(result->GetValue(6, row).ToString());
        }
    }
    wire::Days out;
    for (auto& [name, day] : days) {
        day.set_merged(!unmerged.contains(name) && (day.tick_files() || day.minute_files()));
        *out.add_days() = std::move(day);
    }
    return out;
}

wire::Minutes Reader::minutes(const wire::MinutesQuery& request)
{
    static const std::regex day("[0-9]{8}");
    if (!std::regex_match(request.trading_day(), day))
        throw Error(ErrorCode::invalid_request, "trading day must be YYYYMMDD");
    const InstrumentId id{request.venue(), request.symbol()};
    id.validate();
    wire::Minutes out;
    out.set_trading_day(request.trading_day());
    out.set_venue(id.venue);
    out.set_symbol(id.symbol);
    const auto folder = root_ / "minutes" / ("trading_day=" + request.trading_day());
    if (!fs::is_directory(folder))
        return out;
    duckdb::Connection connection(*database_);
    const auto result = query(
        connection,
        "SELECT start_ms, open::VARCHAR, high::VARCHAR, low::VARCHAR, close::VARCHAR, volume," " average_price::VARCHAR, open_interest::VARCHAR, partial FROM read_parquet("
            + parquet::literal(folder / "*.parquet")
            + ") WHERE venue = $1 AND symbol = $2 ORDER BY start_ms",
        {duckdb::Value(id.venue), duckdb::Value(id.symbol)});
    for (duckdb::idx_t row = 0; row < result->RowCount(); ++row) {
        auto* bar = out.add_bars();
        bar->set_start_ms(result->GetValue(0, row).GetValue<std::int64_t>());
        bar->set_open(text(result->GetValue(1, row)));
        bar->set_high(text(result->GetValue(2, row)));
        bar->set_low(text(result->GetValue(3, row)));
        bar->set_close(text(result->GetValue(4, row)));
        bar->set_volume(result->GetValue(5, row).GetValue<std::int64_t>());
        bar->set_average_price(text(result->GetValue(6, row)));
        bar->set_open_interest(text(result->GetValue(7, row)));
        bar->set_partial(result->GetValue(8, row).GetValue<bool>());
    }
    return out;
}

} // namespace asterion::recording
