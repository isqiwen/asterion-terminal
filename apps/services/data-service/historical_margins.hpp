#pragma once
#include "history_tables.hpp"
#include <asterion/v1/data.pb.h>

namespace asterion::data {

v1::HistoricalMargins
historical_margins(const history_files::TableStore&, const v1::HistoricalMarginQuery&);

} // namespace asterion::data
