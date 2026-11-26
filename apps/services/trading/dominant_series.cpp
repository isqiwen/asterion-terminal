#include "dominant_series.hpp"
#include "strategy_validation.hpp"
#include <asterion/protocol/common.hpp>
#include <algorithm>
#include <stdexcept>

namespace asterion::trading {
namespace wire = ::strategy;

std::optional<Decimal>
prev_open_interest(const LiveSeries& series, std::size_t index, const std::string& day)
{
    const auto& map = series.history[index];
    if (const auto today = map.find(day); today != map.end() && today->second.prev_open_interest)
        return today->second.prev_open_interest;
    auto before = map.lower_bound(day);
    if (before != map.begin()) {
        --before;
        return before->second.open_interest;
    }
    return std::nullopt;
}

Decimal series_roll_ratio(const LiveSeries& series, std::size_t next, const std::string& day)
{
    const auto settlement = [&](std::size_t index) {
        const auto it = series.history[index].find(day);
        if (it == series.history[index].end() || !it->second.prev_settlement)
            throw std::runtime_error("dominant roll lacks previous settlement price on " + day);
        return *it->second.prev_settlement;
    };
    return divide(settlement(next), settlement(series.active_index), Rounding::half_up);
}

std::optional<std::size_t> choose_dominant(
    const LiveSeries& series,
    const std::string& day,
    bool had_history,
    const wire::Bars* raw)
{
    std::optional<std::size_t> chosen;
    Decimal best_oi;
    for (std::size_t m = series.active_index; m < series.candidates.size(); ++m) {
        if (candidate_expiring(series.expiry_days, m, day))
            continue;
        const bool active_today = series.history[m].contains(day)
            || (raw && std::ranges::any_of(raw->bars(), [&](const auto& bar) {
                   return bar.instrument().exchange() == series.candidates[m].venue
                       && bar.instrument().symbol() == series.candidates[m].symbol;
               }));
        if (!active_today)
            continue;
        auto oi = prev_open_interest(series, m, day);
        if (!oi && !had_history && series.history[m].contains(day))
            oi = series.history[m].at(day).open_interest;
        if (!oi)
            continue;
        if (!chosen || *oi > best_oi) {
            chosen = m;
            best_oi = *oi;
        }
    }
    return chosen;
}

std::optional<std::size_t>
choose_sub_dominant(const LiveSeries& series, const std::string& day, const wire::Bars* raw)
{
    auto is_active = [&](std::size_t m) {
        return series.history[m].contains(day)
            || (raw && std::ranges::any_of(raw->bars(), [&](const auto& bar) {
                   return bar.instrument().exchange() == series.candidates[m].venue
                       && bar.instrument().symbol() == series.candidates[m].symbol;
               }));
    };
    bool any_prev = false;
    for (std::size_t m = series.active_index + 1; m < series.candidates.size(); ++m)
        if (is_active(m) && prev_open_interest(series, m, day)) {
            any_prev = true;
            break;
        }
    std::optional<std::size_t> chosen;
    Decimal best_oi;
    for (std::size_t m = series.active_index + 1; m < series.candidates.size(); ++m) {
        if (!is_active(m))
            continue;
        auto oi = prev_open_interest(series, m, day);
        if (!oi && !any_prev && series.history[m].contains(day))
            oi = series.history[m].at(day).open_interest;
        if (!oi)
            continue;
        if (!chosen || *oi > best_oi) {
            chosen = m;
            best_oi = *oi;
        }
    }
    return chosen;
}

std::set<std::string> awaited_series_contracts(
    const wire::Instance& instance,
    const std::vector<LiveSeries>& live_series,
    const std::string& day)
{
    std::set<std::string> out;
    for (const auto& id : instance.instruments())
        out.insert(id.exchange() + "/" + id.symbol());
    for (const auto& series : live_series) {
        const auto& active = series.candidates[series.active_index];
        out.insert(active.venue + "/" + active.symbol);
        if (!day.empty()) {
            if (const auto sub = choose_sub_dominant(series, day))
                out.insert(series.candidates[*sub].venue + "/" + series.candidates[*sub].symbol);
        }
        if (!day.empty() && day > series.decided_day) {
            const bool expiring = candidate_expiring(series.expiry_days, series.active_index, day);
            const auto active_oi = prev_open_interest(series, series.active_index, day);
            for (std::size_t m = series.active_index + 1; m < series.candidates.size(); ++m) {
                const auto oi = prev_open_interest(series, m, day);
                if (expiring || (oi && (!active_oi || *oi > *active_oi)))
                    out.insert(series.candidates[m].venue + "/" + series.candidates[m].symbol);
            }
        }
    }
    return out;
}

void observe_series_quote(
    const wire::Instance& instance,
    std::vector<LiveSeries>& live_series,
    StrategyBars* bars,
    std::string& last_awaited_day,
    const MarketQuote& observed)
{
    if (observed.trading_day.size() != 8)
        return;
    const auto day = observed.trading_day.substr(0, 4) + "-" + observed.trading_day.substr(4, 2)
        + "-" + observed.trading_day.substr(6, 2);
    bool matched = false;
    bool awaited_changed = day != last_awaited_day;
    for (auto& series : live_series) {
        const auto found = std::ranges::find(series.candidates, observed.instrument);
        if (found == series.candidates.end())
            continue;
        matched = true;
        auto [it, inserted] =
            series.history[static_cast<std::size_t>(found - series.candidates.begin())].try_emplace(
                day);
        auto& stats = it->second;
        if (inserted)
            awaited_changed = true;
        if (observed.previous_settlement && *observed.previous_settlement > Decimal{})
            stats.prev_settlement = observed.previous_settlement;
        if (observed.open_interest && observed.open_interest_change) {
            const auto prev_oi = *observed.open_interest - *observed.open_interest_change;
            if (stats.prev_open_interest != prev_oi) {
                stats.prev_open_interest = prev_oi;
                awaited_changed = true;
            }
        }
        if (observed.open_interest) {
            if (!stats.prev_open_interest && stats.open_interest != observed.open_interest)
                awaited_changed = true;
            stats.open_interest = observed.open_interest;
        }
    }
    if (matched && bars && awaited_changed) {
        last_awaited_day = day;
        bars->set_awaited(awaited_series_contracts(instance, live_series, day));
    }
}

void record_series_step_bars(std::vector<LiveSeries>& live_series, const wire::Bars& input)
{
    for (const auto& bar : input.bars()) {
        const InstrumentId id{bar.instrument().exchange(), bar.instrument().symbol()};
        for (auto& series : live_series) {
            const auto found = std::ranges::find(series.candidates, id);
            if (found != series.candidates.end()) {
                series.active_index = static_cast<std::size_t>(found - series.candidates.begin());
                series.decided_day = bar.trading_day();
                auto& stats = series.history[series.active_index][bar.trading_day()];
                if (bar.has_open_interest() && !bar.open_interest().text().empty())
                    stats.open_interest = Decimal::parse(bar.open_interest().text());
                if (bar.has_close() && !bar.close().text().empty())
                    series.prev_bar_close[series.active_index] = Decimal::parse(bar.close().text());
            }
        }
    }
}

wire::Bars filter_and_roll_bars(
    const wire::Instance& instance,
    std::vector<LiveSeries>& live_series,
    LiveSession* account,
    const StrategyAuthorityPtr& authority,
    StrategyBars* bars,
    const wire::Bars& raw,
    const std::function<void(const std::string&)>& on_roll_event)
{
    for (std::size_t s = 0; s < live_series.size(); ++s) {
        auto& series = live_series[s];
        std::string day;
        for (const auto& bar : raw.bars()) {
            const InstrumentId id{bar.instrument().exchange(), bar.instrument().symbol()};
            const auto found = std::ranges::find(series.candidates, id);
            if (found != series.candidates.end()
                && static_cast<std::size_t>(found - series.candidates.begin())
                    >= series.active_index) {
                day = bar.trading_day();
                break;
            }
        }
        if (day.empty() || day <= series.decided_day)
            continue;
        const bool had_history = !series.decided_day.empty();
        const auto chosen = choose_dominant(series, day, had_history, &raw);
        if (chosen && *chosen > series.active_index) {
            std::optional<Decimal> ratio;
            if (had_history)
                ratio = series_roll_ratio(series, *chosen, day);
            account->roll_strategy_series(authority, s, series.candidates[*chosen]).get();
            const auto old_symbol = series.candidates[series.active_index].symbol;
            series.active_index = *chosen;
            series.pending_roll_ratio = ratio;
            bars->set_awaited(awaited_series_contracts(instance, live_series, day));
            if (ratio)
                on_roll_event(
                    "rolled " + series.symbol + " from " + old_symbol + " to "
                    + series.candidates[*chosen].symbol + " ratio=" + ratio->str());
        }
        series.decided_day = day;
    }
    wire::Bars out;
    for (const auto& fixed : instance.instruments()) {
        for (const auto& bar : raw.bars())
            if (bar.instrument().exchange() == fixed.exchange()
                && bar.instrument().symbol() == fixed.symbol())
                *out.add_bars() = bar;
    }
    for (auto& series : live_series) {
        const auto& active = series.candidates[series.active_index];
        for (const auto& bar : raw.bars()) {
            if (bar.instrument().exchange() == active.venue
                && bar.instrument().symbol() == active.symbol) {
                auto* added = out.add_bars();
                *added = bar;
                if (series.pending_roll_ratio) {
                    *added->mutable_roll_ratio() = protocol::decimal(*series.pending_roll_ratio);
                    series.pending_roll_ratio.reset();
                }
                if (bar.has_close() && !bar.close().text().empty()
                    && series.prev_bar_close[series.active_index]) {
                    if (const auto nr = simple_return(
                            Decimal::parse(bar.close().text()),
                            *series.prev_bar_close[series.active_index]))
                        *added->mutable_near_return() = protocol::decimal(*nr);
                }
                if (const auto sub = choose_sub_dominant(series, bar.trading_day(), &raw)) {
                    const auto& far = series.candidates[*sub];
                    for (const auto& far_bar : raw.bars()) {
                        if (far_bar.instrument().exchange() == far.venue
                            && far_bar.instrument().symbol() == far.symbol && far_bar.has_close()
                            && !far_bar.close().text().empty() && bar.has_close()
                            && !bar.close().text().empty()) {
                            const auto far_px = Decimal::parse(far_bar.close().text());
                            *added->mutable_far_close() = far_bar.close();
                            if (const auto ry = annualized_roll_yield(
                                    Decimal::parse(bar.close().text()),
                                    far_px,
                                    series.delivery_months[series.active_index],
                                    series.delivery_months[*sub])) {
                                *added->mutable_roll_yield() = protocol::decimal(*ry);
                            }
                            if (series.prev_bar_close[*sub]) {
                                if (const auto fr =
                                        simple_return(far_px, *series.prev_bar_close[*sub]))
                                    *added->mutable_far_return() = protocol::decimal(*fr);
                            }
                            break;
                        }
                    }
                }
            }
        }
        for (const auto& bar : raw.bars()) {
            if (!bar.has_close() || bar.close().text().empty())
                continue;
            const InstrumentId id{bar.instrument().exchange(), bar.instrument().symbol()};
            const auto found = std::ranges::find(series.candidates, id);
            if (found != series.candidates.end()) {
                series.prev_bar_close[static_cast<std::size_t>(found - series.candidates.begin())] =
                    Decimal::parse(bar.close().text());
            }
        }
    }
    return out;
}

void roll_decision_for_live_day(
    const std::vector<LiveSeries>& live_series,
    LiveSession* account,
    const StrategyAuthorityPtr& authority,
    wire::Decision& exec_decision)
{
    for (std::size_t s = 0; s < live_series.size(); ++s) {
        const auto& series = live_series[s];
        std::string live_day;
        for (std::size_t m = series.active_index; m < series.history.size(); ++m)
            if (!series.history[m].empty() && series.history[m].rbegin()->first > live_day)
                live_day = series.history[m].rbegin()->first;
        if (live_day.empty() || live_day <= series.decided_day)
            continue;
        const auto chosen = choose_dominant(series, live_day, true);
        if (!chosen || *chosen <= series.active_index)
            continue;
        const auto& from = series.candidates[series.active_index];
        const auto& to = series.candidates[*chosen];
        (void)series_roll_ratio(series, *chosen, live_day);
        account->roll_strategy_series(authority, s, to).get();
        for (auto& target : *exec_decision.mutable_targets())
            if (target.instrument().exchange() == from.venue
                && target.instrument().symbol() == from.symbol) {
                target.mutable_instrument()->set_exchange(to.venue);
                target.mutable_instrument()->set_symbol(to.symbol);
            }
    }
}

} // namespace asterion::trading
