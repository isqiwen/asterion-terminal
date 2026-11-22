#pragma once
#include <asterion/warehouse.pb.h>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace asterion::warehouse::acquisition {

// The acquisitions that bring the current dataset to `yesterday`: the calendar
// of exchanges that lack yesterday's year, then each day-partitioned table for
// the exchanges (listed contracts, for minutes) that hold some of it but end
// before their last open day, then the fundamental reports missing from the days
// each exchange has market bars for. Nothing for a table no one has acquired yet.
// Ids are `<prefix>-<table>`.
std::vector<::warehouse::StartAcquisition> update_requests(
    const std::filesystem::path& directory,
    const std::string& yesterday,
    const std::string& prefix);

// Refetches the contract table of `exchanges`, or with `missing_only` of those
// the current dataset has none for. Nothing when no exchange is left.
std::optional<::warehouse::StartAcquisition> contracts_request(
    const std::filesystem::path& directory,
    const std::vector<std::string>& exchanges,
    const std::string& day,
    const std::string& id,
    bool missing_only);

} // namespace asterion::warehouse::acquisition
