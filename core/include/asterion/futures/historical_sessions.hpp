#pragma once
#include <asterion/futures/history_identity.hpp>
#include <asterion/futures/trading_days.hpp>
#include <algorithm>
#include <chrono>
#include <optional>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace asterion::historical_sessions {

inline constexpr unsigned rules_version = 1;
enum class Part
{
    day,
    evening
};

struct Source
{
    std::string id;
    unsigned version = 1;
    std::string exchange, first_day, last_day, published_day, url;
};

// A closed civil date or an explicitly cancelled evening, not a night-to-trading-day map.
inline std::optional<Source>
reviewed_closure(const std::string& exchange, const std::string& civil_day, Part part)
{
    (void)parse_trading_date(civil_day);
    if (exchange != "SHFE" && exchange != "INE")
        return {};
    std::string first, last;
    if (part == Part::evening) {
        if (civil_day != "2026-09-24" && civil_day != "2026-09-30")
            return {};
        first = last = civil_day;
    } else if (civil_day >= "2026-09-25" && civil_day <= "2026-09-27") {
        first = "2026-09-25";
        last = "2026-09-27";
    } else if (civil_day >= "2026-10-01" && civil_day <= "2026-10-07") {
        first = "2026-10-01";
        last = "2026-10-07";
    } else
        return {};
    return Source{
        exchange + ".2026.mid-autumn-national.closures",
        1,
        exchange,
        first,
        last,
        "2026-09-21",
        exchange == "SHFE"
            ? "https://www.shfe.com.cn/publicnotice/notice/202609/t20260921_833503.html"
            : "https://www.shfe.com.cn/publicnotice/notice/202609/t20260921_833507.html"
    };
}
enum class Anchor
{
    trading_date,
    explicit_evening
};

struct Segment
{
    Anchor anchor = Anchor::trading_date;
    unsigned begin_minute = 0, end_minute = 0;
    bool end_next_date = false;
};

// Reviewed definitions only. A broad current timetable or a chart config is insufficient.
struct Rule
{
    Source source;
    std::string product, timezone = "UTC+08:00", scope_reference;
    std::vector<Segment> segments;
};

struct DayFacts
{
    std::string contract, trading_day, calendar_snapshot, calendar_reference;
    std::optional<bool> open;
    // Explicitly reviewed per-day exceptions and night association, never inferred from bars.
    std::string exception_reference, evening_day, evening_reference;
    std::optional<bool> evening_open;
};

struct Window
{
    std::int64_t begin_ns = 0, end_ns = 0;
};
enum class PlanState
{
    unknown_rule,
    unknown_calendar,
    unknown_exception,
    unknown_evening,
    closed,
    ready
};

struct Plan
{
    Rule rule;
    DayFacts facts;
    PlanState state = PlanState::unknown_rule;
    std::vector<Window> windows;
};

inline constexpr std::int64_t minute_ns = 60000000000LL;

// Explicit fixed UTC+08:00 wall time, never an implicit historical IANA offset lookup.
inline std::int64_t midnight(const std::string& date)
{
    if (date < "1990-01-01" || date > "2100-12-31")
        throw std::invalid_argument("unsupported historical session timestamp date");
    return std::chrono::sys_days(parse_trading_date(date)).time_since_epoch().count() * 1440
        * minute_ns
        - 480 * minute_ns;
}

inline Plan pin_plan(const Rule& rule, const DayFacts& facts)
{
    Plan result{rule, facts, PlanState::unknown_rule, {}};
    const auto identity = HistoryIdentity::parse(facts.contract);
    (void)parse_trading_date(facts.trading_day);
    const auto& source = rule.source;
    if (source.version == 0 || source.id.empty() || source.url.empty()
        || source.published_day.empty() || rule.scope_reference.empty()
        || source.exchange != identity.venue || rule.product != identity.product
        || rule.timezone != "UTC+08:00" || source.first_day.empty() || source.last_day.empty())
        return result;
    (void)parse_trading_date(source.first_day);
    (void)parse_trading_date(source.last_day);
    (void)parse_trading_date(source.published_day);
    if (source.first_day > source.last_day || facts.trading_day < source.first_day
        || facts.trading_day > source.last_day)
        return result;
    if (!facts.open || facts.calendar_snapshot.size() != 64
        || facts.calendar_snapshot.find_first_not_of("0123456789abcdef") != std::string::npos
        || facts.calendar_reference.empty()) {
        result.state = PlanState::unknown_calendar;
        return result;
    }
    if (facts.exception_reference.empty()) {
        result.state = PlanState::unknown_exception;
        return result;
    }
    if (!*facts.open) {
        result.state = PlanState::closed;
        return result;
    }
    if (rule.segments.empty())
        return result;
    for (const auto& segment : rule.segments) {
        if (segment.begin_minute >= 1440 || segment.end_minute >= 1440)
            throw std::invalid_argument("invalid historical session segment");
        auto base = midnight(facts.trading_day);
        if (segment.anchor == Anchor::explicit_evening) {
            if (!facts.evening_open || facts.evening_reference.empty()) {
                result.windows.clear();
                result.state = PlanState::unknown_evening;
                return result;
            }
            if (!*facts.evening_open)
                continue;
            if (facts.evening_day.empty() || facts.evening_day >= facts.trading_day) {
                result.windows.clear();
                result.state = PlanState::unknown_evening;
                return result;
            }
            base = midnight(facts.evening_day);
        }
        const Window window{
            base + segment.begin_minute * minute_ns,
            base + (segment.end_minute + (segment.end_next_date ? 1440 : 0)) * minute_ns
        };
        if (window.end_ns <= window.begin_ns || window.end_ns - window.begin_ns > 1440 * minute_ns
            || window.end_ns > midnight(facts.trading_day) + 1440 * minute_ns)
            throw std::invalid_argument("invalid historical session segment");
        result.windows.push_back(window);
    }
    std::ranges::sort(result.windows, {}, &Window::begin_ns);
    for (std::size_t i = 1; i < result.windows.size(); ++i)
        if (result.windows[i].begin_ns < result.windows[i - 1].end_ns)
            throw std::invalid_argument("historical session segments overlap");
    if (!result.windows.empty())
        result.state = PlanState::ready;
    return result;
}
enum class Emission
{
    unknown,
    every_one_minute_slot
};

struct MinuteDefinition
{
    std::string source, revision, timestamp_reference, emission_reference;
    bool bar_end = false;
    Emission emission = Emission::unknown;
};

struct Observation
{
    std::int64_t end_ns = 0;
    std::string trading_day;
};

struct Request
{
    std::string dataset_id, source;
    std::int64_t begin_ns = 0, end_ns = 0;
    unsigned interval_minutes = 1;
};
enum class AuditState
{
    unknown_plan,
    unknown_labels,
    unsupported_minutes,
    unknown_emission,
    closed,
    conflict,
    partial_request,
    covered_grid,
    missing_grid
};

struct Audit
{
    Plan plan;
    Request request;
    MinuteDefinition definition;
    AuditState state = AuditState::unknown_plan;
    bool covers_first = false, covers_last = false;
    std::vector<std::int64_t> unobserved_slots;
    // Absent without dense emission proof. Never interpreted as zero missing records.
    std::optional<std::vector<std::int64_t>> proven_gaps;
};

inline Audit audit(
    const Plan& plan,
    const Request& request,
    const MinuteDefinition& definition,
    const std::vector<Observation>& observations)
{
    Audit
        result{plan, request, definition, AuditState::unknown_plan, false, false, {}, std::nullopt};
    if (request.dataset_id.size() != 64
        || request.dataset_id.find_first_not_of("0123456789abcdef") != std::string::npos
        || request.begin_ns > request.end_ns)
        throw std::invalid_argument("invalid historical session audit request");
    if (plan.state == PlanState::closed) {
        result.state = observations.empty() ? AuditState::closed : AuditState::conflict;
        return result;
    }
    if (plan.state != PlanState::ready)
        return result;
    if (request.interval_minutes != 1 || !definition.bar_end || definition.source != request.source
        || definition.revision.empty() || definition.timestamp_reference.empty()) {
        result.state = AuditState::unsupported_minutes;
        return result;
    }
    std::set<std::int64_t> grid, observed;
    for (const auto& window : plan.windows)
        for (auto end = window.begin_ns + minute_ns; end <= window.end_ns; end += minute_ns)
            if (end >= request.begin_ns && end <= request.end_ns)
                grid.insert(end);
    for (const auto& row : observations) {
        if (row.trading_day.empty()) {
            result.state = AuditState::unknown_labels;
            return result;
        }
        if (row.trading_day != plan.facts.trading_day || !grid.contains(row.end_ns)
            || !observed.insert(row.end_ns).second) {
            result.state = AuditState::conflict;
            return result;
        }
    }
    result.covers_first = request.begin_ns <= plan.windows.front().begin_ns;
    result.covers_last = request.end_ns >= plan.windows.back().end_ns;
    std::ranges::set_difference(grid, observed, std::back_inserter(result.unobserved_slots));
    if (definition.emission != Emission::every_one_minute_slot
        || definition.emission_reference.empty()) {
        result.state = AuditState::unknown_emission;
        return result;
    }
    result.proven_gaps = result.unobserved_slots;
    if (!result.covers_first || !result.covers_last)
        result.state = AuditState::partial_request;
    else
        result.state =
            result.unobserved_slots.empty() ? AuditState::covered_grid : AuditState::missing_grid;
    return result;
}

} // namespace asterion::historical_sessions
