#pragma once
#include "history_archive.hpp"
#include "history_tables.hpp"
#include <asterion/v1/data.pb.h>
#include <map>
#include <optional>
namespace asterion::data {
struct MinuteReferenceDays {
  std::map<std::string, std::optional<bool>> calendar, traded;
};
MinuteReferenceDays minute_reference_days(const history_files::TableStore&,
                                          const data::v1::DailyTable&, const std::string& contract,
                                          const std::string& begin, const std::string& end);
data::v1::MinuteRangeEvidence minute_range_evidence(const history_files::Archive&,
                                                    const history_files::TableStore&,
                                                    const data::v1::MinuteRangeQuery&);
// The days of a minute version against the market-wide daily table: what the
// table holds of them and, of those, the days the contract traded in the
// auction without a minute bar (all of them, ascending).
struct MinuteCoverage {
  std::string compared_first, compared_last;
  unsigned traded = 0;
  std::vector<std::string> uncovered;
};
MinuteCoverage minute_coverage(const history_files::TableStore&, const data::v1::DailyTable&,
                               const std::string& contract, const data::v1::MinuteEvidence&);
data::v1::MinuteEvidence minute_evidence(const history_files::Archive&,
                                         const data::v1::MinuteEvidenceQuery&);
// Each minute version of the archive matching the query's filter, against the
// daily table it names. Every file digest is verified before it is counted.
data::v1::HistoryCoverages history_coverage(const history_files::Archive&,
                                            const history_files::TableStore&,
                                            const data::v1::HistoryCoverageQuery&);
} // namespace asterion::data
