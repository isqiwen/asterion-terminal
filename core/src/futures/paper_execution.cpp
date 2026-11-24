#include <asterion/futures/paper_execution.hpp>
#include <asterion/futures/position_target.hpp>
#include <asterion/futures/trading_days.hpp>
#include <algorithm>
#include <queue>
#include <stdexcept>

namespace asterion {
namespace {

template <class Data>
const MarketBar& bar_of(const Data& data, const PaperExecution::Event& event)
{
    return data.contracts[event.contract].bars[event.bar];
}

// A contract's price increment on a trading day: that of the last change on
// or before it, or of its first bar's day.
Decimal increment_on(const ContractBars& contract, const std::string& trading_day)
{
    auto result = contract.terms.instrument.price_increment;
    for (const auto& change : contract.price_increment_changes) {
        if (change.trading_day > trading_day)
            break;
        result = change.price_increment;
    }
    return result;
}

// What a fill gives up on a day.
Decimal slippage_on(const ContractBars& contract, const std::string& trading_day)
{
    return increment_on(contract, trading_day) * Decimal::from_integer(contract.slippage_ticks);
}

const DayPriceLimits* price_limits_on(const ContractBars& contract, const std::string& trading_day)
{
    const auto found = std::ranges::lower_bound(
        contract.price_limits,
        trading_day,
        {},
        &DayPriceLimits::trading_day);
    return found != contract.price_limits.end() && found->trading_day == trading_day ? &*found
                                                                                     : nullptr;
}

std::vector<ContractTerms> terms_of(const std::vector<ContractBars>& contracts)
{
    std::vector<ContractTerms> result;
    for (const auto& contract : contracts) {
        auto terms = contract.terms;
        if (!contract.cost_schedule.empty()) {
            validate_cost_schedule(contract.cost_schedule);
            if (!contract.bars.empty())
                terms.costs =
                    costs_on(contract.cost_schedule, contract.bars.front().trading_day).values;
        }
        result.push_back(std::move(terms));
    }
    return result;
}

} // namespace

PaperExecution::PaperExecution(
    Decimal deposit,
    std::vector<ContractBars> contracts,
    OrderLimitsConfig limits)
    : PaperExecution(deposit, std::move(contracts), limits, {})
{}

PaperExecution::PaperExecution(
    Decimal deposit,
    std::vector<ContractBars> contracts,
    OrderLimitsConfig limits,
    std::vector<Event> events)
    : account_(deposit, terms_of(contracts)),
      risk_(limits),
      max_order_quantity_(limits.max_order_quantity)
{
    auto data = std::make_shared<Data>();
    data->contracts = std::move(contracts);
    std::size_t total = 0;
    for (std::size_t c = 0; c < data->contracts.size(); ++c) {
        const auto& contract = data->contracts[c];
        // Each change follows the first bar's day and the change before it, and
        // is to another increment.
        auto instrument = contract.terms.instrument;
        for (const std::string* after = nullptr;
            const auto& change : contract.price_increment_changes) {
            if (contract.bars.empty() || change.trading_day <= contract.bars.front().trading_day
                || (after && change.trading_day <= *after) || change.price_increment <= Decimal{}
                || change.price_increment == instrument.price_increment)
                throw std::invalid_argument("invalid price increment change");
            instrument.price_increment = change.price_increment;
            after = &change.trading_day;
        }
        for (const std::string* after = nullptr; const auto& limit : contract.price_limits) {
            (void)parse_trading_date(limit.trading_day);
            if ((after && limit.trading_day <= *after) || limit.lower_limit <= Decimal{}
                || limit.upper_limit < limit.lower_limit)
                throw std::invalid_argument("invalid daily price limits");
            const auto tick = increment_on(contract, limit.trading_day);
            if (!limit.lower_limit.multiple_of(tick) || !limit.upper_limit.multiple_of(tick))
                throw std::invalid_argument("daily price limits must align with price increment");
            after = &limit.trading_day;
        }
        const MarketBar* previous = nullptr;
        for (std::size_t b = 0; b < contract.bars.size(); ++b) {
            const auto& bar = contract.bars[b];
            instrument.price_increment = increment_on(contract, bar.trading_day);
            bar.validate(instrument);
            if (bar.low <= Decimal{})
                throw std::invalid_argument("futures paper model requires positive market prices");
            // A sale that gives the slippage up must still have a price.
            if (bar.low <= slippage_on(contract, bar.trading_day))
                throw std::invalid_argument("slippage reaches a contract's lowest price");
            if (previous
                && (bar.timestamp_ns <= previous->timestamp_ns
                    || bar.trading_day < previous->trading_day))
                throw std::invalid_argument("historical bars must be strictly ascending");
            previous = &bar;
        }
        total += contract.bars.size();
    }
    if (!total)
        throw std::invalid_argument("paper replay requires historical bars");
    for (const auto& contract : data->contracts)
        if (contract.bars.empty())
            throw std::invalid_argument("every portfolio contract requires historical bars");
    // One clock for the engine, its schedule and strategy replays.
    if (events.empty()) {
        std::vector<const std::vector<MarketBar>*> series;
        series.reserve(data->contracts.size());
        for (const auto& contract : data->contracts)
            series.push_back(&contract.bars);
        data->events = replay_order(series);
    } else {
        data->events = std::move(events);
    }
    for (std::size_t i = 1; i < data->events.size(); ++i)
        if (bar_of(*data, data->events[i]).trading_day
            < bar_of(*data, data->events[i - 1]).trading_day)
            throw std::invalid_argument("portfolio bars disagree on trading days by time");
    data_ = std::move(data);
}

void PaperExecution::require_running() const
{
    if (!running_)
        throw std::logic_error("paper execution plugin is not started");
}

void PaperExecution::submit(LimitOrder order, Offset offset)
{
    require_running();
    if (cursor_ == size())
        throw std::invalid_argument("replay has finished; no new orders accepted");
    const auto decision = assess_order(risk_, account_, order, offset);
    if (!decision.allowed())
        throw PaperRiskRejected(decision.reason);
    try {
        account_.submit(std::move(order), offset);
    } catch (const InsufficientAvailableFunds&) {
        throw PaperRiskRejected(RiskReason::insufficient_funds);
    }
    ++revision_;
}

void PaperExecution::cancel(const std::string& id)
{
    require_running();
    account_.cancel(id);
    ++revision_;
}

namespace {

// Called only for FuturesAccount::working_orders(). A bar must reach the
// price that lies the slippage inside the limit; the fill gives the slippage
// back up and so never passes the limit. A bar locked at a daily price limit
// cannot fill orders pushing into that limit.
std::optional<Decimal> fill_price(
    const AccountOrder& item,
    const MarketBar& bar,
    Decimal slippage,
    const DayPriceLimits* limits)
{
    const auto& request = item.order.request();
    if (request.side == Side::buy) {
        if (limits && bar.low >= limits->upper_limit)
            return std::nullopt;
        const auto reached = request.limit_price - slippage;
        if (bar.low > reached)
            return std::nullopt;
        auto price = std::min(bar.open, reached) + slippage;
        if (limits)
            price = std::min(price, limits->upper_limit);
        return price;
    }
    if (limits && bar.high <= limits->lower_limit)
        return std::nullopt;
    const auto reached = request.limit_price + slippage;
    if (bar.high < reached)
        return std::nullopt;
    auto price = std::max(bar.open, reached) - slippage;
    if (limits)
        price = std::max(price, limits->lower_limit);
    return price;
}

} // namespace

RiskDecision PaperExecution::advance(const std::optional<Target>& target)
{
    require_running();
    if (cursor_ == size())
        throw std::invalid_argument("replay has finished");
    const auto& current = event(cursor_);
    const auto& bar = this->bar(current);
    const auto& terms = data_->contracts[current.contract].terms;
    const auto& instrument = terms.instrument.id;
    const auto slippage = slippage_on(data_->contracts[current.contract], bar.trading_day);
    const auto* limits = price_limits_on(data_->contracts[current.contract], bar.trading_day);
    const auto fills = [&](const AccountOrder& item) {
        return item.order.request().instrument == instrument
            && fill_price(item, bar, slippage, limits).has_value();
    };
    const bool new_terms =
        cursor_ && bar.trading_day != this->bar(event(cursor_ - 1)).trading_day && changes_terms();
    const auto same_time = [&](std::size_t index) {
        const auto& other = this->bar(event(index));
        return other.timestamp_ns == bar.timestamp_ns && other.trading_day == bar.trading_day;
    };
    const bool first = cursor_ == 0 || !same_time(cursor_ - 1);
    const bool last = cursor_ + 1 == size() || !same_time(cursor_ + 1);
    // At a lone bar or inside a batch, an idle event needs no ledger copy.
    if (first == last && !target && !new_terms
        && std::ranges::none_of(account_.working_orders(), [&](const auto index) {
               return fills(account_.orders()[index]);
           })) {
        if (last)
            account_.mark(instrument, bar.close);
        ++cursor_;
        ++revision_;
        return {RiskReason::allowed};
    }
    RiskDecision result{RiskReason::allowed};
    const auto previous_revision = revision_;
    auto batch = account_.transaction();
    try {
        auto& next = account_;
        if (new_terms)
            enter_day(bar.trading_day);
        // All simultaneous opens are known before any target trades. Closes arrive after all fills.
        if (first) {
            for (auto n = cursor_; n < size() && same_time(n); ++n) {
                const auto& opening = event(n);
                next.mark(contract(opening.contract).terms.instrument.id, this->bar(opening).open);
            }
        }
        auto sequence = execution_sequence_;
        auto liquidity = quantize(
            multiply(bar.volume, paper_bar_participation, Rounding::floor),
            terms.instrument.quantity_increment,
            Rounding::floor);
        std::size_t leg = 0;
        result = match(current, target, liquidity, sequence, leg, false);
        if (last) {
            for (auto n = cursor_ + 1; n > 0 && same_time(n - 1); --n) {
                const auto& closing = event(n - 1);
                next.mark(contract(closing.contract).terms.instrument.id, this->bar(closing).close);
            }
        }
        batch.commit();
        execution_sequence_ = sequence;
        ++cursor_;
        ++revision_;
    } catch (...) {
        revision_ = previous_revision;
        throw;
    }
    return result;
}

Decimal PaperExecution::held_quantity(const InstrumentId& instrument) const
{
    Decimal held;
    for (const auto& lot : account_.positions())
        if (lot.instrument == instrument)
            held = held + (lot.side == Side::buy ? lot.quantity : Decimal{} - lot.quantity);
    return held;
}

RiskDecision PaperExecution::match(
    const Event& current,
    const std::optional<Target>& target,
    Decimal& liquidity,
    std::uint64_t& sequence,
    std::size_t& leg,
    bool reduce_only)
{
    auto& next = account_;
    const auto& bar = this->bar(current);
    const auto& instrument = contract(current.contract).terms.instrument.id;
    const auto slippage = slippage_on(contract(current.contract), bar.trading_day);
    const auto* limits = price_limits_on(contract(current.contract), bar.trading_day);
    RiskDecision result{RiskReason::allowed};
    const auto match_working = [&] {
        // Arrival order shares the bar's participation volume. Fills and cancels
        // never append orders, so indexes stay stable during the pass.
        for (auto pending = next.working_orders().begin();
            pending != next.working_orders().end() && liquidity != Decimal{};) {
            // Advance before filling/cancelling: completion removes just this index.
            const auto i = *pending++;
            const auto& item = next.orders()[i];
            if (item.order.request().instrument != instrument)
                continue;
            const auto price = fill_price(item, bar, slippage, limits);
            if (!price)
                continue;
            const auto id = item.order.request().id;
            if (item.offset == Offset::open && next.available() < Decimal{}) {
                next.cancel(id);
                continue;
            }
            const auto quantity = std::min(liquidity, item.order.remaining_quantity());
            next.fill({"paper.fill." + std::to_string(++sequence), id, quantity, *price});
            liquidity = liquidity - quantity;
        }
    };
    if (target) {
        auto quantity = target->quantity;
        if (reduce_only) {
            const auto held = held_quantity(instrument);
            quantity = std::clamp(quantity, std::min(Decimal{}, held), std::max(Decimal{}, held));
        }
        for (;;) {
            const auto orders_before = next.orders().size();
            try {
                reconcile_target(
                    leg == 0 ? target->order_id : target->order_id + "." + std::to_string(leg + 1),
                    instrument,
                    quantity,
                    target->limit_price);
            } catch (const PaperRiskRejected& rejection) {
                switch (rejection.reason()) {
                    case RiskReason::gross_quantity:
                    case RiskReason::working_orders:
                    case RiskReason::insufficient_funds:
                        result.reason = rejection.reason();
                        break;
                    default:
                        throw;
                }
                break;
            }
            if (next.orders().size() == orders_before)
                break;
            ++leg;
            match_working();
            const bool open_remains =
                std::ranges::any_of(next.working_orders(), [&](const auto index) {
                    return next.orders()[index].order.request().instrument == instrument;
                });
            if (open_remains || liquidity == Decimal{})
                break;
        }
    } else {
        match_working();
    }
    return result;
}

std::vector<RiskDecision> PaperExecution::advance_targets(
    std::span<const std::optional<Target>> targets,
    const std::function<bool(std::size_t)>& can_open)
{
    require_running();
    const auto& day = bar(event(cursor_)).trading_day;
    const bool new_terms = cursor_ && day != bar(event(cursor_ - 1)).trading_day && changes_terms();
    std::vector<RiskDecision> decisions(targets.size(), {RiskReason::allowed});
    // Skip transaction snapshot and matching passes when no working orders, term
    // changes or position deltas exist at this timestamp.
    bool idle = !new_terms && !account_.has_working_orders();
    for (std::size_t i = 0; idle && i < targets.size(); ++i) {
        if (!targets[i])
            continue;
        const auto& current = event(cursor_ + i);
        const auto& spec = contract(current.contract).terms.instrument;
        if (targets[i]->order_id.empty() || targets[i]->order_id.size() > 128
            || !targets[i]->quantity.multiple_of(spec.quantity_increment)) {
            idle = false;
            break;
        }
        Decimal long_qty, short_qty;
        for (const auto& lot : account_.positions()) {
            if (lot.instrument != spec.id)
                continue;
            if (lot.side == Side::buy)
                long_qty = long_qty + lot.quantity;
            else
                short_qty = short_qty + lot.quantity;
        }
        if ((long_qty > Decimal{} && short_qty > Decimal{})
            || long_qty - short_qty != targets[i]->quantity)
            idle = false;
    }
    if (idle) {
        for (std::size_t i = 0; i < targets.size(); ++i) {
            const auto& current = event(cursor_ + i);
            account_.mark(contract(current.contract).terms.instrument.id, bar(current).close);
        }
        cursor_ += targets.size();
        ++revision_;
        return decisions;
    }
    const auto previous_revision = revision_;
    auto batch = account_.transaction();
    try {
        if (new_terms)
            enter_day(day);
        std::vector<Decimal> liquidity;
        std::vector<std::size_t> legs(targets.size());
        auto sequence = execution_sequence_;
        for (std::size_t i = 0; i < targets.size(); ++i) {
            const auto& current = event(cursor_ + i);
            const auto& spec = contract(current.contract).terms.instrument;
            const auto& opening = bar(current);
            account_.mark(spec.id, opening.open);
            liquidity.push_back(quantize(
                multiply(opening.volume, paper_bar_participation, Rounding::floor),
                spec.quantity_increment,
                Rounding::floor));
        }
        for (std::size_t i = 0; i < targets.size(); ++i)
            decisions[i] =
                match(event(cursor_ + i), targets[i], liquidity[i], sequence, legs[i], true);
        bool closing = false;
        for (std::size_t i = 0; i < targets.size(); ++i) {
            if (!targets[i])
                continue;
            const auto held =
                held_quantity(contract(event(cursor_ + i).contract).terms.instrument.id);
            closing = closing || (held > Decimal{} && targets[i]->quantity < held)
                || (held < Decimal{} && targets[i]->quantity > held);
        }
        if (!closing)
            for (std::size_t i = 0; i < targets.size(); ++i)
                if (decisions[i].allowed() && can_open(cursor_ + i)
                    && (legs[i] == 0 || liquidity[i] > Decimal{}))
                    decisions[i] = match(
                        event(cursor_ + i),
                        targets[i],
                        liquidity[i],
                        sequence,
                        legs[i],
                        false);
        for (std::size_t i = 0; i < targets.size(); ++i) {
            const auto& current = event(cursor_ + i);
            const auto& instrument = contract(current.contract).terms.instrument.id;
            cancel_open_orders(instrument);
            account_.mark(instrument, bar(current).close);
        }
        batch.commit();
        execution_sequence_ = sequence;
        cursor_ += targets.size();
        ++revision_;
    } catch (...) {
        revision_ = previous_revision;
        throw;
    }
    return decisions;
}

bool PaperExecution::changes_terms() const
{
    return std::ranges::any_of(data_->contracts, [](const auto& c) {
        return !c.cost_schedule.empty() || !c.price_increment_changes.empty();
    });
}

void PaperExecution::enter_day(const std::string& trading_day)
{
    std::vector<FuturesCosts> costs;
    std::vector<Decimal> increments;
    for (const auto& contract : data_->contracts) {
        const auto& cost_day = !contract.cost_schedule.empty() && !contract.bars.empty()
                && trading_day < contract.bars.front().trading_day
                && trading_day < contract.cost_schedule.front().effective_from
            ? contract.bars.front().trading_day
            : trading_day;
        costs.push_back(
            contract.cost_schedule.empty() ? contract.terms.costs
                                           : costs_on(contract.cost_schedule, cost_day).values);
        increments.push_back(increment_on(contract, trading_day));
    }
    account_.update_costs(costs);
    account_.update_price_increments(increments);
}

std::optional<PaperExecution::MarginRevaluation> PaperExecution::settle_scheduled(
    const std::string& trading_day,
    const std::vector<std::optional<Decimal>>& prices,
    std::optional<Decimal> settlement_margin)
{
    require_running();
    if (trading_day <= last_settled_day_
        || (cursor_ && bar(event(cursor_ - 1)).trading_day > trading_day)
        || (cursor_ < size() && bar(event(cursor_)).trading_day <= trading_day))
        throw std::invalid_argument(
            "a day is settled once, after its bars and before those of a later day");
    // The first bar of a day brings the day's terms. A day after the first bar
    // that has none of its own follows the calendar here: the schedules cover
    // the days from that bar on.
    const bool idle =
        cursor_ && bar(event(cursor_ - 1)).trading_day < trading_day && changes_terms();
    auto batch = account_.transaction();
    if (idle)
        enter_day(trading_day);
    account_.settle_traded(prices);
    std::optional<MarginRevaluation> revaluation;
    if (settlement_margin) {
        if (*settlement_margin <= Decimal{} || *settlement_margin >= Decimal::parse("1"))
            throw std::invalid_argument(
                "settlement margin requires a single-contract percentage model");
        std::vector<FuturesCosts> updated;
        for (const auto& contract : account_.contracts()) {
            if (contract.costs.margin_per_lot != Decimal{})
                throw std::invalid_argument(
                    "settlement margin requires a single-contract percentage model");
            auto costs = contract.costs;
            costs.margin_rate = *settlement_margin;
            costs.short_margin_rate = *settlement_margin;
            updated.push_back(costs);
        }
        revaluation = MarginRevaluation{
            account_.contracts()[0].costs.margin_rate,
            *settlement_margin,
            account_.margin(),
            account_.available()
        };
        account_.update_costs(updated);
    }
    batch.commit();
    last_settled_day_ = trading_day;
    ++revision_;
    return revaluation;
}

void PaperExecution::cancel_open_orders()
{
    require_running();
    // Cancelling a working order cannot fail, so the loop is all-or-nothing.
    while (!account_.working_orders().empty()) {
        account_.cancel(account_.orders()[*account_.working_orders().begin()].order.request().id);
        ++revision_;
    }
}

void PaperExecution::cancel_open_orders(const InstrumentId& instrument)
{
    require_running();
    (void)account_.contract_index(instrument);
    for (auto pending = account_.working_orders().begin();
        pending != account_.working_orders().end();) {
        const auto& item = account_.orders()[*pending++];
        if (item.order.request().instrument == instrument) {
            account_.cancel(item.order.request().id);
            ++revision_;
        }
    }
}

void PaperExecution::reconcile_target(
    const std::string& order_id,
    const InstrumentId& instrument,
    Decimal target,
    Decimal price)
{
    require_running();
    if (order_id.empty() || order_id.size() > 128)
        throw std::invalid_argument("invalid target order identity");
    const auto index = account_.contract_index(instrument);
    const auto& spec = account_.contracts()[index].instrument;
    if (!target.multiple_of(spec.quantity_increment))
        throw std::invalid_argument("target must be lot aligned");
    HeldPosition held;
    for (const auto& lot : account_.positions()) {
        if (lot.instrument != instrument)
            continue;
        if (held.today + held.yesterday > Decimal{} && lot.side != held.side)
            throw std::invalid_argument("a target needs the contract held on one side only");
        held.side = lot.side;
        auto& bucket = lot.today ? held.today : held.yesterday;
        bucket = bucket + lot.quantity;
    }
    // On the increment the account trades the contract at now: a price decided
    // before it changed is moved onto it against the order.
    const auto increment = spec.price_increment;
    const auto slippage = increment * Decimal::from_integer(data_->contracts[index].slippage_ticks);
    const auto orders = target_orders(target, held, account_.close_policy(instrument));
    const auto leg = quantize(max_order_quantity_, spec.quantity_increment, Rounding::floor);
    const auto working = std::ranges::any_of(account_.working_orders(), [&](const auto index) {
        return account_.orders()[index].order.request().instrument == instrument;
    });
    if (orders.empty() && !working)
        return;
    // Keep account and revision changes atomic across both close buckets without
    // copying the completed order/fill history. Risk checks keep the same order.
    auto batch = account_.transaction();
    const auto previous_revision = revision_;
    try {
        for (auto pending = account_.working_orders().begin();
            pending != account_.working_orders().end();) {
            const auto index = *pending++;
            const auto& item = account_.orders()[index];
            if (item.order.request().instrument == instrument)
                account_.cancel(item.order.request().id);
        }
        for (const auto& order : orders)
            // The limit lies the slippage beyond the price a bar must reach.
            submit(
                {order_id + std::string(order.suffix),
                    instrument,
                    order.side,
                    std::min(order.quantity, leg),
                    order.side == Side::buy
                        ? quantize(price, increment, Rounding::ceiling) + slippage
                        : quantize(price, increment, Rounding::floor) - slippage},
                order.offset);
    } catch (...) {
        revision_ = previous_revision;
        throw;
    }
    batch.commit();
    ++revision_;
}

std::optional<std::int64_t> PaperExecution::timestamp_ns() const
{
    if (!cursor_)
        return std::nullopt;
    return bar(event(cursor_ - 1)).timestamp_ns;
}

PaperExecution::Snapshot PaperExecution::snapshot() const
{
    return {account_.snapshot(), cursor_, size(), timestamp_ns()};
}

std::vector<PaperExecution::Event> replay_order(
    const std::vector<const std::vector<MarketBar>*>& contracts)
{
    std::size_t total = 0;
    bool per_contract_sorted = true;
    for (const auto* series : contracts) {
        total += series->size();
        for (std::size_t i = 1; per_contract_sorted && i < series->size(); ++i)
            if ((*series)[i].timestamp_ns < (*series)[i - 1].timestamp_ns)
                per_contract_sorted = false;
    }
    std::vector<PaperExecution::Event> events;
    events.reserve(total);
    if (contracts.size() == 1 && per_contract_sorted) {
        for (std::size_t b = 0; b < contracts[0]->size(); ++b)
            events.push_back({0, b});
        return events;
    }
    if (per_contract_sorted && contracts.size() > 1) {
        struct Entry
        {
            bool operator>(const Entry& other) const noexcept
            {
                if (timestamp_ns != other.timestamp_ns)
                    return timestamp_ns > other.timestamp_ns;
                if (contract != other.contract)
                    return contract > other.contract;
                return bar > other.bar;
            }

            std::int64_t timestamp_ns;
            std::size_t contract;
            std::size_t bar;
        };

        std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> heap;
        for (std::size_t c = 0; c < contracts.size(); ++c)
            if (!contracts[c]->empty())
                heap.push({(*contracts[c])[0].timestamp_ns, c, 0});
        while (!heap.empty()) {
            const auto top = heap.top();
            heap.pop();
            events.push_back({top.contract, top.bar});
            const auto next_bar = top.bar + 1;
            if (next_bar < contracts[top.contract]->size())
                heap.push(
                    {(*contracts[top.contract])[next_bar].timestamp_ns, top.contract, next_bar});
        }
        return events;
    }
    for (std::size_t c = 0; c < contracts.size(); ++c)
        for (std::size_t b = 0; b < contracts[c]->size(); ++b)
            events.push_back({c, b});
    std::ranges::stable_sort(events, [&](const auto& left, const auto& right) {
        const auto& a = contracts[left.contract]->at(left.bar);
        const auto& b = contracts[right.contract]->at(right.bar);
        return a.timestamp_ns != b.timestamp_ns ? a.timestamp_ns < b.timestamp_ns
                                                : left.contract < right.contract;
    });
    return events;
}

} // namespace asterion
