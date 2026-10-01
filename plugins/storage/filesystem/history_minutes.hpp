#pragma once
#include <asterion/domain/historical_bars.hpp>
#include <asterion/foundation/serialization.hpp>
#include <filesystem>
#include <asterion/v1/data.pb.h>
#include <functional>
namespace asterion::history_files {
HistoricalBarRange minute_range(const data::v1::MinuteDownload&);
Json minute_request_json(const data::v1::MinuteDownload&);
data::v1::MinuteDownload minute_request(const Json&);
data::v1::MinuteDownloadResult minute_result(const std::filesystem::path&);
void verify_minute_result(const data::v1::MinuteDownload&, const data::v1::MinuteDownloadResult&);
data::v1::MinutePage read_minute_page(const data::v1::MinuteDownload&,
                                      const data::v1::MinuteDownloadResult&,
                                      const data::v1::MinutePageQuery&);
using DownloadProgress =
    std::function<void(unsigned completed, unsigned total, std::uint64_t rows)>;
// Resumes only the identical dataset specification. Each bounded page is durable
// before the manifest advances. No credentials belong to this specification.
Json download_minutes(HistoricalBarPort&, const HistoricalBarRange&, const std::filesystem::path&,
                      unsigned requests_per_minute, std::stop_token = {}, DownloadProgress = {});
HistorySemantics minute_semantics(const std::filesystem::path&);
// Trading days present in a complete dataset; every segment digest is checked.
std::vector<std::string> minute_trading_days(const std::filesystem::path&);
Json inspect_minutes(const std::filesystem::path&);
// Validates every page/hash, only complete datasets are consumable by research.
void read_minutes(const std::filesystem::path&, const std::function<void(const HistoricalBar&)>&);
} // namespace asterion::history_files
