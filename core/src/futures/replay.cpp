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

PaperReplaySchedule schedule_of(
    const std::vector<ContractBars>& contracts,
    const std::vector<std::vector<DaySettlement>>& days,
    const std::vector<std::size_t>& series)
{
    std::vector<Instrument> instruments;
    std::vector<const std::vector<MarketBar>*> bars;
    for (const auto& contract : contracts) {
        instruments.push_back(contract.terms.instrument);
        bars.push_back(&contract.bars);
    }
    std::vector<std::string> event_days;
    std::vector<std::size_t> event_contracts;
    for (const auto& event : replay_order(bars)) {
        event_days.push_back(contracts[event.contract].bars[event.bar].trading_day);
        event_contracts.push_back(event.contract);
    }
    std::vector<bool> sparse;
    for (const auto item : series)
        sparse.push_back(item != none);
    return PaperReplaySchedule(instruments, event_days, event_contracts, days, sparse);
}

} // namespace

const DominantRoll& DominantSeries::on(const std::string& trading_day) const
{
    const auto* current = &rolls.at(0);
    for (const auto& roll : rolls)
        if (roll.trading_day <= trading_day)
            current = &roll;
    return *current;
}

Decimal dominant_price(Decimal raw, Decimal factor, Decimal increment)
{
    return quantize(multiply(raw, factor, Rounding::half_up), increment, Rounding::half_up);
}

Replay::Replay(
    Decimal deposit,
    std::vector<ContractBars> contracts,
    const std::vector<std::vector<DaySettlement>>& days,
    std::vector<DominantSeries> series,
    std::shared_ptr<const RiskPort> risk,
    std::map<std::string, Decimal> margins)
    : series_(series_of(contracts.size(), series)),
      unit_(contracts.size()),
      rolls_(std::move(series)),
      schedule_(schedule_of(contracts, days, series_)),
      execution_(deposit, std::move(contracts), std::move(risk)),
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

bool Replay::holds(std::size_t contract) const
{
    const auto& id = execution_.contract(contract).terms.instrument.id;
    return std::ranges::any_of(execution_.account().positions(), [&](const auto& lot) {
        return lot.instrument == id;
    });
}

const std::vector<Replay::Bar>& Replay::next(Observer& observer, std::stop_token stop)
{
    if (finished() || day_done())
        throw std::logic_error("the trading day has no more bars");
    bars_.clear();
    const auto& account = execution_.account();
    const auto timestamp = execution_.bar(execution_.event(index_)).timestamp_ns;
    for (; !day_done() && execution_.bar(execution_.event(index_)).timestamp_ns == timestamp;
        ++index_) {
        if (stop.stop_requested())
            throw std::runtime_error("backtest cancelled");
        const auto current = execution_.event(index_);
        const auto& bar = execution_.bar(current);
        const auto& instrument = execution_.contract(current.contract).terms.instrument;
        // Orders placed after a contract's bar N can only fill on its bar N+1;
        // unfilled remainders expire before its strategy decides again.
        auto& intent = pending_[current.contract];
        const auto member = series_[current.contract];
        const auto* roll = member == none ? nullptr : &rolls_[member].on(bar.trading_day);
        const bool leading = !roll || roll->contract == current.contract;
        if (roll) {
            // A roll moves what the series holds and decides nothing, so it waits
            // for no price: both months trade at the open of a bar, which every
            // bar reaches. On daily bars both happen on the roll day where both
            // months trade on it, the left month first.
            const bool others = std::ranges::any_of(rolls_[member].rolls, [&](const auto& other) {
                return other.contract != current.contract && holds(other.contract);
            });
            if (!leading)
                intent =
                    PaperExecution::Target{"roll." + std::to_string(index_), Decimal{}, bar.open};
            else if (others) {
                if (intent && intent->quantity != Decimal{})
                    intent.reset();
            } else if (!took_over_[current.contract] && account.priced(current.contract)) {
                if (const auto quantity = target(current.contract))
                    intent = PaperExecution::Target{
                        "roll." + std::to_string(index_),
                        *quantity,
                        bar.open
                    };
                took_over_[current.contract] = true;
            }
        }
        const auto previous_orders = account.orders().size();
        const auto decision = execution_.advance(intent);
        observer.advanced(index_, intent, decision, previous_orders);
        intent.reset();
        // Daily orders expire after their original bar. A target remains
        // evidence of intent; expiration does not turn it into a GTC order.
        execution_.cancel_open_orders(instrument.id);
        auto seen = bar;
        if (roll)
            // On the price increment of the bar's day, which the account trades at.
            for (auto* price : {&seen.open, &seen.high, &seen.low, &seen.close})
                *price = dominant_price(
                    *price,
                    roll->factor,
                    account.contracts()[current.contract].instrument.price_increment);
        bars_.push_back(
            {index_, current.contract, unit_[current.contract], leading, seen, bar.close});
        last_[current.contract] = bars_.back();
        settled_at_ = bar.timestamp_ns;
        observer.marked(index_);
    }
    return bars_;
}

void Replay::want(std::size_t unit, std::optional<Decimal> lots)
{
    wanted_.at(unit) = lots;
}

void Replay::issue(const Bar& bar)
{
    // None decides for a month that is not dominant: a roll closes that.
    if (!bar.leading)
        return;
    if (const auto quantity = target(bar.contract))
        pending_[bar.contract] =
            PaperExecution::Target{"strategy." + std::to_string(bar.index), *quantity, bar.close};
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
