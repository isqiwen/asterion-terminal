// asterion_engine: core's Replay for the lab's Python backtests. Amounts and
// prices cross as the raw int64 of an eight-place Decimal, never as floats.
#include <asterion/futures/intraday_bars.hpp>
#include <asterion/futures/order_limits.hpp>
#include <asterion/futures/performance.hpp>
#include <asterion/futures/trading_days.hpp>
#include <asterion/futures/portfolio_targets.hpp>
#include <asterion/futures/replay.hpp>
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace py = pybind11;

namespace asterion {
namespace {

Decimal raw(const py::handle& value)
{
    return Decimal::from_raw(value.cast<std::int64_t>());
}

const char* side_name(Side side)
{
    return side == Side::buy ? "buy" : "sell";
}

const char* offset_name(Offset offset)
{
    switch (offset) {
        case Offset::open:
            return "open";
        case Offset::close_today:
            return "close_today";
        case Offset::close_yesterday:
            return "close_yesterday";
        case Offset::close:
            return "close";
    }
    return "";
}

const char* state_name(OrderState state)
{
    switch (state) {
        case OrderState::pending:
            return "pending";
        case OrderState::accepted:
            return "accepted";
        case OrderState::partially_filled:
            return "partially_filled";
        case OrderState::filled:
            return "filled";
        case OrderState::cancelled:
            return "cancelled";
        case OrderState::rejected:
            return "rejected";
    }
    return "";
}

// One contract as Python gives it: its terms, dated costs and price
// increments, slippage, bars and daily settlements.
ContractBars contract_of(const py::dict& item, std::vector<DaySettlement>& days)
{
    ContractBars contract;
    auto& instrument = contract.terms.instrument;
    instrument.id = {item["venue"].cast<std::string>(), item["symbol"].cast<std::string>()};
    instrument.quote_currency = "CNY";
    instrument.price_increment = raw(item["price_increment"]);
    instrument.quantity_increment = raw(item["quantity_increment"]);
    instrument.multiplier = raw(item["multiplier"]);
    for (const auto& version : item["costs"].cast<py::list>()) {
        const auto values = version["values"].cast<std::vector<std::optional<std::int64_t>>>();
        if (values.size() != 8 && values.size() != 9)
            throw std::invalid_argument("costs have eight or nine values");
        const bool has_close_today = values[2].has_value() && values[6].has_value();
        if (values[2].has_value() != values[6].has_value() || !values[0] || !values[1] || !values[3]
            || !values[4] || !values[5] || !values[7] || (values.size() == 9 && !values[8]))
            throw std::invalid_argument("missing required cost value");
        FuturesCosts costs{
            Decimal::from_raw(*values[0]),
            Decimal::from_raw(*values[1]),
            has_close_today ? Decimal::from_raw(*values[2]) : Decimal{},
            Decimal::from_raw(*values[3]),
            Decimal::from_raw(*values[4]),
            Decimal::from_raw(*values[5]),
            has_close_today ? Decimal::from_raw(*values[6]) : Decimal{},
            Decimal::from_raw(*values[7]),
            values.size() == 9 ? Decimal::from_raw(*values[8]) : Decimal::from_raw(*values[4]),
            has_close_today
        };
        contract.cost_schedule.push_back(
            {version["effective_from"].cast<std::string>(),
                version["source"].cast<std::string>(),
                costs});
    }
    if (contract.cost_schedule.empty())
        throw std::invalid_argument("a contract needs its costs");
    contract.terms.costs = contract.cost_schedule.front().values;
    for (const auto& change : item["increments"].cast<py::list>())
        contract.price_increment_changes.push_back(
            {change[py::int_(0)].cast<std::string>(), raw(change[py::int_(1)])});
    contract.slippage_ticks = item["slippage_ticks"].cast<unsigned>();
    if (item.contains("price_limits")) {
        for (const auto& entry : item["price_limits"].cast<py::list>()) {
            const auto row = entry.cast<py::tuple>();
            if (row.size() != 3)
                throw std::invalid_argument("daily price limits have three fields");
            contract.price_limits.push_back({row[0].cast<std::string>(), raw(row[1]), raw(row[2])});
        }
    }
    // (trading_day, timestamp_ns, open, high, low, close, volume)
    if (item.contains("bars_columns")) {
        const auto cols = item["bars_columns"].cast<py::tuple>();
        if (cols.size() != 7)
            throw std::invalid_argument("bars_columns has seven fields");
        const auto days_list = cols[0].cast<py::list>();
        const auto ts =
            cols[1].cast<py::array_t<std::int64_t, py::array::c_style | py::array::forcecast>>();
        const auto op =
            cols[2].cast<py::array_t<std::int64_t, py::array::c_style | py::array::forcecast>>();
        const auto hi =
            cols[3].cast<py::array_t<std::int64_t, py::array::c_style | py::array::forcecast>>();
        const auto lo =
            cols[4].cast<py::array_t<std::int64_t, py::array::c_style | py::array::forcecast>>();
        const auto cl =
            cols[5].cast<py::array_t<std::int64_t, py::array::c_style | py::array::forcecast>>();
        const auto vo =
            cols[6].cast<py::array_t<std::int64_t, py::array::c_style | py::array::forcecast>>();
        const auto n = static_cast<py::ssize_t>(days_list.size());
        if (ts.size() != n || op.size() != n || hi.size() != n || lo.size() != n || cl.size() != n
            || vo.size() != n)
            throw std::invalid_argument("bars_columns arrays must have equal length");
        contract.bars.reserve(static_cast<std::size_t>(n));
        const auto* ts_p = ts.data();
        const auto* op_p = op.data();
        const auto* hi_p = hi.data();
        const auto* lo_p = lo.data();
        const auto* cl_p = cl.data();
        const auto* vo_p = vo.data();
        for (py::ssize_t i = 0; i < n; ++i) {
            contract.bars.push_back(
                {days_list[i].cast<std::string>(),
                    ts_p[i],
                    Decimal::from_raw(op_p[i]),
                    Decimal::from_raw(hi_p[i]),
                    Decimal::from_raw(lo_p[i]),
                    Decimal::from_raw(cl_p[i]),
                    Decimal::from_raw(vo_p[i])});
        }
    } else {
        const auto bars_list = item["bars"].cast<py::list>();
        contract.bars.reserve(bars_list.size());
        for (const auto& bar : bars_list) {
            const auto row = bar.cast<py::tuple>();
            if (row.size() != 7)
                throw std::invalid_argument("a bar has seven fields");
            contract.bars.push_back(
                {row[0].cast<std::string>(),
                    row[1].cast<std::int64_t>(),
                    raw(row[2]),
                    raw(row[3]),
                    raw(row[4]),
                    raw(row[5]),
                    raw(row[6])});
        }
    }
    for (const auto& day : item["settlements"].cast<py::list>())
        days.push_back({day[py::int_(0)].cast<std::string>(), raw(day[py::int_(1)])});
    return contract;
}

// What the replay did, kept for the run's tables in plain C++ structs so
// Python tuples are materialized only when read.
struct Record final : Replay::Observer
{
    struct EquityPoint
    {
        std::int64_t time_ns, equity;
        const char* event;
    };

    struct SettlementRow
    {
        std::string trading_day;
        std::int64_t time_ns, balance, equity, realized, fees, margin, available;
    };

    struct PositionRow
    {
        std::string trading_day;
        std::size_t contract;
        std::int64_t held_long, held_short, price, margin;
    };

    struct RejectionRow
    {
        std::int64_t time_ns;
        std::string trading_day;
        std::size_t contract;
        std::int64_t lots;
        std::string_view reason;
    };

    std::int64_t value() const
    {
        const auto& account = replay->execution().account();
        return (account.balance() + account.unrealized()).raw();
    }

    void advanced(
        std::size_t index,
        const std::optional<PaperExecution::Target>& intent,
        const RiskDecision& decision) override
    {
        const auto& execution = replay->execution();
        const auto& bar = execution.bar(execution.event(index));
        order_times.resize(execution.account().orders().size(), bar.timestamp_ns);
        fill_times.resize(execution.account().fills().size(), bar.timestamp_ns);
        order_days.resize(execution.account().orders().size(), bar.trading_day);
        fill_days.resize(execution.account().fills().size(), bar.trading_day);
        if (!decision.allowed() && intent)
            rejections.push_back(
                {bar.timestamp_ns,
                    bar.trading_day,
                    execution.event(index).contract,
                    intent->quantity.raw(),
                    risk_reason_name(decision.reason)});
    }

    void marked(std::size_t index) override
    {
        const auto& execution = replay->execution();
        equity.push_back({execution.bar(execution.event(index)).timestamp_ns, value(), "mark"});
    }

    void settled(
        const PaperReplaySchedule::Day& day,
        std::int64_t at,
        const std::optional<PaperExecution::MarginRevaluation>&) override
    {
        const auto& account = replay->execution().account();
        const auto current = value();
        equity.push_back({at, current, "settlement"});
        settlements.push_back(
            {day.trading_day,
                at,
                account.balance().raw(),
                current,
                account.realized().raw(),
                account.fees().raw(),
                account.margin().raw(),
                account.available().raw()});
        if (account.positions().empty())
            return;

        struct Held
        {
            Decimal held_long, held_short, margin;
        };

        std::map<std::size_t, Held> by_contract;
        for (const auto& lot : account.positions()) {
            const auto c = account.contract_index(lot.instrument);
            if (!day.prices[c])
                continue;
            const auto& terms = account.contracts()[c];
            auto& entry = by_contract[c];
            auto& bucket = lot.side == Side::buy ? entry.held_long : entry.held_short;
            bucket = bucket + lot.quantity;
            entry.margin = entry.margin
                + terms.costs
                      .margin(lot.side, lot.quantity, *day.prices[c], terms.instrument.multiplier);
        }
        for (const auto& [c, entry] : by_contract) {
            if (entry.held_long != Decimal{} || entry.held_short != Decimal{})
                positions.push_back(
                    {day.trading_day,
                        c,
                        entry.held_long.raw(),
                        entry.held_short.raw(),
                        day.prices[c]->raw(),
                        entry.margin.raw()});
        }
    }

    const Replay* replay = nullptr;
    std::vector<EquityPoint> equity;
    std::vector<SettlementRow> settlements;
    std::vector<PositionRow> positions;
    std::vector<RejectionRow> rejections;
    // When each order was placed and each fill made, and on which trading day.
    std::vector<std::int64_t> order_times, fill_times;
    std::vector<std::string> order_days, fill_days;
};

class Engine
{
public:
    Engine(
        std::int64_t deposit,
        const py::list& contracts,
        const py::list& series,
        const py::tuple& limits,
        const py::dict& margins)
    {
        std::vector<ContractBars> items;
        std::vector<std::vector<DaySettlement>> days;
        for (const auto& item : contracts) {
            auto& settled = days.emplace_back();
            items.push_back(contract_of(item.cast<py::dict>(), settled));
        }
        std::vector<DominantSeries> rolls;
        for (const auto& item : series) {
            auto& current = rolls.emplace_back();
            for (const auto& roll : item.cast<py::list>())
                current.rolls.push_back(
                    {roll[py::int_(0)].cast<std::string>(), roll[py::int_(1)].cast<std::size_t>()});
        }
        if (limits.size() != 4)
            throw std::invalid_argument(
                "limits are order lots, gross lots, working orders and contract lots");
        OrderLimitsConfig
            config{raw(limits[0]), raw(limits[1]), limits[2].cast<std::size_t>(), raw(limits[3])};
        std::map<std::string, Decimal> revalued;
        for (const auto& [day, rate] : margins)
            revalued.emplace(day.cast<std::string>(), raw(rate));
        replay_ = std::make_unique<Replay>(
            Decimal::from_raw(deposit),
            std::move(items),
            days,
            std::move(rolls),
            config,
            std::move(revalued));
        record_.replay = replay_.get();
    }

    // The bars of the next timestamp: (event, contract, unit, leading,
    // trading_day, timestamp_ns, open, high, low, close, volume), each at its
    // month's own prices.
    py::list next()
    {
        py::list result;
        for (const auto& bar : replay_->next(record_)) {
            const auto& prices = bar.bar;
            result.append(
                py::make_tuple(
                    bar.index,
                    bar.contract,
                    bar.unit,
                    bar.leading,
                    prices.trading_day,
                    prices.timestamp_ns,
                    prices.open.raw(),
                    prices.high.raw(),
                    prices.low.raw(),
                    prices.close.raw(),
                    prices.volume.raw()));
        }
        return result;
    }

    py::list next_leading()
    {
        py::list result;
        for (const auto& bar : replay_->next(record_)) {
            if (!bar.leading)
                continue;
            const auto& prices = bar.bar;
            result.append(
                py::make_tuple(
                    bar.contract,
                    bar.unit,
                    prices.timestamp_ns,
                    prices.open.raw(),
                    prices.high.raw(),
                    prices.low.raw(),
                    prices.close.raw(),
                    prices.volume.raw()));
        }
        return result;
    }

    void want(std::size_t unit, std::optional<std::int64_t> lots)
    {
        replay_->want(unit, lots ? std::optional{Decimal::from_raw(*lots)} : std::nullopt);
    }

    void decide()
    {
        replay_->decide();
    }

    // Weights of equity by unit as whole lots, toward zero: each unit at the
    // last close of the month it trades now. Weights beyond the run's gross or
    // net budget are refused.
    std::map<std::size_t, std::int64_t> weights(
        const std::map<std::size_t, std::int64_t>& wanted,
        std::int64_t gross,
        std::int64_t net,
        std::int64_t max_margin_ratio = 0,
        std::int64_t min_rebalance_weight = 0)
    {
        const auto& execution = replay_->execution();
        const auto& account = execution.account();
        std::vector<std::optional<Replay::Bar>> latest_by_unit(replay_->units());
        for (std::size_t c = 0; c < account.contracts().size(); ++c) {
            if (const auto& bar = replay_->last(c); bar && bar->leading) {
                auto& slot = latest_by_unit[bar->unit];
                if (!slot || bar->index > slot->index)
                    slot = bar;
            }
        }
        std::unordered_map<std::size_t, Decimal> held_by_contract;
        for (const auto& lot : account.positions()) {
            auto& qty = held_by_contract[account.contract_index(lot.instrument)];
            qty = qty + (lot.side == Side::buy ? lot.quantity : Decimal{} - lot.quantity);
        }
        std::vector<WeightedContract> members;
        std::vector<std::size_t> units;
        members.reserve(wanted.size());
        units.reserve(wanted.size());
        for (const auto& [unit, weight] : wanted) {
            const auto& latest = unit < latest_by_unit.size() ? latest_by_unit[unit] : std::nullopt;
            if (!latest)
                throw std::invalid_argument("a weighted symbol has had no bar yet");
            const auto& contract_terms = execution.contract(latest->contract).terms;
            const auto& terms = contract_terms.instrument;
            const auto& costs = account.contracts().at(latest->contract).costs;
            const auto margin_rate = weight < 0 ? costs.short_margin_rate : costs.margin_rate;
            const auto found_held = held_by_contract.find(latest->contract);
            const auto net_held =
                found_held == held_by_contract.end() ? Decimal{} : found_held->second;
            members.push_back(
                {terms.id,
                    Decimal::from_raw(weight),
                    latest->bar.close,
                    terms.multiplier,
                    margin_rate,
                    net_held});
            units.push_back(unit);
        }
        const auto targets = fixed_weight_targets(
            account.balance() + account.unrealized(),
            members,
            Decimal::from_raw(gross),
            Decimal::from_raw(net),
            {},
            Decimal::from_raw(max_margin_ratio),
            Decimal::from_raw(min_rebalance_weight));
        std::map<std::size_t, std::int64_t> result;
        for (std::size_t m = 0; m < units.size(); ++m)
            result.emplace(units[m], targets.members[m].quantity.raw());
        return result;
    }

    void redecide()
    {
        replay_->redecide();
    }

    void settle()
    {
        replay_->settle(record_);
    }

    const Replay& replay() const
    {
        return *replay_;
    }

    const Record& record() const
    {
        return record_;
    }

    // (id, contract, side, offset, lots, limit, state, filled, time_ns, trading_day)
    py::list orders() const
    {
        const auto& account = replay_->execution().account();
        py::list result;
        for (std::size_t n = 0; n < account.orders().size(); ++n) {
            const auto& item = account.orders()[n];
            const auto& request = item.order.request();
            result.append(
                py::make_tuple(
                    request.id,
                    contract_index(request.instrument),
                    side_name(request.side),
                    offset_name(item.offset),
                    request.quantity.raw(),
                    request.limit_price.raw(),
                    state_name(item.order.state()),
                    item.order.filled_quantity().raw(),
                    record_.order_times.at(n),
                    record_.order_days.at(n)));
        }
        return result;
    }

    // (id, order, lots, price, fee, time_ns, trading_day)
    py::list fills() const
    {
        const auto& account = replay_->execution().account();
        py::list result;
        for (std::size_t n = 0; n < account.fills().size(); ++n) {
            const auto& fill = account.fills()[n];
            result.append(
                py::make_tuple(
                    fill.execution.execution_id,
                    fill.execution.order_id,
                    fill.execution.quantity.raw(),
                    fill.execution.price.raw(),
                    fill.fee.raw(),
                    record_.fill_times.at(n),
                    record_.fill_days.at(n)));
        }
        return result;
    }

    py::list equity() const
    {
        py::list result;
        for (const auto& point : record_.equity)
            result.append(py::make_tuple(point.time_ns, point.equity, point.event));
        return result;
    }

    py::list settlements() const
    {
        py::list result;
        for (const auto& row : record_.settlements)
            result.append(
                py::make_tuple(
                    row.trading_day,
                    row.time_ns,
                    row.balance,
                    row.equity,
                    row.realized,
                    row.fees,
                    row.margin,
                    row.available));
        return result;
    }

    py::list positions() const
    {
        py::list result;
        for (const auto& row : record_.positions)
            result.append(
                py::make_tuple(
                    row.trading_day,
                    row.contract,
                    row.held_long,
                    row.held_short,
                    row.price,
                    row.margin));
        return result;
    }

    py::dict table_columns() const
    {
        const auto& account = replay_->execution().account();
        const auto make_i64 = [](std::size_t n) {
            return py::array_t<std::int64_t>(static_cast<py::ssize_t>(n));
        };

        // equity
        const auto n_eq = record_.equity.size();
        auto eq_ts = make_i64(n_eq);
        auto eq_val = make_i64(n_eq);
        py::list eq_ev(n_eq);
        auto* eq_ts_p = eq_ts.mutable_data();
        auto* eq_val_p = eq_val.mutable_data();
        for (std::size_t i = 0; i < n_eq; ++i) {
            eq_ts_p[i] = record_.equity[i].time_ns;
            eq_val_p[i] = record_.equity[i].equity;
            eq_ev[i] = py::str(record_.equity[i].event);
        }

        // orders
        const auto& ords = account.orders();
        const auto n_ord = ords.size();
        py::list ord_id(n_ord), ord_day(n_ord), ord_side(n_ord), ord_off(n_ord), ord_state(n_ord);
        auto ord_ts = make_i64(n_ord);
        auto ord_c = make_i64(n_ord);
        auto ord_qty = make_i64(n_ord);
        auto ord_px = make_i64(n_ord);
        auto* ord_ts_p = ord_ts.mutable_data();
        auto* ord_c_p = ord_c.mutable_data();
        auto* ord_qty_p = ord_qty.mutable_data();
        auto* ord_px_p = ord_px.mutable_data();
        std::unordered_map<std::string, std::size_t> order_pos;
        order_pos.reserve(n_ord);
        for (std::size_t i = 0; i < n_ord; ++i) {
            const auto& item = ords[i];
            const auto& req = item.order.request();
            order_pos.emplace(req.id, i);
            ord_id[i] = py::str(req.id);
            ord_ts_p[i] = record_.order_times.at(i);
            ord_day[i] = py::str(record_.order_days.at(i));
            ord_c_p[i] = static_cast<std::int64_t>(contract_index(req.instrument));
            ord_side[i] = py::str(side_name(req.side));
            ord_off[i] = py::str(offset_name(item.offset));
            ord_qty_p[i] = req.quantity.raw();
            ord_px_p[i] = req.limit_price.raw();
            ord_state[i] = py::str(state_name(item.order.state()));
        }

        // fills
        const auto& fls = account.fills();
        const auto n_fl = fls.size();
        py::list fl_id(n_fl), fl_ord(n_fl), fl_day(n_fl), fl_side(n_fl), fl_off(n_fl);
        auto fl_ts = make_i64(n_fl);
        auto fl_c = make_i64(n_fl);
        auto fl_qty = make_i64(n_fl);
        auto fl_px = make_i64(n_fl);
        auto fl_fee = make_i64(n_fl);
        auto* fl_ts_p = fl_ts.mutable_data();
        auto* fl_c_p = fl_c.mutable_data();
        auto* fl_qty_p = fl_qty.mutable_data();
        auto* fl_px_p = fl_px.mutable_data();
        auto* fl_fee_p = fl_fee.mutable_data();
        for (std::size_t i = 0; i < n_fl; ++i) {
            const auto& fill = fls[i];
            const auto pos = order_pos.at(fill.execution.order_id);
            const auto& ord_item = ords[pos];
            fl_id[i] = py::str(fill.execution.execution_id);
            fl_ord[i] = py::str(fill.execution.order_id);
            fl_ts_p[i] = record_.fill_times.at(i);
            fl_day[i] = py::str(record_.fill_days.at(i));
            fl_c_p[i] = ord_c_p[pos];
            fl_side[i] = py::str(side_name(ord_item.order.request().side));
            fl_off[i] = py::str(offset_name(ord_item.offset));
            fl_qty_p[i] = fill.execution.quantity.raw();
            fl_px_p[i] = fill.execution.price.raw();
            fl_fee_p[i] = fill.fee.raw();
        }

        // positions
        const auto n_pos = record_.positions.size();
        py::list pos_day(n_pos);
        auto pos_c = make_i64(n_pos);
        auto pos_long = make_i64(n_pos);
        auto pos_short = make_i64(n_pos);
        auto pos_px = make_i64(n_pos);
        auto pos_mgn = make_i64(n_pos);
        auto* pos_c_p = pos_c.mutable_data();
        auto* pos_long_p = pos_long.mutable_data();
        auto* pos_short_p = pos_short.mutable_data();
        auto* pos_px_p = pos_px.mutable_data();
        auto* pos_mgn_p = pos_mgn.mutable_data();
        for (std::size_t i = 0; i < n_pos; ++i) {
            const auto& row = record_.positions[i];
            pos_day[i] = py::str(row.trading_day);
            pos_c_p[i] = static_cast<std::int64_t>(row.contract);
            pos_long_p[i] = row.held_long;
            pos_short_p[i] = row.held_short;
            pos_px_p[i] = row.price;
            pos_mgn_p[i] = row.margin;
        }

        // settlements
        const auto n_set = record_.settlements.size();
        py::list set_day(n_set);
        auto set_ts = make_i64(n_set);
        auto set_bal = make_i64(n_set);
        auto set_eq = make_i64(n_set);
        auto set_real = make_i64(n_set);
        auto set_fee = make_i64(n_set);
        auto set_mgn = make_i64(n_set);
        auto set_avail = make_i64(n_set);
        auto* set_ts_p = set_ts.mutable_data();
        auto* set_bal_p = set_bal.mutable_data();
        auto* set_eq_p = set_eq.mutable_data();
        auto* set_real_p = set_real.mutable_data();
        auto* set_fee_p = set_fee.mutable_data();
        auto* set_mgn_p = set_mgn.mutable_data();
        auto* set_avail_p = set_avail.mutable_data();
        for (std::size_t i = 0; i < n_set; ++i) {
            const auto& row = record_.settlements[i];
            set_day[i] = py::str(row.trading_day);
            set_ts_p[i] = row.time_ns;
            set_bal_p[i] = row.balance;
            set_eq_p[i] = row.equity;
            set_real_p[i] = row.realized;
            set_fee_p[i] = row.fees;
            set_mgn_p[i] = row.margin;
            set_avail_p[i] = row.available;
        }

        py::dict out;
        out["equity"] = py::make_tuple(eq_ts, eq_val, eq_ev);
        out["orders"] = py::make_tuple(
            ord_id,
            ord_ts,
            ord_day,
            ord_c,
            ord_side,
            ord_off,
            ord_qty,
            ord_px,
            ord_state);
        out["fills"] = py::make_tuple(
            fl_id,
            fl_ord,
            fl_ts,
            fl_day,
            fl_c,
            fl_side,
            fl_off,
            fl_qty,
            fl_px,
            fl_fee);
        out["positions"] = py::make_tuple(pos_day, pos_c, pos_long, pos_short, pos_px, pos_mgn);
        out["settlements"] =
            py::make_tuple(set_day, set_ts, set_bal, set_eq, set_real, set_fee, set_mgn, set_avail);
        return out;
    }

    py::list rejections() const
    {
        py::list result;
        for (const auto& row : record_.rejections)
            result.append(
                py::make_tuple(row.time_ns, row.trading_day, row.contract, row.lots, row.reason));
        return result;
    }

private:
    std::size_t contract_index(const InstrumentId& id) const
    {
        return replay_->execution().account().contract_index(id);
    }

    std::unique_ptr<Replay> replay_;
    Record record_;
};

// The old engine's statistics of an equity record. `days`: (trading_day,
// settlement equity) ascending; `marks`: every equity observation in order;
// `begin_ns` and `end_ns` bound what the record observed. A statistic without
// a value carries why.
py::dict statistics(
    std::int64_t base,
    const std::vector<std::pair<std::string, std::int64_t>>& days,
    const std::vector<std::int64_t>& marks,
    std::int64_t begin_ns,
    std::int64_t end_ns,
    std::optional<double> annual_periods,
    bool zero_risk_free,
    bool opening_is_full_period)
{
    std::vector<EquityDay> equity;
    for (const auto& [day, value] : days)
        equity.push_back(
            {std::chrono::sys_days(parse_trading_date(day)), Decimal::from_raw(value)});
    std::vector<Decimal> observed;
    for (const auto value : marks)
        observed.push_back(Decimal::from_raw(value));
    PerformanceContext context;
    context.begin_ns = begin_ns;
    context.end_ns = end_ns;
    context.annual_periods = annual_periods;
    context.explicitly_zero_risk_free = zero_risk_free;
    context.opening_is_full_period = opening_is_full_period;
    const auto result = performance(Decimal::from_raw(base), equity, observed, context);
    py::dict values;
    values["trading_days"] = result.trading_days;
    values["total_return"] = result.total_return;
    values["max_drawdown"] = result.max_drawdown;
    values["winning_days"] = result.winning_days;
    const auto optional =
        [&](const char* name, const std::optional<double>& value, PerformanceUnavailable why) {
            values[name] = value ? py::cast(*value) : py::none();
            values[(std::string(name) + "_unavailable").c_str()] =
                value ? py::none() : py::cast(std::string(performance_unavailable_name(why)));
        };
    optional("annual_return", result.annual_return, result.annual_return_reason);
    optional("annual_volatility", result.annual_volatility, result.annual_volatility_reason);
    optional("sharpe", result.sharpe, result.sharpe_reason);
    optional("calmar", result.calmar, result.calmar_reason);
    return values;
}

std::int64_t floor_div(std::int64_t a, std::int64_t b) noexcept
{
    std::int64_t q = a / b;
    std::int64_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0)))
        --q;
    return q;
}

int pos_mod(std::int64_t a, int m) noexcept
{
    int r = static_cast<int>(a % m);
    return r < 0 ? r + m : r;
}

py::tuple aggregate_minutes(
    const py::array_t<std::int32_t, py::array::c_style | py::array::forcecast>& day_ids,
    const py::array_t<std::int64_t, py::array::c_style | py::array::forcecast>& start_ms,
    const py::array_t<std::uint8_t, py::array::c_style | py::array::forcecast>& src_partial,
    const py::array_t<std::uint8_t, py::array::c_style | py::array::forcecast>& ohlc_valid,
    const py::array_t<std::int64_t, py::array::c_style | py::array::forcecast>& open_raw,
    const py::array_t<std::int64_t, py::array::c_style | py::array::forcecast>& high_raw,
    const py::array_t<std::int64_t, py::array::c_style | py::array::forcecast>& low_raw,
    const py::array_t<std::int64_t, py::array::c_style | py::array::forcecast>& close_raw,
    const py::array_t<std::uint8_t, py::array::c_style | py::array::forcecast>& vol_valid,
    const py::array_t<std::int64_t, py::array::c_style | py::array::forcecast>& vol_raw,
    const py::array_t<double, py::array::c_style | py::array::forcecast>& oi_f64,
    int minutes,
    const std::vector<std::pair<int, int>>& sessions,
    int num_days)
{
    if (minutes != 1 && minutes != 5 && minutes != 10 && minutes != 15 && minutes != 30
        && minutes != 60)
        throw std::invalid_argument("minute periods are 1, 5, 10, 15, 30, 60");
    const auto n = day_ids.size();
    if (start_ms.size() != n || src_partial.size() != n || ohlc_valid.size() != n
        || open_raw.size() != n || high_raw.size() != n || low_raw.size() != n
        || close_raw.size() != n || vol_valid.size() != n || vol_raw.size() != n
        || oi_f64.size() != n)
        throw std::invalid_argument("minute aggregation inputs must have equal length");

    std::array<std::uint64_t, 1440> expected_mask{};
    const bool has_sessions = !sessions.empty();
    for (const auto& [start, end] : sessions) {
        const int stop = start < end ? end : end + 1440;
        for (int slot = start; slot < stop; ++slot) {
            const int m = slot % 1440;
            expected_mask[static_cast<std::size_t>(m / minutes)] |=
                (std::uint64_t{1} << (m % minutes));
        }
    }
    const std::uint64_t full_mask = (std::uint64_t{1} << minutes) - 1;

    std::vector<int> openings;
    for (const auto& [start, end] : sessions) {
        if (start == sessions.front().first || start >= 18 * 60)
            openings.push_back(start);
    }
    std::array<int, 1440> until_open{};
    for (int m = 0; m < 1440; ++m) {
        for (int opening : openings) {
            if (opening - 5 <= m && m < opening) {
                until_open[static_cast<std::size_t>(m)] = opening - m;
                break;
            }
        }
    }

    const auto* d_ptr = day_ids.data();
    const auto* s_ptr = start_ms.data();
    const auto* sp_ptr = src_partial.data();
    const auto* ov_ptr = ohlc_valid.data();
    const auto* o_ptr = open_raw.data();
    const auto* h_ptr = high_raw.data();
    const auto* l_ptr = low_raw.data();
    const auto* c_ptr = close_raw.data();
    const auto* vv_ptr = vol_valid.data();
    const auto* v_ptr = vol_raw.data();
    const auto* oi_ptr = oi_f64.data();

    const std::int64_t span = static_cast<std::int64_t>(minutes) * 60'000;
    std::vector<std::int32_t> zero_vol_by_day(static_cast<std::size_t>(std::max(0, num_days)), 0);

    std::vector<std::int32_t> obs_day;
    std::vector<std::int64_t> obs_end_ms;
    std::vector<std::uint8_t> obs_partial;

    std::vector<std::int32_t> bar_day;
    std::vector<std::int64_t> bar_end_ms;
    std::vector<std::int64_t> bar_open, bar_high, bar_low, bar_close, bar_vol;
    std::vector<double> bar_oi;
    std::vector<std::uint8_t> bar_partial;

    const std::size_t est = static_cast<std::size_t>(n / minutes + 16);
    obs_day.reserve(est);
    obs_end_ms.reserve(est);
    obs_partial.reserve(est);
    bar_day.reserve(est);
    bar_end_ms.reserve(est);
    bar_open.reserve(est);
    bar_high.reserve(est);
    bar_low.reserve(est);
    bar_close.reserve(est);
    bar_vol.reserve(est);
    bar_oi.reserve(est);
    bar_partial.reserve(est);

    bool in_group = false;
    std::int32_t cur_day = -1;
    std::int64_t cur_bucket = 0;
    std::uint64_t observed_mask = 0;
    bool partial = false;
    bool has_bar = false;
    std::int64_t b_open = 0, b_high = 0, b_low = 0, b_close = 0, b_vol = 0;
    double last_oi = std::numeric_limits<double>::quiet_NaN();

    const auto flush_group = [&]() {
        if (!in_group)
            return;
        const std::int64_t end_ms = (cur_bucket + 1) * span;
        const int local_start = pos_mod(cur_bucket * minutes + 480, 1440);
        const std::uint64_t exp_mask = has_sessions
            ? expected_mask[static_cast<std::size_t>(local_start / minutes)]
            : full_mask;
        if (exp_mask == 0 || observed_mask != exp_mask)
            partial = true;
        obs_day.push_back(cur_day);
        obs_end_ms.push_back(end_ms);
        obs_partial.push_back(partial ? 1 : 0);
        if (has_bar) {
            bar_day.push_back(cur_day);
            bar_end_ms.push_back(end_ms);
            bar_open.push_back(b_open);
            bar_high.push_back(b_high);
            bar_low.push_back(b_low);
            bar_close.push_back(b_close);
            bar_vol.push_back(b_vol);
            bar_oi.push_back(last_oi);
            bar_partial.push_back(partial ? 1 : 0);
        }
        in_group = false;
    };

    for (py::ssize_t i = 0; i < n; ++i) {
        const std::int32_t d = d_ptr[i];
        const bool v_ok = vv_ptr[i] != 0;
        const std::int64_t vol = v_ok ? v_ptr[i] : 0;
        if (d >= 0 && static_cast<std::size_t>(d) < zero_vol_by_day.size() && v_ok && vol == 0)
            ++zero_vol_by_day[static_cast<std::size_t>(d)];

        const std::int64_t st = s_ptr[i];
        const std::int64_t start_min = floor_div(st, 60'000);
        const int local_min = pos_mod(start_min + 480, 1440);
        const int u = until_open[static_cast<std::size_t>(local_min)];
        // A zero-volume auction row carries a quote that never traded.
        if (u != 0 && (!v_ok || vol == 0))
            continue;
        const std::int64_t bucket = floor_div(st + static_cast<std::int64_t>(u) * 60'000, span);
        if (!in_group || d != cur_day || bucket != cur_bucket) {
            flush_group();
            in_group = true;
            cur_day = d;
            cur_bucket = bucket;
            observed_mask = 0;
            partial = false;
            has_bar = false;
            last_oi = std::numeric_limits<double>::quiet_NaN();
        }
        if (u == 0)
            observed_mask |= (std::uint64_t{1} << pos_mod(start_min, minutes));
        last_oi = oi_ptr[i];
        if (sp_ptr[i] != 0)
            partial = true;
        if (ov_ptr[i] == 0) {
            if (!v_ok || vol != 0)
                partial = true;
            continue;
        }
        if (!has_bar) {
            has_bar = true;
            b_open = o_ptr[i];
            b_high = h_ptr[i];
            b_low = l_ptr[i];
            b_vol = 0;
        }
        if (h_ptr[i] > b_high)
            b_high = h_ptr[i];
        if (l_ptr[i] < b_low)
            b_low = l_ptr[i];
        b_close = c_ptr[i];
        if (v_ok)
            b_vol += vol;
    }
    flush_group();

    const auto to_arr = [](const auto& vec) {
        using T = typename std::decay_t<decltype(vec)>::value_type;
        py::array_t<T> out(static_cast<py::ssize_t>(vec.size()));
        if (!vec.empty())
            std::copy(vec.begin(), vec.end(), out.mutable_data());
        return out;
    };

    return py::make_tuple(
        to_arr(bar_day),
        to_arr(bar_end_ms),
        to_arr(bar_open),
        to_arr(bar_high),
        to_arr(bar_low),
        to_arr(bar_close),
        to_arr(bar_vol),
        to_arr(bar_oi),
        to_arr(bar_partial),
        to_arr(obs_day),
        to_arr(obs_end_ms),
        to_arr(obs_partial),
        to_arr(zero_vol_by_day));
}

} // namespace
} // namespace asterion

PYBIND11_MODULE(asterion_engine, module)
{
    using asterion::Engine;
    module.doc() = "core's backtest Replay; amounts are raw eight-place decimals";
    module.def("trading_sessions", [](const std::string& venue, const std::string& product) {
        return asterion::trading_sessions({venue, product});
    });
    module.def("aggregate_minutes", &asterion::aggregate_minutes);
    module.def(
        "performance",
        &asterion::statistics,
        py::arg("base"),
        py::arg("days"),
        py::arg("marks"),
        py::arg("begin_ns"),
        py::arg("end_ns"),
        py::arg("annual_periods"),
        py::arg("zero_risk_free"),
        py::arg("opening_is_full_period") = false);
    py::class_<Engine>(module, "Replay")
        .def(
            py::init<
                std::int64_t,
                const py::list&,
                const py::list&,
                const py::tuple&,
                const py::dict&>(),
            py::arg("deposit"),
            py::arg("contracts"),
            py::arg("series"),
            py::arg("limits"),
            py::arg("margins") = py::dict())
        .def_property_readonly(
            "units",
            [](const Engine& e) {
                return e.replay().units();
            })
        .def_property_readonly(
            "finished",
            [](const Engine& e) {
                return e.replay().finished();
            })
        .def_property_readonly(
            "day_done",
            [](const Engine& e) {
                return e.replay().day_done();
            })
        .def_property_readonly(
            "trading_day",
            [](const Engine& e) {
                return e.replay().day().trading_day;
            })
        .def(
            "unit",
            [](const Engine& e, std::size_t contract) {
                return e.replay().unit(contract);
            })
        .def(
            "series",
            [](const Engine& e, std::size_t contract) {
                return e.replay().series(contract);
            })
        .def("next", &Engine::next)
        .def("next_leading", &Engine::next_leading)
        .def("want", &Engine::want, py::arg("unit"), py::arg("lots"))
        .def("decide", &Engine::decide)
        .def(
            "weights",
            &Engine::weights,
            py::arg("weights"),
            py::arg("gross"),
            py::arg("net"),
            py::arg("max_margin_ratio") = 0,
            py::arg("min_rebalance_weight") = 0)
        .def("redecide", &Engine::redecide)
        .def("settle", &Engine::settle)
        .def("orders", &Engine::orders)
        .def("fills", &Engine::fills)
        .def("table_columns", &Engine::table_columns)
        .def_property_readonly("equity", &Engine::equity)
        .def_property_readonly("settlements", &Engine::settlements)
        .def_property_readonly("positions", &Engine::positions)
        .def_property_readonly("rejections", &Engine::rejections);
}
