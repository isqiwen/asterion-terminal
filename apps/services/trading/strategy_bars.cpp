#include "strategy_bars.hpp"
#include <asterion/futures/trading_days.hpp>
#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace asterion::trading {
namespace wire = strategy::proto;

StrategyBars::StrategyBars(
    const wire::Instance& configuration,
    std::int64_t started_ms,
    std::uint64_t sequence)
    : daily_(configuration.period() == "1d"),
      interval_(daily_ ? 0 : std::stoll(configuration.period()) * 60000),
      started_ms_(started_ms),
      contracts_(configuration.contracts().begin(), configuration.contracts().end()),
      sequence_(sequence)
{
    for (const auto& contract : contracts_)
        current_.emplace(contract, Current{});
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
    if (quote.source_ms <= 0 || quote.received_ms - quote.source_ms > 10000
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
        current.bar.set_contract(contract);
        current.bar.set_trading_day(day);
        // A day completes at its 15:00 Beijing close, as research labels it.
        current.bar.set_end_ms(daily_ ? key + 15 * 3600000LL : key + interval_);
        current.bar.set_open_units(quote.last->raw());
        current.bar.set_high_units(quote.last->raw());
        current.bar.set_low_units(quote.last->raw());
        current.key = key;
        current.complete = complete;
        current.initialized = true;
    }
    auto& bar = current.bar;
    bar.set_high_units(std::max(bar.high_units(), quote.last->raw()));
    bar.set_low_units(std::min(bar.low_units(), quote.last->raw()));
    bar.set_close_units(quote.last->raw());
    bar.set_volume(bar.volume() + delta);
    bar.set_open_interest_units(quote.open_interest->raw());
    current.source_ms = quote.source_ms;
    current.volume = quote.volume;

    // A slow contract may not contribute a bar at this time, but must have
    // advanced past it. Never deliver the same timestamp in two Python steps.
    std::vector<wire::Bars> result;
    while (!pending_.empty()) {
        const auto key = pending_.begin()->first;
        if (std::ranges::any_of(current_, [key](const auto& item) {
                return !item.second.initialized || item.second.key <= key;
            }))
            break;
        wire::Bars batch;
        batch.set_sequence(++sequence_);
        for (const auto& contract : contracts_)
            if (const auto row = pending_.begin()->second.find(contract);
                row != pending_.begin()->second.end())
                *batch.add_bars() = row->second;
        result.push_back(std::move(batch));
        pending_.erase(pending_.begin());
    }
    if (pending_.size() > 128)
        throw std::runtime_error("strategy contracts stopped advancing together");
    return result;
}

} // namespace asterion::trading
