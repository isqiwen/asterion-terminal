#pragma once
#include "history_archive.hpp"
#include <asterion/v1/data.pb.h>
namespace asterion::tasks {
// Per-contract trading-day coverage of the archive's minute and daily versions
// matching `filter`. Every file digest is verified before it is counted.
data::v1::HistoryCoverages history_coverage(const history_files::Archive&,
                                            const data::v1::HistoryFilter& filter);
} // namespace asterion::tasks
