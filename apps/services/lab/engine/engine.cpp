// asterion_engine: core's Replay for the lab's Python backtests. Amounts and
// prices cross as the raw int64 of an eight-place Decimal, never as floats.
#include <asterion/futures/order_limits.hpp>
#include <asterion/futures/replay.hpp>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

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
        const auto values = version["values"].cast<std::vector<std::int64_t>>();
        if (values.size() != 8)
            throw std::invalid_argument("costs have eight values");
        FuturesCosts costs{
            Decimal::from_raw(values[0]),
            Decimal::from_raw(values[1]),
            Decimal::from_raw(values[2]),
            Decimal::from_raw(values[3]),
            Decimal::from_raw(values[4]),
            Decimal::from_raw(values[5]),
            Decimal::from_raw(values[6]),
            Decimal::from_raw(values[7])
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
    // (trading_day, timestamp_ns, open, high, low, close, volume)
    for (const auto& bar : item["bars"].cast<py::list>()) {
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
    for (const auto& day : item["settlements"].cast<py::list>())
        days.push_back({day[py::int_(0)].cast<std::string>(), raw(day[py::int_(1)])});
    return contract;
}

// What the replay did, kept for the run's tables.
struct Record final : Replay::Observer
{
    const Replay* replay = nullptr;
    // (time_ns, equity, event)
    std::vector<py::tuple> equity;
    // (trading_day, time_ns, balance, equity, realized, fees, margin, available)
    std::vector<py::tuple> settlements;
    // (trading_day, contract, long, short, settlement_price, margin)
    std::vector<py::tuple> positions;
    // (event, intent, lots, reason)
    std::vector<py::tuple> rejections;
    // When each order was placed and each fill made.
    std::vector<std::int64_t> order_times, fill_times;

    std::int64_t value() const
    {
        const auto& account = replay->execution().account();
        return (account.balance() + account.unrealized()).raw();
    }

    void advanced(
        std::size_t index,
        const std::optional<PaperExecution::Target>& intent,
        const RiskDecision& decision,
        std::size_t) override
    {
        const auto& execution = replay->execution();
        const auto time = execution.bar(execution.event(index)).timestamp_ns;
        order_times.resize(execution.account().orders().size(), time);
        fill_times.resize(execution.account().fills().size(), time);
        if (!decision.allowed() && intent)
            rejections.push_back(
                py::make_tuple(
                    index,
                    intent->order_id,
                    intent->quantity.raw(),
                    std::string(risk_reason_name(decision.reason))));
    }

    void marked(std::size_t index) override
    {
        const auto& execution = replay->execution();
        equity.push_back(
            py::make_tuple(execution.bar(execution.event(index)).timestamp_ns, value(), "mark"));
    }

    void settled(
        const PaperReplaySchedule::Day& day,
        std::int64_t at,
        const std::optional<PaperExecution::MarginRevaluation>&) override
    {
        const auto& account = replay->execution().account();
        equity.push_back(py::make_tuple(at, value(), "settlement"));
        settlements.push_back(
            py::make_tuple(
                day.trading_day,
                at,
                account.balance().raw(),
                value(),
                account.realized().raw(),
                account.fees().raw(),
                account.margin().raw(),
                account.available().raw()));
        for (std::size_t c = 0; c < account.contracts().size(); ++c) {
            if (!day.prices[c])
                continue;
            const auto& terms = account.contracts()[c];
            Decimal held_long, held_short, margin;
            for (const auto& lot : account.positions())
                if (lot.instrument == terms.instrument.id) {
                    (lot.side == Side::buy ? held_long : held_short) =
                        (lot.side == Side::buy ? held_long : held_short) + lot.quantity;
                    margin = margin
                        + terms.costs.margin(
                            lot.quantity,
                            *day.prices[c],
                            terms.instrument.multiplier);
                }
            if (held_long != Decimal{} || held_short != Decimal{})
                positions.push_back(
                    py::make_tuple(
                        day.trading_day,
                        c,
                        held_long.raw(),
                        held_short.raw(),
                        day.prices[c]->raw(),
                        margin.raw()));
        }
    }
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
                    {roll[py::int_(0)].cast<std::string>(),
                        roll[py::int_(1)].cast<std::size_t>(),
                        raw(roll[py::int_(2)])});
        }
        if (limits.size() != 3)
            throw std::invalid_argument("limits are order lots, gross lots and working orders");
        OrderLimitsConfig config{raw(limits[0]), raw(limits[1]), limits[2].cast<std::size_t>()};
        config.validate();
        auto risk = std::make_shared<OrderLimits>(config);
        risk->start();
        std::map<std::string, Decimal> revalued;
        for (const auto& [day, rate] : margins)
            revalued.emplace(day.cast<std::string>(), raw(rate));
        replay_ = std::make_unique<Replay>(
            Decimal::from_raw(deposit),
            std::move(items),
            days,
            std::move(rolls),
            std::move(risk),
            std::move(revalued));
        record_.replay = replay_.get();
    }

    // The bars of the next timestamp: (event, contract, unit, leading,
    // trading_day, timestamp_ns, open, high, low, close, volume, raw close),
    // prices as a strategy sees them.
    py::list next()
    {
        py::list result;
        for (const auto& bar : replay_->next(record_)) {
            const auto& seen = bar.seen;
            result.append(
                py::make_tuple(
                    bar.index,
                    bar.contract,
                    bar.unit,
                    bar.leading,
                    seen.trading_day,
                    seen.timestamp_ns,
                    seen.open.raw(),
                    seen.high.raw(),
                    seen.low.raw(),
                    seen.close.raw(),
                    seen.volume.raw(),
                    bar.close.raw()));
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

    // (id, contract, side, offset, lots, limit, state, filled, time_ns)
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
                    record_.order_times.at(n)));
        }
        return result;
    }

    // (id, order, lots, price, fee, time_ns)
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
                    record_.fill_times.at(n)));
        }
        return result;
    }

private:
    std::size_t contract_index(const InstrumentId& id) const
    {
        const auto& contracts = replay_->execution().account().contracts();
        for (std::size_t c = 0; c < contracts.size(); ++c)
            if (contracts[c].instrument.id == id)
                return c;
        throw std::logic_error("an order names no contract of the account");
    }

    std::unique_ptr<Replay> replay_;
    Record record_;
};

} // namespace
} // namespace asterion

PYBIND11_MODULE(asterion_engine, module)
{
    using asterion::Engine;
    module.doc() = "core's backtest Replay; amounts are raw eight-place decimals";
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
        .def("want", &Engine::want, py::arg("unit"), py::arg("lots"))
        .def("decide", &Engine::decide)
        .def("settle", &Engine::settle)
        .def("orders", &Engine::orders)
        .def("fills", &Engine::fills)
        .def_property_readonly(
            "equity",
            [](const Engine& e) {
                return e.record().equity;
            })
        .def_property_readonly(
            "settlements",
            [](const Engine& e) {
                return e.record().settlements;
            })
        .def_property_readonly(
            "positions",
            [](const Engine& e) {
                return e.record().positions;
            })
        .def_property_readonly("rejections", [](const Engine& e) {
            return e.record().rejections;
        });
}
