#pragma once
#include <asterion/domain/daily_bars.hpp>
#include <filesystem>
#include <asterion/foundation/serialization.hpp>
#include <asterion/v1/data.pb.h>
#include <functional>
namespace asterion::history_files {
HistoricalDailyRange daily_range(const data::v1::DailyDownload&);
unsigned daily_work_units(const data::v1::DailyDownload&);
data::v1::DailyDownload daily_request(const Json&);
data::v1::DailyDownloadResult daily_result(const std::filesystem::path&);
void verify_daily_result(const data::v1::DailyDownload&, const data::v1::DailyDownloadResult&);
data::v1::DailyPage read_daily_page(const data::v1::DailyDownload&,
                                    const data::v1::DailyDownloadResult&,
                                    const data::v1::DailyPageQuery&);
struct DailyDatasetInfo {
  HistoricalDailyRange range;
  std::uint64_t rows = 0;
  unsigned pages = 0;
  bool complete = false;
  std::string manifest_sha256;
  std::int64_t acquired_at_ns = 0;
};
struct DailyDataset {
  DailyDatasetInfo info;
  HistorySemantics semantics;
  std::vector<HistoricalDailyBar> bars;
};
using DailyProgress = std::function<void(unsigned completed, unsigned total, std::uint64_t rows)>;
// Immutable source-date chunks; resume only identical requests. No credentials
// are stored here. A single writer excludes readers; completed readers coexist.
DailyDatasetInfo download_daily(HistoricalDailyPort&, const HistoricalDailyRange&,
                                const std::filesystem::path&, std::stop_token = {},
                                DailyProgress = {});
DailyDatasetInfo inspect_daily(const std::filesystem::path&);
// At most twenty years of one bar per date. Verify all hashes before returning.
DailyDataset read_daily(const std::filesystem::path&);
// Trading days present in a complete dataset; every page digest is checked.
std::vector<std::string> daily_trading_days(const std::filesystem::path&,
                                            const std::string& expected_revision);
} // namespace asterion::history_files
