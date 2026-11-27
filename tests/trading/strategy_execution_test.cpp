#include "trading/live_session_fixture.hpp"

using namespace asterion;
using namespace asterion::test::live;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

class StrategyExecution : public Live
{
protected:
    static asterion::trading::StrategyOrderBasis
    basis(asterion::trading::StrategyAuthorityPtr authority, std::uint64_t sequence = 1)
    {
        if (test_clock)
            test_clock->advance(1100ms);
        const auto now = test_clock ? steady_now(*test_clock) : std::chrono::steady_clock::now();
        return {std::move(authority), sequence, 0, true, now + 10s};
    }
};

} // namespace

TEST_F(StrategyExecution, RegistrationIsExclusiveAndNeverGrantsAuthorizationOrReadiness)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    EXPECT_THROW((void)session.register_strategy().get(), Error);
    ASSERT_EQ(ready(session).at("phase"), "ready");
    EXPECT_THROW(
        (void)session.register_strategy("bad", {{"SHFE", "cu2610"}}).get(),
        std::invalid_argument);
    auto owner = session.register_strategy().get();
    EXPECT_THROW((void)session.register_strategy("second").get(), Error);
    EXPECT_THROW(act(session, submit("manual", "1")), Error);
    auto warming = basis(owner);
    warming.ready = false;
    EXPECT_THROW(session.execute_strategy(warming, submit("warming", "1")).get(), Error);
    EXPECT_TRUE(session.snapshot().at("orders").empty());
    session.release_strategy(owner).get();
    owner = session.register_strategy("not-authorized").get();
    EXPECT_THROW(
        session.execute_strategy(basis(owner), submit("unauthorized", "1")).get(),
        std::invalid_argument);
    act(session, authorize());
    // A failed execution does not revive when authorization is later granted.
    EXPECT_THROW(
        session.execute_strategy(basis(owner, 2), submit("failed.instance", "1")).get(),
        Error);
    EXPECT_TRUE(session.snapshot().at("orders").empty());
    session.release_strategy(owner).get();
    EXPECT_NO_THROW(act(session, submit("manual.after.release", "1")));
}

TEST_F(StrategyExecution, UsesTheExistingRiskAndPriceChecksAndBlocksFurtherLegsAfterFailure)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    for (const auto& order : {submit("too.many", "6"), submit("bad.price", "1", "3701")}) {
        auto owner = session.register_strategy().get();
        EXPECT_THROW(session.execute_strategy(basis(owner), order).get(), std::invalid_argument);
        EXPECT_THROW(
            session.execute_strategy(basis(owner, 2), submit("cannot.continue", "1")).get(),
            Error);
        session.release_strategy(owner).get();
    }
    EXPECT_TRUE(session.snapshot().at("orders").empty());
    EXPECT_TRUE(session.snapshot().at("unconfirmed").empty());
}

TEST_F(StrategyExecution, PlannedOrderIsDurableAndAttributedWithoutRestoringAuthority)
{
    asterion::trading::StrategyAuthorityPtr old_owner;
    {
        LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
        ASSERT_EQ(ready(session).at("phase"), "ready");
        act(session, authorize());
        old_owner = session.register_strategy().get();
        // The production planner supplies the C++ order leg; Python has no
        // entry to this interface or the account's limit price selection.
        BrokerSnapshot facts;
        facts.phase = BrokerPhase::ready;
        facts.positions_reconciled = true;
        const auto policy = asterion::trading::PolicyDefinition::from_wire(
            protocol::encode_policy(manifest().at("policy")));
        const asterion::trading::PositionTarget target{{"SHFE", "rb2610"}, Decimal::parse("1")};
        const auto plan =
            asterion::trading::plan_strategy_orders({&target, 1}, policy.contracts, facts, false);
        ASSERT_EQ(plan.orders.size(), 1U);
        const auto& leg = plan.orders[0];
        auto order = submit("strategy.fill", leg.quantity.str().c_str());
        EXPECT_NO_THROW(session.execute_strategy(basis(old_owner), order).get());
        ASSERT_TRUE(order_has_status(
            wait_for(
                session,
                [](const Json& state) {
                    return order_has_status(state, "strategy.fill", "filled")
                        && state.at("positions_reconciled") == true;
                }),
            "strategy.fill",
            "filled"));
        EXPECT_THROW(
            session.execute_strategy(basis(old_owner), submit("duplicate.target", "1")).get(),
            Error);
    }
    const auto record = test::read_record(test::journal_record(directory.path, 2));
    const auto decoded =
        std::get<asterion::trading::CommandRecord>(asterion::trading::parse_record(record));
    ASSERT_TRUE(decoded.request.as<asterion::trading::SubmitOrder>()->strategy);
    const auto& source = *decoded.request.as<asterion::trading::SubmitOrder>()->strategy;
    EXPECT_EQ(source.instance, "strategy-run");
    EXPECT_EQ(source.target_sequence, 1U);
    EXPECT_EQ(source.leg, 0U);
    LiveSession recovered(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
    ASSERT_EQ(ready(recovered).at("phase"), "ready");
    EXPECT_FALSE(recovered.snapshot().contains("authorization"));
    act(recovered, authorize("after.restart"));
    auto fresh = recovered.register_strategy().get();
    EXPECT_THROW(
        recovered.execute_strategy(basis(old_owner, 2), submit("old.authority", "1")).get(),
        Error);
    EXPECT_NE(fresh, old_owner);
    recovered.release_strategy(fresh).get();
}

TEST_F(StrategyExecution, RefusesStaleSequencesSkippedLegsAndChangedTargetDeadlines)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest(4));
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    auto owner = session.register_strategy().get();
    auto admitted = basis(owner, 7);
    EXPECT_NO_THROW(session.execute_strategy(admitted, submit("first.leg", "3")).get());
    ASSERT_TRUE(order_has_status(
        wait_for(
            session,
            [](const Json& state) {
                return order_has_status(state, "first.leg", "accepted");
            }),
        "first.leg",
        "accepted"));
    EXPECT_THROW(session.execute_strategy(admitted, submit("duplicate.leg", "1")).get(), Error);
    EXPECT_THROW(
        session.execute_strategy(basis(owner, 6), submit("older.target", "1")).get(),
        Error);
    auto skipped = admitted;
    skipped.leg = 2;
    EXPECT_THROW(session.execute_strategy(skipped, submit("skipped.leg", "1")).get(), Error);
    auto refreshed = basis(owner, 7);
    refreshed.leg = 1;
    EXPECT_THROW(
        session.execute_strategy(refreshed, submit("refreshed.deadline", "1")).get(),
        Error);
    auto excessive = basis(owner, 8);
    excessive.expires_at += 1s;
    EXPECT_THROW(session.execute_strategy(excessive, submit("excessive.ttl", "1")), Error);
    EXPECT_EQ(session.snapshot().at("orders").size(), 1U);
    auto expired = basis(owner, 8);
    expired.expires_at = steady_now(*clock) - 1s;
    EXPECT_THROW(session.execute_strategy(expired, submit("already.expired", "1")).get(), Error);
    EXPECT_THROW(
        session.execute_strategy(basis(owner, 8), submit("cannot.refresh", "1")).get(),
        Error);
    // Refused sequencing sends nothing more; expiry alone does not end the instance.
    EXPECT_EQ(session.snapshot().at("orders").size(), 1U);
    EXPECT_TRUE(session.strategy_failure(owner).empty());
}

TEST_F(StrategyExecution, ReleasedAuthorityCannotSubmitEvenWhenTheInstanceNameIsReused)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    const auto old = session.register_strategy().get();
    session.release_strategy(old).get();
    const auto fresh = session.register_strategy().get();
    EXPECT_THROW(session.execute_strategy(basis(old), submit("old", "1")).get(), Error);
    EXPECT_THROW(session.release_strategy(old).get(), Error);
    EXPECT_NO_THROW(session.execute_strategy(basis(fresh), submit("fresh", "1")).get());
}

TEST_F(StrategyExecution, ReleaseDuringDurableSubmissionFencesQueuedAndRecordedOrders)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    auto owner = session.register_strategy().get();
    HeldCommit held;
    auto sending = session.execute_strategy(basis(owner), submit("stopping", "1"));
    ASSERT_TRUE(held.waiting());
    auto queued = session.execute_strategy(basis(owner, 2), submit("queued", "1"));
    auto stopped = session.release_strategy(owner);
    EXPECT_EQ(stopped.wait_for(1s), std::future_status::ready);
    stopped.get();
    held.release();
    EXPECT_THROW(sending.get(), Error);
    EXPECT_THROW(queued.get(), Error);
    const auto after = session.snapshot();
    EXPECT_TRUE(after.at("orders").empty());
    EXPECT_TRUE(after.at("unconfirmed").empty());
    EXPECT_FALSE(session.recovery_required());
    EXPECT_NO_THROW(act(session, submit("manual.after.stop", "1")));
}

TEST_F(StrategyExecution, RevocationDuringDurableSubmissionDoesNotPermitLateDispatch)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    auto owner = session.register_strategy().get();
    HeldCommit held;
    auto sending = session.execute_strategy(basis(owner), submit("revoked", "1"));
    ASSERT_TRUE(held.waiting());
    const auto current = session.snapshot();
    auto revoke = session.execute(
        current.at("account_id").get<std::string>(),
        current.at("policy_revision").get<std::string>(),
        {{"request_id", "revoke.strategy"}, {"action", "live_revoke"}});
    held.release();
    EXPECT_THROW(sending.get(), Error);
    EXPECT_NO_THROW(revoke.get());
    EXPECT_TRUE(session.snapshot().at("orders").empty());
    EXPECT_TRUE(session.snapshot().at("unconfirmed").empty());
    EXPECT_FALSE(session.snapshot().contains("authorization"));
}

TEST_F(StrategyExecution, TargetExpiryDuringDurableSubmissionDoesNotBorrowTheQuoteDeadline)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    auto owner = session.register_strategy().get();
    auto target = basis(owner);
    target.expires_at = steady_now(*clock) + 3s;
    HeldCommit held;
    auto sending = session.execute_strategy(target, submit("expired.target", "1"));
    ASSERT_TRUE(held.waiting());
    clock->advance(3020ms);
    held.release();
    EXPECT_THROW(sending.get(), Error);
    EXPECT_TRUE(session.snapshot().at("orders").empty());
    EXPECT_TRUE(session.snapshot().at("unconfirmed").empty());
}

TEST_F(StrategyExecution, TargetExpiryDuringQuoteQueryProducesNoOrderIntent)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    auto owner = session.register_strategy().get();

    struct Release
    {
        ~Release()
        {
            exchange.call<void (*)(int)>("asterion_fake_trader_hold_quote_return", 0);
        }

        FakeExchange& exchange;
    } release{exchange};

    exchange.call<void (*)(int)>("asterion_fake_trader_hold_quote_return", 1);
    auto target = basis(owner);
    target.expires_at = steady_now(*clock) + 2s;
    const auto before = session.snapshot().at("capacity").at("records_used");
    auto sending = session.execute_strategy(target, submit("quote.expired", "1"));
    const auto waiting = std::chrono::steady_clock::now() + 3s;
    while (!exchange.call<int (*)()>("asterion_fake_trader_quote_return_waiting")
        && std::chrono::steady_clock::now() < waiting)
        std::this_thread::sleep_for(5ms);
    ASSERT_TRUE(exchange.call<int (*)()>("asterion_fake_trader_quote_return_waiting"));
    clock->advance(2020ms);
    exchange.call<void (*)(int)>("asterion_fake_trader_hold_quote_return", 0);
    EXPECT_THROW(sending.get(), Error);
    EXPECT_TRUE(session.snapshot().at("orders").empty());
    EXPECT_EQ(session.snapshot().at("capacity").at("records_used"), before);
}

TEST_F(StrategyExecution, ClaimingTheAccountFencesAnAlreadyRecordedManualOrder)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    const auto current = session.snapshot();
    HeldCommit held;
    auto manual = session.execute(
        current.at("account_id").get<std::string>(),
        current.at("policy_revision").get<std::string>(),
        submit("manual.in.flight", "1"));
    ASSERT_TRUE(held.waiting());
    auto owner = session.register_strategy().get();
    EXPECT_THROW(act(session, submit("manual.during.strategy", "1")), Error);
    held.release();
    EXPECT_THROW(manual.get(), Error);
    EXPECT_TRUE(session.snapshot().at("orders").empty());
    EXPECT_TRUE(session.snapshot().at("unconfirmed").empty());
    session.release_strategy(owner).get();
}

TEST_F(StrategyExecution, ReleaseAfterDispatchPreservesTheBrokersOrderAndLateSdkResult)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    auto owner = session.register_strategy().get();

    struct Release
    {
        ~Release()
        {
            exchange.call<void (*)(int, int)>("asterion_fake_trader_hold_insert_return", 0, 0);
        }

        FakeExchange& exchange;
    } release{exchange};

    exchange.call<void (*)(int, int)>("asterion_fake_trader_hold_insert_return", 1, -7);
    auto sending = session.execute_strategy(basis(owner), submit("already.sent", "3"));
    ASSERT_TRUE(order_has_status(
        wait_for(
            session,
            [](const Json& state) {
                return order_has_status(state, "already.sent", "accepted");
            }),
        "already.sent",
        "accepted"));
    auto stopped = session.release_strategy(owner);
    ASSERT_EQ(stopped.wait_for(1s), std::future_status::ready);
    stopped.get();
    EXPECT_TRUE(order_has_status(session.snapshot(), "already.sent", "accepted"));
    EXPECT_EQ(sending.wait_for(0s), std::future_status::timeout);
    const auto effects = exchange.call<int (*)()>("asterion_fake_catalog_side_effects");
    exchange.call<void (*)(int, int)>("asterion_fake_trader_hold_insert_return", 0, -7);
    EXPECT_NO_THROW(sending.get());
    EXPECT_TRUE(order_has_status(
        wait_for(
            session,
            [](const Json& state) {
                return order_has_status(state, "already.sent", "cancelled");
            }),
        "already.sent",
        "cancelled"));
    EXPECT_EQ(exchange.call<int (*)()>("asterion_fake_catalog_side_effects"), effects + 1);
}

TEST_F(StrategyExecution, AnInstanceCannotUseAnotherAllowedContract)
{
    auto configuration = manifest();
    configuration["policy"]["contracts"].push_back(contract("rb2611", "rb", "2026-11"));
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, configuration);
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    auto owner = session.register_strategy().get();
    EXPECT_THROW(
        session.execute_strategy(basis(owner), submit("outside.selection", "1", "3500", "rb2611"))
            .get(),
        std::invalid_argument);
    EXPECT_TRUE(session.snapshot().at("orders").empty());
}

TEST_F(StrategyExecution, FailureCancelsOnlyThisInstancesOrdersAndPreservesPositions)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest(4));
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    act(session, submit("position", "1"));
    ASSERT_TRUE(wait_for(session, [](const Json& state) {
        return state.at("positions_reconciled") == true && !state.at("positions").empty();
    }).at("positions_reconciled"));
    const auto accepted = [&](const char* id) {
        return order_has_status(
            wait_for(
                session,
                [&](const Json& state) {
                    return order_has_status(state, id, "accepted");
                }),
            id,
            "accepted");
    };
    act(session, submit("manual.resting", "3"));
    ASSERT_TRUE(accepted("manual.resting"));
    auto owner = session.register_strategy().get();
    session.execute_strategy(basis(owner), submit("strategy.first", "3")).get();
    ASSERT_TRUE(accepted("strategy.first"));
    session.execute_strategy(basis(owner, 2), submit("strategy.second", "3")).get();
    ASSERT_TRUE(accepted("strategy.second"));
    const auto effects = exchange.call<int (*)()>("asterion_fake_catalog_side_effects");
    EXPECT_THROW(
        session.execute_strategy(basis(owner, 3), submit("risk.failure", "6")).get(),
        std::invalid_argument);
    const auto after = wait_for(session, [](const Json& state) {
        return order_has_status(state, "strategy.first", "cancelled")
            && order_has_status(state, "strategy.second", "cancelled");
    });
    EXPECT_TRUE(order_has_status(after, "strategy.first", "cancelled"));
    EXPECT_TRUE(order_has_status(after, "strategy.second", "cancelled"));
    EXPECT_TRUE(order_has_status(after, "manual.resting", "accepted"));
    ASSERT_EQ(after.at("positions").size(), 1U);
    EXPECT_EQ(decimal(after.at("positions")[0].at("today")), "1");
    EXPECT_EQ(exchange.call<int (*)()>("asterion_fake_catalog_side_effects"), effects + 2);
    EXPECT_FALSE(session.strategy_failure(owner).empty());
    session.release_strategy(owner).get();
    EXPECT_EQ(exchange.call<int (*)()>("asterion_fake_catalog_side_effects"), effects + 2);
}

TEST_F(StrategyExecution, RevocationCancelsRestingOrdersAndReauthorizationCannotReviveTheInstance)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    auto owner = session.register_strategy().get();
    session.execute_strategy(basis(owner), submit("resting", "3")).get();
    act(session, {{"request_id", "revoke"}, {"action", "live_revoke"}});
    EXPECT_TRUE(order_has_status(
        wait_for(
            session,
            [](const Json& state) {
                return order_has_status(state, "resting", "cancelled");
            }),
        "resting",
        "cancelled"));
    EXPECT_EQ(session.strategy_failure(owner), "account authorization ended");
    act(session, authorize("again"));
    EXPECT_THROW(session.execute_strategy(basis(owner, 2), submit("old.target", "1")).get(), Error);
}

TEST_F(StrategyExecution, FailedCancellationIsNotRetriedByStopReconnectOrRecovery)
{
    {
        LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
        ASSERT_EQ(ready(session).at("phase"), "ready");
        act(session, authorize());
        auto owner = session.register_strategy().get();
        session.execute_strategy(basis(owner), submit("resting", "3")).get();
        ASSERT_TRUE(order_has_status(
            wait_for(
                session,
                [](const Json& state) {
                    return order_has_status(state, "resting", "accepted");
                }),
            "resting",
            "accepted"));
        const auto effects = exchange.call<int (*)()>("asterion_fake_catalog_side_effects");
        const auto records = number(session.snapshot().at("capacity").at("records_used"));
        exchange.call<void (*)(int)>("asterion_fake_trader_cancel_code", -2);
        session.release_strategy(owner).get();
        const auto after = wait_for(session, [records](const Json& state) {
            return number(state.at("capacity").at("records_used")) >= records + 2;
        });
        EXPECT_EQ(number(after.at("capacity").at("records_used")), records + 2);
        EXPECT_TRUE(order_has_status(after, "resting", "accepted"));
        EXPECT_EQ(exchange.call<int (*)()>("asterion_fake_catalog_side_effects"), effects + 1);
        EXPECT_THROW(session.release_strategy(owner).get(), Error);
        exchange.call<void (*)(int)>("asterion_fake_trader_cancel_code", 0);
        session.disconnect();
        ASSERT_EQ(ready(session).at("phase"), "ready");
        EXPECT_TRUE(order_has_status(session.snapshot(), "resting", "accepted"));
    }
    LiveSession recovered(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
    ASSERT_EQ(ready(recovered).at("phase"), "ready");
    const auto effects = exchange.call<int (*)()>("asterion_fake_catalog_side_effects");
    const auto after = recovered.snapshot();
    EXPECT_TRUE(order_has_status(after, "resting", "accepted"));
    EXPECT_FALSE(after.contains("authorization"));
    EXPECT_THROW((void)recovered.register_strategy().get(), Error);
    auto next = recovered.register_strategy("new-instance").get();
    recovered.release_strategy(next).get();
    EXPECT_EQ(exchange.call<int (*)()>("asterion_fake_catalog_side_effects"), effects);
    EXPECT_TRUE(order_has_status(recovered.snapshot(), "resting", "accepted"));
}

TEST_F(StrategyExecution, SdkRefusalLeavesNothingToCancelOrResolveAcrossRestart)
{
    {
        LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
        ASSERT_EQ(ready(session).at("phase"), "ready");
        act(session, authorize());
        auto owner = session.register_strategy().get();
        const auto records = number(session.snapshot().at("capacity").at("records_used"));
        exchange.call<void (*)(int)>("asterion_fake_trader_cancel_code", -3);
        EXPECT_THROW(session.execute_strategy(basis(owner), submit("refused", "3")).get(), Error);
        const auto effects = exchange.call<int (*)()>("asterion_fake_catalog_side_effects");
        session.release_strategy(owner).get();
        // The intent and its not-sent result; stopping finds no order to cancel.
        const auto after = session.snapshot();
        EXPECT_EQ(number(after.at("capacity").at("records_used")), records + 2);
        EXPECT_EQ(exchange.call<int (*)()>("asterion_fake_catalog_side_effects"), effects);
        EXPECT_TRUE(after.at("unconfirmed").empty());
        EXPECT_TRUE(after.at("orders").empty());
    }
    exchange.call<void (*)(int)>("asterion_fake_trader_cancel_code", 0);
    LiveSession recovered(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
    ASSERT_EQ(ready(recovered).at("phase"), "ready");
    EXPECT_TRUE(recovered.snapshot().at("unconfirmed").empty());
    act(recovered, authorize("recovered"));
    EXPECT_NO_THROW(act(recovered, submit("after.refusal", "1")));
}

TEST_F(StrategyExecution, FaultCancellationPassesAnOrderWaitingForAThrottledQuote)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest(2));
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    auto owner = session.register_strategy().get();
    session.execute_strategy(basis(owner), submit("resting", "3")).get();
    exchange.call<void (*)(int, int)>("asterion_fake_trader_reject_quotes", -3, 100);
    auto sending = session.execute_strategy(basis(owner, 2), submit("waiting", "1"));
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!exchange.call<int (*)()>("asterion_fake_trader_query_rejections")
        && std::chrono::steady_clock::now() < deadline) {
        clock->advance(250ms);
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_GT(exchange.call<int (*)()>("asterion_fake_trader_query_rejections"), 0);
    session.release_strategy(owner).get();
    EXPECT_TRUE(order_has_status(
        wait_for(
            session,
            [](const Json& state) {
                return order_has_status(state, "resting", "cancelled");
            }),
        "resting",
        "cancelled"));
    EXPECT_EQ(sending.wait_for(0s), std::future_status::timeout);
    exchange.call<void (*)(int, int)>("asterion_fake_trader_reject_quotes", 0, 0);
    clock->advance(1100ms);
    EXPECT_THROW(sending.get(), Error);
    EXPECT_TRUE(find_order(session.snapshot(), "waiting").is_null());
}

TEST_F(StrategyExecution, AccountShutdownAttemptsCancellationBeforeDisconnectingTheSdk)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    auto owner = session.register_strategy().get();
    session.execute_strategy(basis(owner), submit("resting", "3")).get();
    const auto effects = exchange.call<int (*)()>("asterion_fake_catalog_side_effects");
    clock->advance(250ms);
    session.close_admission();
    session.stop();
    ASSERT_EQ(session.stopped().wait_for(3s), std::future_status::ready);
    session.stopped().get();
    EXPECT_EQ(exchange.call<int (*)()>("asterion_fake_catalog_side_effects"), effects + 1);
}

TEST_F(StrategyExecution, StopDoesNotRepeatAnEarlierFailedManualCancellation)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    auto owner = session.register_strategy().get();
    session.execute_strategy(basis(owner), submit("resting", "3")).get();
    ASSERT_TRUE(order_has_status(
        wait_for(
            session,
            [](const Json& state) {
                return order_has_status(state, "resting", "accepted");
            }),
        "resting",
        "accepted"));
    exchange.call<void (*)(int)>("asterion_fake_trader_cancel_code", -2);
    EXPECT_THROW(
        act(session,
            {{"request_id", "cancel.manual"}, {"action", "cancel"}, {"order_id", "resting"}}),
        Error);
    const auto effects = exchange.call<int (*)()>("asterion_fake_catalog_side_effects");
    const auto before = session.snapshot().at("capacity");
    session.release_strategy(owner).get();
    const auto after = session.snapshot();
    EXPECT_EQ(exchange.call<int (*)()>("asterion_fake_catalog_side_effects"), effects);
    EXPECT_EQ(after.at("capacity"), before);
    EXPECT_TRUE(order_has_status(after, "resting", "accepted"));
    exchange.call<void (*)(int)>("asterion_fake_trader_cancel_code", 0);
}

TEST_F(StrategyExecution, DisconnectEndsTheInstanceWithoutRetryingCancellationOnReconnect)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    auto owner = session.register_strategy().get();
    session.execute_strategy(basis(owner), submit("resting", "3")).get();
    session.disconnect();
    EXPECT_FALSE(session.strategy_failure(owner).empty());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    EXPECT_TRUE(order_has_status(session.snapshot(), "resting", "accepted"));
    EXPECT_TRUE(session.snapshot().contains("authorization"));
    EXPECT_THROW(session.execute_strategy(basis(owner, 2), submit("old.target", "1")).get(), Error);
    const auto effects = exchange.call<int (*)()>("asterion_fake_catalog_side_effects");
    session.release_strategy(owner).get();
    EXPECT_EQ(exchange.call<int (*)()>("asterion_fake_catalog_side_effects"), effects);
    EXPECT_TRUE(order_has_status(session.snapshot(), "resting", "accepted"));
}

TEST_F(StrategyExecution, FaultCancellationCannotBypassFailedDurabilityOrEraseTheIntent)
{
    {
        LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
        ASSERT_EQ(ready(session).at("phase"), "ready");
        act(session, authorize());
        auto owner = session.register_strategy().get();
        session.execute_strategy(basis(owner), submit("resting", "3")).get();
        ASSERT_TRUE(order_has_status(
            wait_for(
                session,
                [](const Json& state) {
                    return order_has_status(state, "resting", "accepted");
                }),
            "resting",
            "accepted"));
        const auto effects = exchange.call<int (*)()>("asterion_fake_catalog_side_effects");
        sqlite::fail_next_commits_for_testing(1);
        session.release_strategy(owner).get();
        const auto after = wait_for(session, [&](const Json&) {
            return session.recovery_required();
        });
        EXPECT_TRUE(session.recovery_required());
        EXPECT_TRUE(order_has_status(after, "resting", "accepted"));
        EXPECT_EQ(exchange.call<int (*)()>("asterion_fake_catalog_side_effects"), effects);
    }
    LiveSession recovered(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
    ASSERT_EQ(ready(recovered).at("phase"), "ready");
    EXPECT_TRUE(order_has_status(recovered.snapshot(), "resting", "accepted"));
    act(recovered, authorize("after.recovery"));
    EXPECT_THROW(act(recovered, submit("still.occupied", "1")), std::invalid_argument);
}

// Writes fixtures/record-format/journal.sqlite: one trading record with every
// kind of entry. Run with --gtest_also_run_disabled_tests only after changing a
// record format on purpose; its release then needs a fresh record (docs/trading/deployment.md).
TEST_F(StrategyExecution, DISABLED_WriteRecordFormatFixture)
{
    const auto cancel = [](const char* order) {
        return Json{
            {"request_id", std::string("cancel.") + order},
            {"action", "cancel"},
            {"order_id", order}
        };
    };
    {
        LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest(2));
        ASSERT_EQ(ready(session).at("phase"), "ready");
        // A fill pauses opening orders until positions are reconciled.
        const auto reconciled = [&](const char* order) {
            wait_for(session, [order](const Json& state) {
                return order_has_status(state, order, "filled")
                    && state.at("positions_reconciled") == true;
            });
        };
        act(session, authorize());
        act(session, submit("filled", "1"));
        reconciled("filled");
        act(session, submit("resting", "3"));
        wait_for(session, [](const Json& state) {
            return order_has_status(state, "resting", "accepted");
        });
        act(session, cancel("resting"));
        wait_for(session, [](const Json& state) {
            return order_has_status(state, "resting", "cancelled");
        });
        exchange.call<void (*)(int)>("asterion_fake_trader_cancel_code", -3);
        EXPECT_THROW(act(session, submit("refused", "1")), Error);
        exchange.call<void (*)(int)>("asterion_fake_trader_cancel_code", 0);
        auto owner = session.register_strategy().get();
        session.execute_strategy(basis(owner), submit("strategy", "1")).get();
        reconciled("strategy");
        session.release_strategy(owner).get();
        act(session, submit("lost", "3"));
        wait_for(session, [](const Json& state) {
            return order_has_status(state, "lost", "accepted");
        });
        // The counter forgets the resting order overnight: it becomes unconfirmed.
        ctp::SharedLibrary(ASTERION_TEST_CTP_TRADER, "asterion_fake_trader_next_day")
            .symbol<void (*)(const char*)>()("20260929");
        ASSERT_EQ(
            wait_for(
                session,
                [](const Json& state) {
                    return state.at("phase") == "ready" && state.at("unconfirmed").size() == 1;
                })
                .at("unconfirmed")
                .size(),
            1U);
        act(session,
            {{"request_id", "resolve.lost"}, {"action", "live_resolve"}, {"order_id", "lost"}});
        act(session, {{"request_id", "revoke"}, {"action", "live_revoke"}});
        auto policy = manifest(2).at("policy");
        policy["max_daily_loss"] = "5000";
        policy["max_drawdown"] = "12000";
        policy["max_margin_ratio"] = "0.7";
        policy["max_daily_cancels_per_contract"] = 400;
        act(session, policy_command(session, policy));
    }
    ASSERT_FALSE(fs::exists(directory.path / "journal.sqlite-wal"));
    fs::copy_file(
        directory.path / "journal.sqlite",
        fs::path(ASTERION_TRADING_FIXTURES) / "record-format/journal.sqlite",
        fs::copy_options::overwrite_existing);
}

// The trading record that the simulation soak starts from keeps opening: every
// entry replays, its identities hold and nothing is sent again.
TEST_F(StrategyExecution, ACommittedTradingRecordStillRecovers)
{
    fs::copy_file(
        fs::path(ASTERION_TRADING_FIXTURES) / "record-format/journal.sqlite",
        directory.path / "journal.sqlite");
    LiveSession restored(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
    const auto recorded = restored.snapshot();
    EXPECT_TRUE(recorded.at("unconfirmed").empty());
    EXPECT_EQ(recorded.at("policy").at("max_daily_loss"), "5000");
    const auto day = restored.day("2026-09-28");
    EXPECT_EQ(
        std::vector<std::string>(day.trading_days().begin(), day.trading_days().end()),
        (std::vector<std::string>{"2026-09-28"}));
    EXPECT_EQ(day.close().trading_day(), "2026-09-28");
    std::set<std::string> filled;
    for (const auto& trade : day.trades())
        filled.insert(trade.order_id());
    EXPECT_EQ(filled, (std::set<std::string>{"filled", "strategy"}));
    ASSERT_EQ(ready(restored).at("phase"), "ready");
    act(restored, authorize("restored"));
    const auto effects = exchange.call<int (*)()>("asterion_fake_catalog_side_effects");
    auto reused = submit("filled", "1");
    reused["request_id"] = "submit.again";
    EXPECT_THROW(act(restored, reused), Error) << "a recorded order ID stays used";
    EXPECT_EQ(exchange.call<int (*)()>("asterion_fake_catalog_side_effects"), effects);
}
