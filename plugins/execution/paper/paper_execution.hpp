#pragma once
#include <asterion/domain/account.hpp>
#include <asterion/domain/risk_port.hpp>
#include <optional>
#include <vector>

namespace asterion {

// Fraction of each bar's volume available to all simulated orders together.
inline const Decimal paper_bar_participation = Decimal::parse("0.1");

// One contract of a replayed portfolio: its terms and its ascending bars.
struct ContractBars
{
    // Its price increment is that of its first bar's day.
    ContractTerms terms;
    std::vector<MarketBar> bars;
    // Empty only for explicitly fixed-cost in-memory callers. Persisted inputs
    // always carry a validated, nonempty schedule through the protocol.
    std::vector<FuturesCostVersion> cost_schedule;
    // The days after the first bar's on which its price increment changes,
    // ascending: each bar is on the increment of its day.
    std::vector<PriceIncrementChange> price_increment_changes;
    // What every fill of the contract gives up, in price increments of the
    // fill's day.
    unsigned slippage_ticks = 0;
};

// Historical bar replay of a futures portfolio. The contracts' bars form one
// event stream ordered by bar time (then contract order); each advance
// consumes one bar of one contract. A working order can only fill on a later
// bar of its own contract than the one it was submitted after: a buy fills
// when the bar's low reaches its limit, at min(open, limit); a sell fills
// when the high reaches it, at max(open, limit). Fills share at most
// paper_bar_participation of that bar's volume.
//
// A contract's slippage moves every fill against the order by that many price
// increments of the day and leaves whether it fills alone. An order's limit
// is the worst price it accepts, so the price the bar must reach lies the
// slippage inside it: a buy at limit L fills when the low reaches
// L - slippage, at min(open, L - slippage) + slippage. A target decided at a
// price is placed that much beyond it, and so fills when the bar reaches the
// price itself.
//
// Each day's first bar, or its settlement when no contract has a bar that
// day, brings the day's costs and price increments. A price decided before
// an increment changed is moved onto the new one against the order: a buy
// up, a sale down.
class PaperExecution final
{
public:
    struct Event
    {
        std::size_t contract, bar;
    };

    PaperExecution(
        Decimal deposit,
        std::vector<ContractBars> contracts,
        std::shared_ptr<const RiskPort> risk);

    void start()
    {
        running_ = true;
    }

    void stop() noexcept
    {
        running_ = false;
    }

    void submit(LimitOrder order, Offset offset);
    void cancel(const std::string& id);

    // A strategy target decided from a preceding bar: the position wanted in
    // lots, positive long and negative short. Apply after any new-day cost
    // change and before matching the next bar of this contract.
    struct Target
    {
        std::string order_id;
        Decimal quantity, limit_price;
    };

    void advance(const std::optional<Target>& target = {});

    // One price per contract, ordered as account().contracts(); a contract
    // that has no settlement price that day has none and must hold no position.
    // Days are settled in order, each once: after every bar of the day and of
    // the days before it, and before any bar of a later day. A day need have no
    // bars at all: costs that change with it change all the same. No
    // outstanding orders.
    struct MarginRevaluation
    {
        Decimal previous_rate, rate, margin_before, available_before;
    };

    // An explicit single-contract percentage model may revalue margin in the
    // same transaction, after settling P&L. The next day still uses its schedule.
    std::optional<MarginRevaluation> settle_scheduled(
        const std::string& trading_day,
        const std::vector<std::optional<Decimal>>& prices,
        std::optional<Decimal> settlement_margin = {});
    void cancel_open_orders();
    // Working orders of one contract only.
    void cancel_open_orders(const InstrumentId& instrument);
    // Moves one contract toward a signed target with limit orders that fill
    // when a bar reaches `price`: their limits lie the contract's slippage
    // beyond it. Routed through normal account checks. Replaces the contract's working
    // orders. A position on the other side is closed first; the target's own
    // side opens on a later call, once that close has filled.
    void reconcile_target(
        const std::string& order_id,
        const InstrumentId& instrument,
        Decimal target,
        Decimal price);
    Json snapshot() const;

    const FuturesAccount& account() const noexcept
    {
        return account_;
    }

    // Number of consumed events; the next advance consumes event(cursor).
    std::size_t cursor() const noexcept
    {
        return cursor_;
    }

    std::size_t size() const noexcept
    {
        return data_->events.size();
    }

    const Event& event(std::size_t index) const
    {
        return data_->events.at(index);
    }

    const MarketBar& bar(const Event& event) const
    {
        return data_->contracts.at(event.contract).bars.at(event.bar);
    }

    const ContractBars& contract(std::size_t index) const
    {
        return data_->contracts.at(index);
    }

    // Incremented by every successful mutation. Each operation has a strong
    // exception guarantee, so an unchanged revision after a failure proves the
    // engine state is untouched.
    std::uint64_t revision() const noexcept
    {
        return revision_;
    }

    // Timestamp of the last consumed event, if any.
    std::optional<std::int64_t> timestamp_ns() const;

private:
    struct Data
    {
        std::vector<ContractBars> contracts;
        std::vector<Event> events;
    };

    void require_running() const;
    // What every contract costs and trades at on a trading day: its costs by
    // its schedule, its price increment by its changes. Within a transaction.
    void enter_day(const std::string& trading_day);
    // Whether a new day can bring other costs or price increments.
    bool changes_terms() const;
    FuturesAccount account_;
    std::shared_ptr<const RiskPort> risk_;
    std::shared_ptr<const Data> data_;
    std::size_t cursor_ = 0;
    std::uint64_t execution_sequence_ = 0;
    std::string last_settled_day_;
    std::uint64_t revision_ = 0;
    bool running_ = false;
};

} // namespace asterion
