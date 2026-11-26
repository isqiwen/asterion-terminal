#pragma once
#include "live_session.hpp"
#include "strategy_bars.hpp"
#include <asterion/base/decimal.hpp>
#include <asterion/futures/futures.hpp>
#include <asterion/strategy.pb.h>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace asterion::trading {

struct LiveSeries
{
    struct DayStats
    {
        std::optional<Decimal> open_interest;
        std::optional<Decimal> prev_open_interest;
        std::optional<Decimal> prev_settlement;
    };

    std::string symbol;
    std::vector<InstrumentId> candidates;
    std::vector<std::string> delivery_months;
    std::vector<std::string> expiry_days;
    std::size_t active_index = 0;
    std::string decided_day;
    std::optional<Decimal> pending_roll_ratio;
    std::vector<std::map<std::string, DayStats>> history;
    std::vector<std::optional<Decimal>> prev_bar_close;
};

std::optional<Decimal>
prev_open_interest(const LiveSeries& series, std::size_t index, const std::string& day);

Decimal series_roll_ratio(const LiveSeries& series, std::size_t next, const std::string& day);

std::optional<std::size_t> choose_dominant(
    const LiveSeries& series,
    const std::string& day,
    bool had_history,
    const ::strategy::Bars* raw = nullptr);

std::optional<std::size_t> choose_sub_dominant(
    const LiveSeries& series,
    const std::string& day,
    const ::strategy::Bars* raw = nullptr);

std::set<std::string> awaited_series_contracts(
    const ::strategy::Instance& instance,
    const std::vector<LiveSeries>& live_series,
    const std::string& day = {});

void observe_series_quote(
    const ::strategy::Instance& instance,
    std::vector<LiveSeries>& live_series,
    StrategyBars* bars,
    std::string& last_awaited_day,
    const MarketQuote& observed);

void record_series_step_bars(std::vector<LiveSeries>& live_series, const ::strategy::Bars& input);

::strategy::Bars filter_and_roll_bars(
    const ::strategy::Instance& instance,
    std::vector<LiveSeries>& live_series,
    LiveSession* account,
    const StrategyAuthorityPtr& authority,
    StrategyBars* bars,
    const ::strategy::Bars& raw,
    const std::function<void(const std::string&)>& on_roll_event);

void roll_decision_for_live_day(
    const std::vector<LiveSeries>& live_series,
    LiveSession* account,
    const StrategyAuthorityPtr& authority,
    ::strategy::Decision& exec_decision);

} // namespace asterion::trading
