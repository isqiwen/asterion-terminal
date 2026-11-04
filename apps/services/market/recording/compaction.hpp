#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

namespace asterion::recording {

// After the close, a trading day's tick and minute files (one per flush and
// instance) are merged into one file per exchange:
//
//   recordings/ticks/trading_day=<YYYYMMDD>/<exchange>.parquet
//   recordings/minutes/trading_day=<YYYYMMDD>/<exchange>.parquet
//
// Merging reads every Parquet file of the day, including earlier merged ones,
// so a quote that arrives after a merge is folded in by the next one. The
// merged files are written beside the originals under names readers ignore;
// only when their rows add up to the originals' does a journal record the
// swap. The originals are then deleted and the merged files take their
// names. A merge interrupted before its journal leaves the originals as they
// were; one interrupted after it is completed by the next start. Segment
// files are never merged.
struct Compacted
{
    std::string trading_day;
    std::uint64_t ticks = 0, minutes = 0, files_before = 0, files_after = 0;
};

// Whether a trading day (YYYYMMDD) has ended at `now`: days before today in
// Beijing, and today from 15:30 Beijing time on. A night session belongs to
// the next trading day, so it never ends a day early.
bool trading_ended(const std::string& trading_day, std::chrono::system_clock::time_point now);

// Trading days with files to merge whose trading has ended at `now`.
std::vector<std::string> compactable_days(
    const std::filesystem::path& recordings,
    std::chrono::system_clock::time_point now,
    std::stop_token stop = {});

Compacted compact_day(
    const std::filesystem::path& recordings,
    const std::string& trading_day,
    std::stop_token stop = {});

// Deletes the tick partitions of every recorded trading day but the latest
// `keep`, and returns those days. Minutes and segments are never deleted.
std::vector<std::string> prune_ticks(const std::filesystem::path& recordings, unsigned keep);

// Completes merges whose journal was written before a stop.
void finish_compactions(const std::filesystem::path& recordings);

} // namespace asterion::recording
