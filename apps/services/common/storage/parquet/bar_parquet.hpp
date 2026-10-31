#pragma once
#include <asterion/futures/historical_bars.hpp>
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

struct MinuteDayEvidence
{
    std::string trading_day; // Empty means the provider supplied no trading day.
    std::uint64_t rows;
    std::int64_t first_ns, last_ns;
};

// Observations grouped by provider-assigned trading day, ascending. These
// counts and timestamp bounds do not establish session completeness.
std::vector<MinuteDayEvidence> minute_evidence(const std::vector<std::filesystem::path>& files);

} // namespace asterion::parquet
