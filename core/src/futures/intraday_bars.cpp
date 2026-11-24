#include <asterion/futures/intraday_bars.hpp>
#include "futures_sessions_json.hpp"
#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <utility>

namespace asterion {
namespace {

constexpr std::int64_t minute_ms = 60000;
constexpr std::size_t max_bars = 24 * 60;

using SessionRanges = std::vector<std::pair<int, int>>;

int parse_clock_minute(std::string_view text)
{
    return (text[0] - '0') * 600 + (text[1] - '0') * 60 + (text[3] - '0') * 10 + (text[4] - '0');
}

std::string lower_product(std::string_view symbol)
{
    std::string out;
    for (unsigned char ch : symbol) {
        if (!std::isalpha(ch))
            break;
        out.push_back(static_cast<char>(std::tolower(ch)));
    }
    return out;
}

const std::map<std::pair<std::string, std::string>, SessionRanges>& session_table()
{
    static const auto table = [] {
        const auto doc = nlohmann::json::parse(detail::futures_sessions_json);
        std::map<std::string, std::vector<std::pair<int, int>>> days;
        for (const auto& [name, ranges] : doc.at("day_sessions").items())
            for (const auto& range : ranges)
                days[name].emplace_back(
                    parse_clock_minute(range.at(0).get<std::string>()),
                    parse_clock_minute(range.at(1).get<std::string>()));
        std::map<std::string, std::pair<int, int>> nights;
        for (const auto& [name, range] : doc.at("night_sessions").items())
            nights.emplace(
                name,
                std::pair{
                    parse_clock_minute(range.at(0).get<std::string>()),
                    parse_clock_minute(range.at(1).get<std::string>())
                });
        std::map<std::pair<std::string, std::string>, SessionRanges> out;
        for (const auto& [venue, products] : doc.at("products").items()) {
            for (const auto& [product, spec] : products.items()) {
                auto ranges = days.at(spec.at(0).get<std::string>());
                if (!spec.at(1).is_null())
                    ranges.push_back(nights.at(spec.at(1).get<std::string>()));
                out.emplace(std::pair{venue, lower_product(product)}, std::move(ranges));
            }
        }
        return out;
    }();
    return table;
}

std::int64_t
minute_bucket_ms_with(std::string_view venue, const SessionRanges& ranges, std::int64_t source_ms)
{
    constexpr std::int64_t day_ms = 86400000;
    constexpr std::int64_t hour_ms = 3600000;
    const auto start = source_ms - source_ms % minute_ms;
    const auto clock_ms = ((source_ms + 8 * hour_ms) % day_ms + day_ms) % day_ms;
    const auto minute = static_cast<int>(clock_ms / minute_ms);
    if (!ranges.empty()) {
        const auto day_open = ranges.front().first;
        if (minute >= day_open - 5 && minute < day_open)
            return start + (day_open - minute) * minute_ms;
        if (ranges.back().first == 21 * 60 && minute >= 21 * 60 - 5 && minute < 21 * 60)
            return start + (21 * 60 - minute) * minute_ms;
        if (clock_ms % minute_ms < 1000
            && std::ranges::find(ranges, minute, &SessionRanges::value_type::second)
                != ranges.end())
            return start - minute_ms;
        return start;
    }
    const int day_open = venue == "CFFEX" ? 9 * 60 + 30 : 9 * 60;
    if (minute >= day_open - 5 && minute < day_open)
        return start + (day_open - minute) * minute_ms;
    if (venue != "CFFEX" && minute >= 21 * 60 - 5 && minute < 21 * 60)
        return start + (21 * 60 - minute) * minute_ms;
    if (clock_ms % minute_ms < 1000
        && (minute == 11 * 60 + 30 || minute == 15 * 60
            || (venue != "CFFEX" && minute == 10 * 60 + 15)))
        return start - minute_ms;
    return start;
}

// A quote folded forward into the trading day's very first session open carries
// only that opening auction's volume, even after a cold start or overnight gap.
bool is_trading_day_open_auction_with(
    std::string_view venue,
    const SessionRanges& ranges,
    std::int64_t source_ms,
    std::int64_t start)
{
    if (source_ms >= start)
        return false;
    constexpr std::int64_t day_ms = 86400000;
    constexpr std::int64_t hour_ms = 3600000;
    const auto start_minute =
        static_cast<int>((((start + 8 * hour_ms) % day_ms + day_ms) % day_ms) / minute_ms);
    const bool has_night = !ranges.empty() ? ranges.back().first == 21 * 60 : venue != "CFFEX";
    const int first_open = has_night ? 21 * 60
        : !ranges.empty()            ? ranges.front().first
        : venue == "CFFEX"           ? 9 * 60 + 30
                                     : 9 * 60;
    return start_minute == first_open;
}

} // namespace

const std::vector<std::pair<int, int>>& trading_sessions(const InstrumentId& instrument)
{
    const auto& table = session_table();
    const auto found = table.find({instrument.venue, lower_product(instrument.symbol)});
    static const SessionRanges none;
    return found == table.end() ? none : found->second;
}

std::int64_t minute_bucket_ms(const InstrumentId& instrument, std::int64_t source_ms)
{
    return minute_bucket_ms_with(instrument.venue, trading_sessions(instrument), source_ms);
}

void IntradayBars::observe(const MarketQuote& quote)
{
    if (!quote.last || quote.source_ms <= 0 || quote.trading_day.empty())
        return;
    auto& state = states_[quote.instrument];
    auto& series = state.series;
    if (!series.trading_day.empty() && quote.trading_day < series.trading_day)
        return;
    if (!state.sessions)
        state.sessions = &trading_sessions(quote.instrument);
    const auto* sessions = state.sessions;
    const auto start = minute_bucket_ms_with(quote.instrument.venue, *sessions, quote.source_ms);
    if (series.trading_day != quote.trading_day) {
        // A new trading day starts a new series; the old one is not joined to it.
        // The provider's counter restarts each day, so after a day observed to its
        // end—or during the day's first opening auction—the first quote's volume
        // belongs to this minute; mid-day or after a gap it may carry unseen minutes.
        const bool continued = (!series.trading_day.empty() && !state.lost)
            || is_trading_day_open_auction_with(
                quote.instrument.venue,
                *sessions,
                quote.source_ms,
                start);
        state = {};
        state.sessions = sessions;
        series.instrument = quote.instrument;
        series.trading_day = quote.trading_day;
        series.first_observation_ms = quote.source_ms;
        state.cumulative_volume = continued ? 0 : quote.volume;
        state.continued = continued;
    }
    if (!series.bars.empty() && start < series.bars.back().start_ms)
        return;
    if (quote.previous_settlement)
        series.previous_settlement = quote.previous_settlement;
    // A falling cumulative counter is a provider reset, never negative volume.
    const auto delta = quote.volume >= state.cumulative_volume
        ? quote.volume - state.cumulative_volume
        : std::int64_t{0};
    const bool reset = quote.volume < state.cumulative_volume;
    if (reset)
        series.interrupted = true;
    state.cumulative_volume = quote.volume;
    const auto price = *quote.last;
    if (series.bars.empty() || series.bars.back().start_ms != start) {
        // The first bar is whole only when its baseline volume was observed from zero.
        const bool partial = state.lost || (series.bars.empty() && !state.continued);
        series.bars.push_back({start, price, price, price, price, 0, {}, {}, partial});
        if (series.bars.size() > max_bars)
            series.bars.erase(series.bars.begin());
    }
    state.lost = false;
    auto& bar = series.bars.back();
    bar.partial = bar.partial || reset;
    bar.high = std::max(bar.high, price);
    bar.low = std::min(bar.low, price);
    bar.close = price;
    bar.volume += delta;
    if (quote.average_price)
        bar.average_price = quote.average_price;
    if (quote.open_interest)
        bar.open_interest = quote.open_interest;
    auto& recent = state.recent;
    if (!recent.empty() && recent.back().first == quote.source_ms)
        recent.back().second = price;
    else
        recent.emplace_back(quote.source_ms, price);
    // Keep only the newest observation that is already a minute old as anchor.
    while (recent.size() >= 2 && recent[1].first <= quote.source_ms - minute_ms)
        recent.pop_front();
    state.revision = ++revision_;
}

void IntradayBars::interrupt()
{
    for (auto& [id, state] : states_) {
        state.series.interrupted = true;
        state.lost = true;
        auto& bars = state.series.bars;
        const bool marked = !bars.empty() && !std::exchange(bars.back().partial, true);
        if (marked || !state.recent.empty())
            state.revision = ++revision_;
        state.recent.clear();
    }
}

std::vector<InstrumentId> IntradayBars::changed_after(std::uint64_t revision) const
{
    if (revision > revision_)
        throw std::invalid_argument("invalid intraday revision");
    std::vector<InstrumentId> changed;
    for (const auto& [id, state] : states_)
        if (state.revision > revision)
            changed.push_back(id);
    return changed;
}

std::optional<IntradaySeries> IntradayBars::series(const InstrumentId& instrument) const
{
    const auto found = states_.find(instrument);
    if (found == states_.end())
        return std::nullopt;
    return found->second.series;
}

const IntradaySeries* IntradayBars::find(const InstrumentId& instrument) const
{
    const auto found = states_.find(instrument);
    return found == states_.end() ? nullptr : &found->second.series;
}

std::optional<Decimal> IntradayBars::change_1m_percent(const InstrumentId& instrument) const
{
    const auto found = states_.find(instrument);
    if (found == states_.end() || found->second.recent.size() < 2)
        return std::nullopt;
    const auto& recent = found->second.recent;
    const auto& [anchor_ms, anchor] = recent.front();
    const auto& [latest_ms, latest] = recent.back();
    if (anchor_ms > latest_ms - minute_ms || anchor <= Decimal{})
        return std::nullopt;
    return divide(
        multiply(latest - anchor, Decimal::from_integer(100), Rounding::half_even),
        anchor,
        Rounding::half_even);
}

} // namespace asterion
