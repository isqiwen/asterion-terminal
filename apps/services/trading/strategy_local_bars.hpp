#pragma once
#include <asterion/database/sqlite/sqlite_database.hpp>
#include <asterion/futures/futures.hpp>
#include <asterion/strategy.pb.h>
#include <asterion/trading.pb.h>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace asterion::trading {

// The bars an ended instance received, in order. Its package, contracts and
// period must match; parameters, package version and weight limits may be updated.
std::vector<::strategy::Bars>
recorded_inputs(const std::filesystem::path& run, const ::strategy::Instance& configuration);

std::int64_t local_bar_retention(::common::Period period);
std::map<InstrumentId, std::string> cached_expiries(const std::filesystem::path& root);

std::vector<::strategy::Bars> load_local_bars(
    sqlite::Database& db,
    const ::strategy::Instance& configuration,
    bool allow_empty,
    const std::map<InstrumentId, std::string>& expiries = {});

void store_local_bars(
    sqlite::Database& db,
    std::map<std::tuple<int, std::string, std::string>, std::uint64_t>& write_counts,
    ::common::Period period,
    const ::strategy::Bars& input);

::trading::RecordedBars
query_recorded_bars(sqlite::Database& db, const ::trading::GetRecordedBars& query);

} // namespace asterion::trading
