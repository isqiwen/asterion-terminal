#pragma once
#include <asterion/futures/historical_bars.hpp>
#include <asterion/base/serialization.hpp>
#include <filesystem>
#include <asterion/data.pb.h>
#include <functional>

namespace asterion::history_files {

HistoricalBarRange minute_range(const data::proto::MinuteDownload&);
data::proto::MinuteDownloadResult minute_result(const std::filesystem::path&);
void verify_minute_result(
    const data::proto::MinuteDownload&,
    const data::proto::MinuteDownloadResult&);

struct MinutePageWork
{
    std::uint64_t macd_rows = 0;
    unsigned checkpoint_page = 0;
};

data::proto::MinutePage read_minute_page(
    const data::proto::MinuteDownload&,
    const data::proto::MinuteDownloadResult&,
    const data::proto::MinutePageQuery&,
    MinutePageWork* = nullptr);
using DownloadProgress =
    std::function<void(unsigned completed, unsigned total, std::uint64_t rows)>;
// Resumes only the identical dataset specification. Each bounded page is durable
// before the manifest advances. No credentials belong to this specification.
Json download_minutes(
    HistoricalBarPort&,
    const HistoricalBarRange&,
    const std::filesystem::path&,
    std::stop_token = {},
    DownloadProgress = {});
// All observed days of a completed download; every segment digest is checked.
// The service slices this result for presentation; no session dates are inferred.
data::proto::MinuteEvidence
minute_evidence(const std::filesystem::path&, const std::string& expected_revision);
Json inspect_minutes(const std::filesystem::path&);
// Validates every page/hash, only complete datasets are consumable by task.
void read_minutes(const std::filesystem::path&, const std::function<void(const HistoricalBar&)>&);

} // namespace asterion::history_files
