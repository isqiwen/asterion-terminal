#include <asterion/futures/replay.hpp>
#include <asterion/futures/trading_days.hpp>
#include <algorithm>
#include <stdexcept>

namespace asterion {
namespace {

constexpr std::size_t none = static_cast<std::size_t>(-1);

std::vector<std::size_t> series_of(std::size_t contracts, const std::vector<DominantSeries>& series)
{
    std::vector<std::size_t> result(contracts, none);
    for (std::size_t s = 0; s < series.size(); ++s) {
        if (series[s].rolls.empty())
            throw std::invalid_argument("a dominant series needs a roll");
        for (const auto& roll : series[s].rolls)
            result.at(roll.contract) = s;
    }
    return result;
}

std::vector<PaperExecution::Event> ordered_events(const std::vector<ContractBars>& contracts)
{
    std::vector<const std::vector<MarketBar>*> bars;
    bars.reserve(contracts.size());
    for (const auto& contract : contracts)
        bars.push_back(&contract.bars);
    return replay_order(bars);
}

PaperReplaySchedule schedule_of(
    const std::vector<ContractBars>& contracts,
    std::span<const PaperExecution::Event> events,
    const std::vector<std::vector<DaySettlement>>& days,
    const std::vector<std::size_t>& series)
{
    std::vector<Instrument> instruments;
    instruments.reserve(contracts.size());
    for (const auto& contract : contracts)
        instruments.push_back(contract.terms.instrument);
    std::vector<std::string> event_days;
    std::vector<std::size_t> event_contracts;
    event_days.reserve(events.size());
    event_contracts.reserve(events.size());
    for (const auto& event : events) {
        event_days.push_back(contracts[event.contract].bars[event.bar].trading_day);
        event_contracts.push_back(event.contract);
    }
    std::vector<bool> sparse;
    sparse.reserve(series.size());
    for (const auto item : series)
        sparse.push_back(item != none);
    return PaperReplaySchedule(instruments, event_days, event_contracts, days, sparse);
}

} // namespace

const DominantRoll& DominantSeries::on(const std::string& trading_day) const
{
    const auto next = std::upper_bound(
        rolls.begin(),
        rolls.end(),
        trading_day,
        [](const std::string& day, const DominantRoll& roll) {
            return day < roll.trading_day;
        });
    return next == rolls.begin() ? rolls.at(0) : *std::prev(next);
}

Replay::Prepared Replay::prepare(std::vector<ContractBars> contracts)
{
    auto events = ordered_events(contracts);
    return {std::move(contracts), std::move(events)};
}

Replay::Replay(
    Decimal deposit,
    std::vector<ContractBars> contracts,
    const std::vector<std::vector<DaySettlement>>& days,
    std::vector<DominantSeries> series,
    OrderLimitsConfig limits,
    std::map<std::string, Decimal> margins)
    : Replay(
          deposit,
          prepare(std::move(contracts)),
          days,
          std::move(series),
          limits,
          std::move(margins))
{}

Replay::Replay(
    Decimal deposit,
    Prepared prepared,
    const std::vector<std::vector<DaySettlement>>& days,
    std::vector<DominantSeries> series,
    OrderLimitsConfig limits,
    std::map<std::string, Decimal> margins)
    : series_(series_of(prepared.contracts.size(), series)),
      unit_(prepared.contracts.size()),
      rolls_(std::move(series)),
      schedule_(schedule_of(prepared.contracts, prepared.events, days, series_)),
      execution_(deposit, std::move(prepared.contracts), limits, std::move(prepared.events)),
      margins_(std::move(margins)),
      pending_(series_.size()),
      last_(series_.size()),
      took_over_(series_.size())
{
    for (std::size_t c = 0; c < series_.size(); ++c)
        if (series_[c] == none)
            unit_[c] = units_++;
    for (std::size_t c = 0; c < series_.size(); ++c)
        if (series_[c] != none)
            unit_[c] = units_ + series_[c];
    units_ += rolls_.size();
    wanted_.resize(units_);
    // The first month of a series has none to take over from and trades as any contract does.
    for (const auto& series : rolls_)
        took_over_[series.rolls.front().contract] = true;
    execution_.start();
    if (!finished())
        settled_at_ = trading_day_close_ns(day().trading_day);
}

std::optional<std::size_t> Replay::series(std::size_t contract) const
{
    const auto item = series_.at(contract);
    return item == none ? std::nullopt : std::optional{item};
}

bool Replay::day_done() const
{
    return index_ == execution_.size() || schedule_.event(index_).day != day_;
}

std::optional<Decimal> Replay::target(std::size_t contract) const
{
    return wanted_[unit_[contract]];
}

Decimal Replay::held(std::size_t contract) const
{
    const auto& id = execution_.contract(contract).terms.instrument.id;
    Decimal total;
    for (const auto& lot : execution_.account().positions())
        if (lot.instrument == id)
            total = total + (lot.side == Side::buy ? lot.quantity : Decimal{} - lot.quantity);
    return total;
}

bool Replay::holds(std::size_t contract) const
{
    return held(contract) != Decimal{};
}

const std::vector<Replay::Bar>& Replay::next(Observer& observer, std::stop_token stop)
{
    if (finished() || day_done())
        throw std::logic_error("the trading day has no more bars");
    bars_.clear();
    std::vector<std::optional<PaperExecution::Target>> intents;
    const auto timestamp = execution_.bar(execution_.event(index_)).timestamp_ns;
    for (auto index = index_; index < execution_.size() && schedule_.event(index).day == day_
        && execution_.bar(execution_.event(index)).timestamp_ns == timestamp;
        ++index) {
        if (stop.stop_requested())
            throw std::runtime_error("backtest cancelled");
        const auto current = execution_.event(index);
        const auto& bar = execution_.bar(current);
        auto intent = pending_[current.contract];
        if (intent)
            intent->limit_price = bar.open;
        const auto member = series_[current.contract];
        const auto* roll = member == none ? nullptr : &rolls_[member].on(bar.trading_day);
        const bool leading = !roll || roll->contract == current.contract;
        if (roll && target(current.contract)) {
            if (!leading) {
                if (holds(current.contract))
                    intent = PaperExecution::Target{
                        "roll." + std::to_string(index),
                        Decimal{},
                        bar.open
                    };
                else
                    intent.reset();
            } else if (!took_over_[current.contract]) {
                intent = PaperExecution::Target{
                    "roll." + std::to_string(index),
                    *target(current.contract),
                    bar.open
                };
            }
        }
        intents.push_back(intent);
        bars_.push_back({index, current.contract, unit_[current.contract], leading, bar});
    }
    const auto can_open = [&](std::size_t index) {
        const auto contract = execution_.event(index).contract;
        const auto member = series_[contract];
        if (member == none)
            return true;
        const auto& account = execution_.account();
        for (const auto& lot : account.positions()) {
            const auto held_contract = account.contract_index(lot.instrument);
            if (held_contract != contract && series_[held_contract] == member)
                return false;
        }
        return true;
    };
    const auto decisions = execution_.advance_targets(intents, can_open);
    for (std::size_t i = 0; i < bars_.size(); ++i) {
        const auto& bar = bars_[i];
        if (series_[bar.contract] != none && bar.leading && !took_over_[bar.contract]
            && can_open(bar.index))
            if (const auto quantity = target(bar.contract);
                quantity && held(bar.contract) == *quantity)
                took_over_[bar.contract] = true;
        observer.advanced(bar.index, intents[i], decisions[i]);
        pending_[bar.contract].reset();
        last_[bar.contract] = bar;
    }
    index_ += bars_.size();
    settled_at_ = timestamp;
    observer.marked(bars_.back().index);
    return bars_;
}

void Replay::want(std::size_t unit, std::optional<Decimal> lots)
{
    wanted_.at(unit) = lots;
    if (!lots)
        for (std::size_t contract = 0; contract < unit_.size(); ++contract)
            if (unit_[contract] == unit)
                pending_[contract].reset();
}

void Replay::issue(const Bar& bar)
{
    // None decides for a month that is not dominant: a roll closes that.
    if (!bar.leading)
        return;
    if (const auto quantity = target(bar.contract))
        pending_[bar.contract] = PaperExecution::Target{
            "strategy." + std::to_string(bar.index),
            *quantity,
            bar.bar.close
        };
}

void Replay::decide()
{
    for (const auto& bar : bars_)
        issue(bar);
}

void Replay::redecide()
{
    for (const auto& bar : last_)
        if (bar)
            issue(*bar);
}

void Replay::settle(Observer& observer)
{
    if (finished() || !day_done())
        throw std::logic_error("the trading day still has bars");
    execution_.cancel_open_orders();
    const auto& current = day();
    const auto margin = margins_.find(current.trading_day);
    const auto revaluation = execution_.settle_scheduled(
        current.trading_day,
        current.prices,
        margin == margins_.end() ? std::nullopt : std::optional{margin->second});
    observer.settled(current, settled_at_, revaluation);
    bars_.clear();
    if (++day_ < schedule_.days())
        // A day without bars is settled when its session closes.
        settled_at_ = trading_day_close_ns(day().trading_day);
}

} // namespace asterion
