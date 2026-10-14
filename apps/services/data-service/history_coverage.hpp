#pragma once
#include "history_archive.hpp"
#include "history_tables.hpp"
#include <asterion/v1/data.pb.h>
#include <set>
namespace asterion::data {
// The days of a minute version against the market-wide daily table: what the
// table holds of them and, of those, the days the contract traded in the
// auction without a minute bar (all of them, ascending).
struct MinuteCoverage {
  std::string compared_first, compared_last;
  unsigned traded = 0;
  std::vector<std::string> uncovered;
};
MinuteCoverage minute_coverage(const history_files::TableStore&, const data::v1::DailyTable&,
                               const std::string& contract,
                               const std::set<std::string>& minute_days);
// Each minute version of the archive matching the query's filter, against the
// daily table it names. Every file digest is verified before it is counted.
data::v1::HistoryCoverages history_coverage(const history_files::Archive&,
                                            const history_files::TableStore&,
                                            const data::v1::HistoryCoverageQuery&);
} // namespace asterion::data
