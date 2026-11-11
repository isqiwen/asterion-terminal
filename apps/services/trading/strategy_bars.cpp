#include "strategy_bars.hpp"
#include <asterion/futures/trading_days.hpp>
#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace asterion::trading {
namespace wire = ::strategy;

namespace {

std::int64_t minutes(::common::Period period)
{
    switch (period) {
        case ::common::PERIOD_MINUTES_1:
            return 1;
        case ::common::PERIOD_MINUTES_5:
            return 5;
        case ::common::PERIOD_MINUTES_10:
            return 10;
        case ::common::PERIOD_MINUTES_15:
            return 15;
        case ::common::PERIOD_MINUTES_30:
            return 30;
        case ::common::PERIOD_MINUTES_60:
            return 60;
        default:
            return 0;
    }
}

// How old an observation may be on arrival.
constexpr std::int64_t fresh_ms = 10000;

void put(::common::Decimal* field, const Decimal& value)
{
    field->set_text(value.str());
}

} // namespace

StrategyBars::StrategyBars(
    const wire::Instance& configuration,
    std::int64_t started_ms,
    std::uint64_t sequence)
    : daily_(configuration.period() == ::common::PERIOD_DAY),
      interval_(minutes(configuration.period()) * 60000),
      started_ms_(started_ms),
      sequence_(sequence)
{
    for (const auto& id : configuration.instruments()) {
        contracts_.push_back(id.exchange() + "/" + id.symbol());
        current_.emplace(contracts_.back(), Current{});
    }
}

std::vector<wire::Bars> StrategyBars::observe(const MarketQuote& quote)
{
    const auto contract = quote.instrument.venue + "/" + quote.instrument.symbol;
    auto found = current_.find(contract);
    if (found == current_.end())
        throw std::invalid_argument("strategy received an unselected contract");
    auto& current = found->second;
    // A new subscription can return cached observations from before this
    // instance started. Even a cache spanning a boundary cannot seed warmup.
    if (!current.initialized && quote.source_ms < started_ms_)
        return {};
    // Closed-session snapshots, stale observations and snapshots a reconnect
    // repeats are not new observations.
    if (quote.source_ms <= 0 || quote.received_ms - quote.source_ms > fresh_ms
        || (current.initialized && quote.source_ms < current.source_ms))
        return {};
    if (quote.source_ms - quote.received_ms > 2000)
        throw std::runtime_error("strategy market timestamp is ahead of the host clock");
    if (!quote.last || !quote.open_interest || quote.volume < 0 || *quote.last <= Decimal{})
        throw std::runtime_error(
            "strategy market observation lacks price, volume or open interest");
    // CTP labels the trading date without separators. Keep the research contract
    // explicit and independent of civil dates of night-session observations.
    if (quote.trading_day.size() != 8)
        throw std::runtime_error("strategy market observation has no trading date");
    const auto day = quote.trading_day.substr(0, 4) + "-" + quote.trading_day.substr(4, 2) + "-"
        + quote.trading_day.substr(6, 2);
    const auto date = std::chrono::sys_days(parse_trading_date(day));
    const auto key = daily_
        ? std::chrono::duration_cast<std::chrono::milliseconds>(date.time_since_epoch()).count()
            - 8 * 3600000LL
        : quote.source_ms / interval_ * interval_;
    now_ = std::max(now_, quote.source_ms);
    if (key <= delivered_)
        return deliver();
    if (daily_)
        opened_.try_emplace(key, quote.source_ms);
    const bool new_day = current.initialized && day != current.bar.trading_day();
    if (current.initialized
        && (key < current.key || day < current.bar.trading_day() || (new_day && key == current.key)
            || (!new_day && quote.volume < current.volume)))
        throw std::runtime_error("strategy market observations are unordered or volume reset");
    const auto delta = !current.initialized ? 0
        : new_day                           ? quote.volume
                                            : quote.volume - current.volume;
    if (!current.initialized || key != current.key) {
        if (current.complete)
            pending_[current.key].emplace(contract, current.bar);
        // The first observed bucket is always excluded, even if the first tick
        // happens to carry its boundary timestamp. Daily starts discard that day.
        const bool complete = current.initialized;
        current.bar.Clear();
        current.bar.mutable_instrument()->set_exchange(quote.instrument.venue);
        current.bar.mutable_instrument()->set_symbol(quote.instrument.symbol);
        current.bar.set_trading_day(day);
        // A day completes at its 15:00 Beijing close, as research labels it.
        current.bar.set_end_ms(daily_ ? key + 15 * 3600000LL : key + interval_);
        put(current.bar.mutable_open(), *quote.last);
        current.high = current.low = *quote.last;
        current.key = key;
        current.complete = complete;
        current.initialized = true;
    }
    auto& bar = current.bar;
    current.high = std::max(current.high, *quote.last);
    current.low = std::min(current.low, *quote.last);
    put(bar.mutable_high(), current.high);
    put(bar.mutable_low(), current.low);
    put(bar.mutable_close(), *quote.last);
    bar.set_volume(bar.volume() + delta);
    put(bar.mutable_open_interest(), *quote.open_interest);
    current.source_ms = quote.source_ms;
    current.volume = quote.volume;

    return deliver();
}

std::vector<wire::Bars> StrategyBars::deliver()
{
    std::vector<wire::Bars> result;
    while (true) {
        // The earliest period with a full bar, sealed or still open.
        auto key = pending_.empty() ? INT64_MAX : pending_.begin()->first;
        for (const auto& [_, current] : current_)
            if (current.complete)
                key = std::min(key, current.key);
        if (key == INT64_MAX)
            break;
        const auto later = opened_.upper_bound(key);
        const auto end = !daily_     ? key + interval_
            : later == opened_.end() ? INT64_MAX - fresh_ms
                                     : later->second;
        if (now_ < end + fresh_ms
            && std::ranges::any_of(
                current_,
                [key](const auto& item) {
                    return !item.second.initialized || item.second.key <= key;
                }))
            break;
        auto& rows = pending_[key];
        for (auto& [contract, current] : current_)
            if (current.complete && current.key == key) {
                rows.emplace(contract, current.bar);
                current.complete = false;
            }
        wire::Bars batch;
        batch.set_sequence(++sequence_);
        for (const auto& contract : contracts_)
            if (const auto row = rows.find(contract); row != rows.end())
                *batch.add_bars() = row->second;
        result.push_back(std::move(batch));
        pending_.erase(key);
        delivered_ = key;
        opened_.erase(opened_.begin(), opened_.upper_bound(key));
    }
    return result;
}

} // namespace asterion::trading
