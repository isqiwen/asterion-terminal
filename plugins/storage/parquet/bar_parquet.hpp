#pragma once
#include <asterion/domain/daily_bars.hpp>
#include <asterion/domain/historical_bars.hpp>
#include <filesystem>
#include <vector>
// Immutable Parquet files for downloaded bars, written and read with an
// embedded in-memory DuckDB. Prices are DECIMAL(18,8); volume, amount and open
// interest are DECIMAL(38,8): exactly the 8-place Decimal values, readable by
// any Parquet tool. Callers verify file digests; these functions validate the
// schema and every value.
namespace asterion::parquet {
// Writes a sibling temporary file, syncs it and renames it over `path`.
void write_minute_bars(const std::filesystem::path& path, const std::vector<HistoricalBar>& bars);
std::vector<HistoricalBar> read_minute_bars(const std::filesystem::path& path);
void write_daily_bars(const std::filesystem::path& path,
                      const std::vector<HistoricalDailyBar>& bars);
std::vector<HistoricalDailyBar> read_daily_bars(const std::filesystem::path& path);
// Distinct trading days ("YYYY-MM-DD", ascending) across verified minute or
// daily files, aggregated by DuckDB without materializing the bars.
std::vector<std::string> trading_days(const std::vector<std::filesystem::path>& files, bool minute);
} // namespace asterion::parquet
