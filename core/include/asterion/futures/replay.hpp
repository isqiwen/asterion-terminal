#pragma once
#include <asterion/futures/paper_execution.hpp>
#include <asterion/futures/replay_schedule.hpp>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace asterion {

// From `trading_day` on, a product's dominant series trades `contract`; its
// prices times `factor` are at the level of the series' latest month.
struct DominantRoll
{
    std::string trading_day;
    std::size_t contract;
    Decimal factor;
};

// A product's dominant series: later months of it, each from a later trading
// day. The first roll stands for the days before the series begins.
struct DominantSeries
{
    std::vector<DominantRoll> rolls;

    const DominantRoll& on(const std::string& trading_day) const;
};

// A month's raw price at the level of the series' latest month, back on the
// price grid. Signals and factors read it; orders and fills never do.
Decimal dominant_price(Decimal raw, Decimal factor, Decimal increment);

// A portfolio's bars replayed one timestamp at a time through a paper account,
// with a strategy's targets between them. Every engine, whatever decides the
// targets, trades by these rules:
//
// - A unit is what a strategy takes a side on: each contract that is no month
//   of a series, in contract order, then each dominant series.
// - A target is decided at a bar and placed at that bar's close; it fills on
//   the contract's next bar, or not at all: what is left of it is cancelled
//   after that bar.
// - A month of a series that is no longer dominant gives up what it holds at
//   the open of each of its bars until it holds nothing. The dominant month
//   takes over what its series wants at the open of its first bar that finds
//   the other months closed, once the account has a price of it.
// - After a trading day's bars every working order is cancelled and the day
//   is settled.
class Replay final
{
public:
    // What happens inside the replay, for its owner to record.
    class Observer
    {
    public:
        virtual ~Observer() = default;

        // Event `index` advanced with `intent`: orders from `previous_orders`
        // on in the account are those it placed.
        virtual void advanced(
            std::size_t,
            const std::optional<PaperExecution::Target>&,
            const RiskDecision&,
            std::size_t)
        {}

        // Event `index` is complete: its fills are in and its unfilled
        // orders cancelled.
        virtual void marked(std::size_t) {}

        // A trading day is settled at `timestamp_ns`, its last bar's or, without
        // bars, its session's close.
        virtual void settled(
            const PaperReplaySchedule::Day&,
            std::int64_t,
            const std::optional<PaperExecution::MarginRevaluation>&)
        {}
    };

    // One bar of the last timestamp. `seen` is what a strategy reads: for a
    // dominant series its month's bar at the series' latest level, on the
    // price increment of the bar's day. A month that is not dominant is
    // `leading` false: it only closes what it holds.
    struct Bar
    {
        std::size_t index, contract, unit;
        bool leading;
        MarketBar seen;
        Decimal close;
    };

    // `days`: each contract's settlements. `margins`: a percentage margin to
    // revalue by after a day's settlement, by trading day.
    Replay(
        Decimal deposit,
        std::vector<ContractBars> contracts,
        const std::vector<std::vector<DaySettlement>>& days,
        std::vector<DominantSeries> series,
        std::shared_ptr<const RiskPort> risk,
        std::map<std::string, Decimal> margins = {});

    std::size_t units() const noexcept
    {
        return units_;
    }

    // The unit a contract belongs to.
    std::size_t unit(std::size_t contract) const
    {
        return unit_.at(contract);
    }

    // For a contract, the series it is a month of, if any.
    std::optional<std::size_t> series(std::size_t contract) const;

    // Whether every trading day is settled.
    bool finished() const noexcept
    {
        return day_ == schedule_.days();
    }

    // The trading day being replayed.
    const PaperReplaySchedule::Day& day() const
    {
        return schedule_.day(day_);
    }

    // Whether every bar of the trading day is replayed.
    bool day_done() const;

    // Replays the bars of the day's next timestamp. Throws when `stop` is requested.
    const std::vector<Bar>& next(Observer& observer, std::stop_token stop = {});

    // What the account wants of a unit from now on: lots, positive long and
    // negative short; none leaves it alone.
    void want(std::size_t unit, std::optional<Decimal> lots);

    // Places what is wanted of the units with a bar at the last timestamp,
    // decided at those bars.
    void decide();

    // Places what is wanted of every contract, decided at its last bar; for an
    // account that follows another strategy from the next day.
    void redecide();

    // Settles the trading day once its bars are replayed.
    void settle(Observer& observer);

    const PaperExecution& execution() const noexcept
    {
        return execution_;
    }

private:
    void issue(const Bar& bar);
    std::optional<Decimal> target(std::size_t contract) const;
    bool holds(std::size_t contract) const;
    std::vector<std::size_t> series_;
    std::vector<std::size_t> unit_;
    std::size_t units_ = 0;
    std::vector<DominantSeries> rolls_;
    PaperReplaySchedule schedule_;
    PaperExecution execution_;
    std::map<std::string, Decimal> margins_;
    std::vector<std::optional<Decimal>> wanted_;
    std::vector<std::optional<PaperExecution::Target>> pending_;
    std::vector<std::optional<Bar>> last_;
    std::vector<bool> took_over_;
    std::vector<Bar> bars_;
    std::size_t day_ = 0, index_ = 0;
    std::int64_t settled_at_ = 0;
};

} // namespace asterion
