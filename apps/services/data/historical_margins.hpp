#pragma once
#include "history_tables.hpp"
#include <asterion/data.pb.h>

namespace asterion::data {

proto::HistoricalMargins
historical_margins(const history_files::TableStore&, const proto::HistoricalMarginQuery&);

} // namespace asterion::data
