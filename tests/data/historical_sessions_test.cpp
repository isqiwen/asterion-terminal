#include "historical_sessions.hpp"
#include "session_definition.hpp"
#include <gtest/gtest.h>
using namespace asterion;
namespace hs = historical_sessions;

namespace {

// Synthetic small sessions exercise mechanisms; these times are not reviewed market rules.
hs::Rule fixture_rule()
{
    return {
        {"fixture.sessions",
            1,
            "SHFE",
            "2026-09-07",
            "2026-09-07",
            "2026-09-01",
            "fixture://sessions"},
        "cu",
        "UTC+08:00",
        "fixture effective-date proof",
        {{hs::Anchor::explicit_evening, 23 * 60 + 58, 2, true},
            {hs::Anchor::trading_date, 9 * 60, 9 * 60 + 3, false}}
    };
}

hs::DayFacts fixture_day()
{
    return {
        "SHFE/cu/2026-10",
        "2026-09-07",
        std::string(64, 'a'),
        "fixture fixed calendar row",
        true,
        "fixture per-day exception review",
        "2026-09-04",
        "fixture explicit Friday-evening to Monday association",
        true
    };
}

hs::MinuteDefinition fixture_definition()
{
    return {
        "fixture.minutes",
        "fixture.minutes.1",
        "fixture bar-end and session-start alignment",
        "fixture emits each one-minute slot including no trades",
        true,
        hs::Emission::every_one_minute_slot
    };
}

hs::Request fixture_request()
{
    return {
        std::string(64, 'b'),
        "fixture.minutes",
        hs::midnight("2026-09-04") + (23 * 60 + 58) * hs::minute_ns,
        hs::midnight("2026-09-07") + (9 * 60 + 3) * hs::minute_ns,
        1
    };
}

std::vector<hs::Observation> fixture_rows(const hs::Plan& plan)
{
    std::vector<hs::Observation> rows;
    for (const auto& window : plan.windows)
        for (auto end = window.begin_ns + hs::minute_ns; end <= window.end_ns; end += hs::minute_ns)
            rows.push_back({end, plan.facts.trading_day});
    return rows;
}

} // namespace

TEST(HistoricalSessions, OfficialClosuresAreStrictlyBoundedByExchangeDateAndSessionPart)
{
    for (const auto* exchange : {"SHFE", "INE"}) {
        const auto closed = hs::reviewed_closure(exchange, "2026-09-26", hs::Part::day);
        ASSERT_TRUE(closed);
        EXPECT_EQ(closed->version, 1);
        EXPECT_EQ(closed->published_day, "2026-09-21");
        EXPECT_EQ(closed->first_day, "2026-09-25");
        EXPECT_EQ(closed->last_day, "2026-09-27");
        EXPECT_NE(closed->url.find("shfe.com.cn"), std::string::npos);
        EXPECT_TRUE(hs::reviewed_closure(exchange, "2026-09-24", hs::Part::evening));
        EXPECT_FALSE(hs::reviewed_closure(exchange, "2026-09-24", hs::Part::day));
        EXPECT_FALSE(hs::reviewed_closure(exchange, "2026-09-26", hs::Part::evening));
        EXPECT_TRUE(hs::reviewed_closure(exchange, "2026-09-30", hs::Part::evening));
        EXPECT_FALSE(hs::reviewed_closure(exchange, "2026-09-30", hs::Part::day));
        EXPECT_TRUE(hs::reviewed_closure(exchange, "2026-10-01", hs::Part::day));
        EXPECT_TRUE(hs::reviewed_closure(exchange, "2026-10-07", hs::Part::day));
        EXPECT_FALSE(hs::reviewed_closure(exchange, "2026-10-08", hs::Part::evening))
            << "resumption does not certify a complete schedule";
        EXPECT_FALSE(hs::reviewed_closure(exchange, "2025-10-01", hs::Part::day));
    }
    EXPECT_FALSE(hs::reviewed_closure("DCE", "2026-09-26", hs::Part::day));
}

TEST(HistoricalSessions, ExplicitFridayNightCrossesMidnightAndBelongsToMondayWithoutWeekendSlots)
{
    const auto plan = hs::pin_plan(fixture_rule(), fixture_day());
    ASSERT_EQ(plan.state, hs::PlanState::ready);
    ASSERT_EQ(plan.windows.size(), 2);
    EXPECT_EQ(
        plan.windows[0].begin_ns,
        hs::midnight("2026-09-04") + (23 * 60 + 58) * hs::minute_ns);
    EXPECT_EQ(plan.windows[0].end_ns, hs::midnight("2026-09-05") + 2 * hs::minute_ns);
    const auto rows = fixture_rows(plan);
    ASSERT_EQ(rows.size(), 7);
    EXPECT_EQ(rows[1].end_ns, hs::midnight("2026-09-05"));
    EXPECT_EQ(rows[1].trading_day, "2026-09-07");
    const auto result = hs::audit(plan, fixture_request(), fixture_definition(), rows);
    EXPECT_EQ(result.state, hs::AuditState::covered_grid);
    ASSERT_TRUE(result.proven_gaps);
    EXPECT_TRUE(result.proven_gaps->empty());
    EXPECT_TRUE(result.covers_first);
    EXPECT_TRUE(result.covers_last);
}

TEST(HistoricalSessions, CalendarExceptionAndNightAssociationRequireIndependentEvidence)
{
    auto facts = fixture_day();
    facts.open.reset();
    EXPECT_EQ(hs::pin_plan(fixture_rule(), facts).state, hs::PlanState::unknown_calendar);
    facts = fixture_day();
    facts.exception_reference.clear();
    EXPECT_EQ(hs::pin_plan(fixture_rule(), facts).state, hs::PlanState::unknown_exception);
    facts = fixture_day();
    facts.evening_reference.clear();
    EXPECT_EQ(hs::pin_plan(fixture_rule(), facts).state, hs::PlanState::unknown_evening);
    facts = fixture_day();
    facts.evening_day.clear();
    EXPECT_EQ(hs::pin_plan(fixture_rule(), facts).state, hs::PlanState::unknown_evening);
    facts = fixture_day();
    facts.evening_open = false;
    const auto plan = hs::pin_plan(fixture_rule(), facts);
    ASSERT_EQ(plan.windows.size(), 1);
    EXPECT_EQ(fixture_rows(plan).size(), 3);
    facts.open = false;
    const auto closed = hs::pin_plan(fixture_rule(), facts);
    EXPECT_EQ(closed.state, hs::PlanState::closed);
    EXPECT_EQ(
        hs::audit(closed, fixture_request(), fixture_definition(), {}).state,
        hs::AuditState::closed);
    EXPECT_EQ(
        hs::audit(
            closed,
            fixture_request(),
            fixture_definition(),
            {{fixture_request().end_ns, facts.trading_day}})
            .state,
        hs::AuditState::conflict);
}

TEST(HistoricalSessions, RuleRevisionAndEffectiveDateBoundariesDoNotAlterPinnedPlans)
{
    auto unsupported = fixture_rule();
    unsupported.timezone = "Asia/Shanghai";
    EXPECT_EQ(hs::pin_plan(unsupported, fixture_day()).state, hs::PlanState::unknown_rule);
    auto rule = fixture_rule();
    auto facts = fixture_day();
    const auto old = hs::pin_plan(rule, facts);
    rule.source.version = 2;
    rule.source.first_day = rule.source.last_day = "2026-09-08";
    rule.segments[0].end_minute = 1;
    EXPECT_EQ(hs::pin_plan(rule, facts).state, hs::PlanState::unknown_rule);
    facts.trading_day = "2026-09-08";
    facts.evening_day = "2026-09-07";
    const auto next = hs::pin_plan(rule, facts);
    ASSERT_EQ(next.state, hs::PlanState::ready);
    EXPECT_EQ(next.rule.source.version, 2);
    EXPECT_EQ(next.windows[0].end_ns, hs::midnight("2026-09-08") + hs::minute_ns);
    EXPECT_EQ(old.rule.source.version, 1);
    EXPECT_EQ(old.rule.source.last_day, "2026-09-07");
    EXPECT_EQ(old.windows[0].end_ns, hs::midnight("2026-09-05") + 2 * hs::minute_ns);
    facts.trading_day = "2026-09-09";
    EXPECT_EQ(hs::pin_plan(rule, facts).state, hs::PlanState::unknown_rule);
}

TEST(HistoricalSessions, MissingSlotsAreNotProvenGapsWithoutDenseVendorEmissionEvidence)
{
    const auto plan = hs::pin_plan(fixture_rule(), fixture_day());
    auto rows = fixture_rows(plan);
    const auto missing = rows[2].end_ns;
    rows.erase(rows.begin() + 2);
    const auto result = hs::audit(plan, fixture_request(), fixture_definition(), rows);
    EXPECT_EQ(result.state, hs::AuditState::missing_grid);
    ASSERT_TRUE(result.proven_gaps);
    ASSERT_EQ(result.proven_gaps->size(), 1);
    EXPECT_EQ(result.proven_gaps->front(), missing);
    auto unknown = fixture_definition();
    unknown.emission = hs::Emission::unknown;
    const auto unverified = hs::audit(plan, fixture_request(), unknown, rows);
    EXPECT_EQ(unverified.state, hs::AuditState::unknown_emission);
    EXPECT_EQ(unverified.unobserved_slots.size(), 1);
    EXPECT_FALSE(unverified.proven_gaps);
    auto tushare = tushare::reviewed_minute_definition();
    auto request = fixture_request();
    request.source = tushare.source;
    EXPECT_EQ(hs::audit(plan, request, tushare, rows).state, hs::AuditState::unknown_emission);
    rows[0].trading_day.clear();
    EXPECT_EQ(
        hs::audit(plan, fixture_request(), fixture_definition(), rows).state,
        hs::AuditState::unknown_labels);
}

TEST(HistoricalSessions, PartialFirstAndLastSessionsCannotBeDeclaredComplete)
{
    const auto plan = hs::pin_plan(fixture_rule(), fixture_day());
    auto request = fixture_request();
    request.begin_ns += 90 * 1000000000LL;
    request.end_ns -= 30 * 1000000000LL;
    auto rows = fixture_rows(plan);
    std::erase_if(rows, [&](const auto& row) {
        return row.end_ns < request.begin_ns || row.end_ns > request.end_ns;
    });
    const auto result = hs::audit(plan, request, fixture_definition(), rows);
    EXPECT_EQ(result.state, hs::AuditState::partial_request);
    EXPECT_FALSE(result.covers_first);
    EXPECT_FALSE(result.covers_last);
    ASSERT_TRUE(result.proven_gaps);
    EXPECT_TRUE(result.proven_gaps->empty());
    request = fixture_request();
    request.interval_minutes = 5;
    EXPECT_EQ(
        hs::audit(plan, request, fixture_definition(), rows).state,
        hs::AuditState::unsupported_minutes);
}

TEST(HistoricalSessions, InvalidSegmentsAndOffGridOrDuplicateObservationsAreRejected)
{
    auto rule = fixture_rule();
    rule.segments.push_back(rule.segments.back());
    EXPECT_THROW(hs::pin_plan(rule, fixture_day()), std::invalid_argument);
    rule = fixture_rule();
    rule.segments[0].begin_minute = 1440;
    EXPECT_THROW(hs::pin_plan(rule, fixture_day()), std::invalid_argument);
    const auto plan = hs::pin_plan(fixture_rule(), fixture_day());
    auto rows = fixture_rows(plan);
    rows.push_back(rows.front());
    EXPECT_EQ(
        hs::audit(plan, fixture_request(), fixture_definition(), rows).state,
        hs::AuditState::conflict);
    rows = fixture_rows(plan);
    rows[0].end_ns += 1000000000;
    EXPECT_EQ(
        hs::audit(plan, fixture_request(), fixture_definition(), rows).state,
        hs::AuditState::conflict);
    EXPECT_THROW(hs::midnight("9999-01-01"), std::invalid_argument);
}
