#pragma once
#include "historical_sessions.hpp"

namespace asterion::tushare {

// The adapter declares bar_end, but neither a current exchange timetable nor
// trade_cal proves which no-trade minute rows this vendor emits historically.
inline historical_sessions::MinuteDefinition reviewed_minute_definition()
{
    return {
        "tushare.ft_mins",
        "tushare.minutes.v3",
        "adapter HistorySemantics bar_end",
        "",
        true,
        historical_sessions::Emission::unknown
    };
}

} // namespace asterion::tushare
