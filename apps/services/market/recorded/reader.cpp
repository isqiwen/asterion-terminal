#include "reader.hpp"
#include "database.hpp"
#include "parquet_engine.hpp"
#include <asterion/futures/market.hpp>
#include <asterion/base/decimal.hpp>
#include <asterion/base/error.hpp>
#include <algorithm>
#include <map>
#include <set>
#include <regex>

namespace asterion::recording {
namespace {

namespace fs = std::filesystem;

void require_trading_day(const std::string& day)
{
    static const std::regex syntax("[0-9]{8}");
    if (!std::regex_match(day, syntax))
        throw Error(ErrorCode::invalid_request, "trading day must be YYYYMMDD");
}

bool has_parquet(const fs::path& folder)
{
    if (fs::is_directory(folder))
        for (const auto& file : fs::directory_iterator(folder))
            if (file.path().extension() == ".parquet" && file.is_regular_file())
                return true;
    return false;
}

} // namespace

Reader::Reader(fs::path recordings)
    : root_(std::move(recordings)),
      database_(open_database("256 MiB"))
{}

std::vector<std::string> Reader::days(const std::string& first, const std::string& last) const
{
    std::vector<std::string> out;
    if (!fs::exists(root_ / "segments"))
        return out;
    for (const auto& entry : fs::directory_iterator(root_ / "segments")) {
        const auto name = entry.path().filename().string();
        if (!name.starts_with("trading_day="))
            continue;
        const auto day = name.substr(12);
        if (day >= first && day <= last)
            out.push_back(day);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<Reader::Segment> Reader::segments(const std::string& day)
{
    require_trading_day(day);
    const auto folder = root_ / "segments" / ("trading_day=" + day);
    std::vector<Segment> out;
    if (!has_parquet(folder))
        return out;
    duckdb::Connection connection(*database_);
    const auto result = query(
        connection,
        "SELECT instance, segment, first_received_ms, last_received_ms, ticks, \"end\" FROM read_parquet("
            + parquet::literal(folder / "*.parquet")
            + ") ORDER BY first_received_ms, instance, segment");
    for (duckdb::idx_t row = 0; row < result->RowCount(); ++row)
        out.push_back(
            {result->GetValue(0, row).ToString(),
                static_cast<std::uint64_t>(result->GetValue(1, row).GetValue<std::int64_t>()),
                result->GetValue(2, row).GetValue<std::int64_t>(),
                result->GetValue(3, row).GetValue<std::int64_t>(),
                static_cast<std::uint64_t>(result->GetValue(4, row).GetValue<std::int64_t>()),
                result->GetValue(5, row).IsNull() ? std::string{}
                                                  : result->GetValue(5, row).ToString()});
    return out;
}

std::vector<MinuteBar> Reader::minutes(const std::string& trading_day, const InstrumentId& id)
{
    require_trading_day(trading_day);
    id.validate();
    std::vector<MinuteBar> out;
    const auto folder = root_ / "minutes" / ("trading_day=" + trading_day);
    if (!has_parquet(folder))
        return out;
    duckdb::Connection connection(*database_);
    const auto result = query(
        connection,
        "SELECT start_ms, open::VARCHAR, high::VARCHAR, low::VARCHAR, close::VARCHAR, volume," " average_price::VARCHAR, open_interest::VARCHAR, partial FROM read_parquet("
            + parquet::literal(folder / "*.parquet")
            + ") WHERE venue = $1 AND symbol = $2 ORDER BY start_ms",
        {duckdb::Value(id.venue), duckdb::Value(id.symbol)});
    const auto decimal = [&](duckdb::idx_t column, duckdb::idx_t row) -> std::optional<Decimal> {
        const auto value = result->GetValue(column, row);
        if (value.IsNull())
            return {};
        return Decimal::parse(value.ToString());
    };
    for (duckdb::idx_t row = 0; row < result->RowCount(); ++row)
        out.push_back(
            {result->GetValue(0, row).GetValue<std::int64_t>(),
                *decimal(1, row),
                *decimal(2, row),
                *decimal(3, row),
                *decimal(4, row),
                result->GetValue(5, row).GetValue<std::int64_t>(),
                decimal(6, row),
                decimal(7, row),
                result->GetValue(8, row).GetValue<bool>()});
    // A restart within a minute leaves two parts of it, one from each process.
    std::vector<MinuteBar> joined;
    for (auto& bar : out) {
        if (joined.empty() || joined.back().start_ms != bar.start_ms) {
            joined.push_back(std::move(bar));
            continue;
        }
        auto& whole = joined.back();
        whole.high = std::max(whole.high, bar.high);
        whole.low = std::min(whole.low, bar.low);
        whole.close = bar.close;
        whole.volume += bar.volume;
        if (bar.average_price)
            whole.average_price = bar.average_price;
        if (bar.open_interest)
            whole.open_interest = bar.open_interest;
        whole.partial = true;
    }
    return joined;
}

std::vector<InstrumentId> Reader::instruments(const std::string& trading_day)
{
    require_trading_day(trading_day);
    const auto folder = root_ / "minutes" / ("trading_day=" + trading_day);
    if (!has_parquet(folder))
        return {};
    duckdb::Connection connection(*database_);
    const auto result = query(
        connection,
        "SELECT DISTINCT venue, symbol FROM read_parquet(" + parquet::literal(folder / "*.parquet")
            + ") ORDER BY venue, symbol",
        {});
    std::vector<InstrumentId> out;
    for (duckdb::idx_t row = 0; row < result->RowCount(); ++row)
        out.push_back({result->GetValue(0, row).ToString(), result->GetValue(1, row).ToString()});
    return out;
}

} // namespace asterion::recording
