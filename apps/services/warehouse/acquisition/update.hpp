#pragma once
#include <asterion/warehouse.pb.h>
#include <filesystem>
#include <string>
#include <vector>

namespace asterion::warehouse::acquisition {

// The acquisitions that bring the current dataset to `yesterday`: the calendar
// of exchanges that lack yesterday's year, then each day-partitioned table for
// the exchanges (listed contracts, for minutes) that hold some of it but end
// before their last open day. Nothing for a table no one has acquired yet.
// Ids are `<prefix>-<table>`.
std::vector<::warehouse::StartAcquisition> update_requests(
    const std::filesystem::path& directory,
    const std::string& yesterday,
    const std::string& prefix);

} // namespace asterion::warehouse::acquisition
