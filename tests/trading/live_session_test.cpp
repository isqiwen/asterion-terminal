#include "trading/live_session_fixture.hpp"

using namespace asterion;
using namespace asterion::test::live;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

TEST_F(Live, PolicyRevisionsRetainExposureFenceStaleCommandsAndSurviveRecoveryAndSegments)
{
    test::SmallJournalSegments budget(2);
    std::string initial, revised, record;
    Json change;
    {
        LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest(2));
        initial = session.snapshot().at("policy_revision");
        record = session.snapshot().at("account_id");
        ASSERT_EQ(ready(session).at("phase"), "ready");
        act(session, authorize());
        act(session, submit("position", "2"));
        ASSERT_FALSE(wait_for(
            session,
            [](const Json& state) {
                return !state.at("positions").empty();
            })
                .at("positions")
                .empty());
        auto next = manifest(2).at("policy");
        next["risk"]["max_gross_quantity"] = next["risk"]["max_contract_quantity"] = "1";
        next["contracts"].push_back(contract("rb2611", "rb", "2026-11"));
        change = policy_command(session, next);
        act(session, change);
        const auto state = session.snapshot();
        revised = state.at("policy_revision");
        EXPECT_NE(revised, initial);
        EXPECT_EQ(state.at("account_id"), record);
        EXPECT_EQ(state.at("contracts").size(), 2U);
        EXPECT_EQ(state.at("phase"), "disconnected");
        EXPECT_FALSE(state.contains("authorization"));
        const auto count = session.snapshot().at("capacity").at("records_used");
        EXPECT_NO_THROW(session.execute(record, initial, change).get());
        EXPECT_EQ(session.snapshot().at("capacity").at("records_used"), count);
        EXPECT_THROW(session.execute(record, initial, submit("stale", "1")).get(), Error);
        ASSERT_EQ(ready(session).at("phase"), "ready");
        act(session, authorize("new.policy.authorization"));
        EXPECT_THROW(act(session, submit("risk.remains", "1")), std::invalid_argument);
        auto close = submit("close", "2");
        close["side"] = "sell";
        close["offset"] = "close_today";
        auto over_close = close;
        over_close["order_id"] = "over.close";
        over_close["quantity"] = "3";
        EXPECT_THROW(act(session, over_close), std::invalid_argument);
        EXPECT_NO_THROW(act(session, close));
        EXPECT_TRUE(wait_for(
            session,
            [](const Json& state) {
                return state.at("positions").empty();
            })
                .at("positions")
                .empty());
    }
    {
        LiveSession recovered(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
        EXPECT_EQ(recovered.snapshot().at("policy_revision"), revised);
        EXPECT_EQ(decimal(recovered.snapshot().at("risk").at("max_gross_quantity")), "1");
        EXPECT_NO_THROW(recovered.execute(record, initial, change).get());
        ASSERT_EQ(ready(recovered).at("phase"), "ready");
        EXPECT_EQ(recovered.snapshot().at("policy_revision"), revised);
        EXPECT_EQ(recovered.snapshot().at("account_id"), record);
    }
    LiveSession continued(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
    EXPECT_EQ(continued.snapshot().at("policy_revision"), revised);
    EXPECT_EQ(decimal(continued.snapshot().at("risk").at("max_gross_quantity")), "1");
}

TEST_F(Live, PolicyCannotRemoveOrRedefineContractsWithWorkingOrders)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    act(session, submit("resting", "3"));
    const auto revision = session.snapshot().at("policy_revision");
    auto next = manifest().at("policy");
    next["contracts"] = Json::array({contract("rb2611", "rb", "2026-11")});
    EXPECT_THROW(act(session, policy_command(session, next)), Error);
    next = manifest().at("policy");
    next["contracts"][0]["multiplier"] = "20";
    EXPECT_THROW(act(session, policy_command(session, next)), Error);
    EXPECT_EQ(session.snapshot().at("policy_revision"), revision);
    EXPECT_EQ(session.snapshot().at("phase"), "ready");
}

TEST_F(Live, PolicyJournalFailureKeepsOriginalRevisionAndRequiresRecovery)
{
    std::string revision;
    {
        LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
        revision = session.snapshot().at("policy_revision");
        ASSERT_EQ(ready(session).at("phase"), "ready");
        act(session, authorize());
        auto next = manifest().at("policy");
        next["risk"]["max_gross_quantity"] = next["risk"]["max_contract_quantity"] = "8";
        const auto command = policy_command(session, next);
        sqlite::fail_next_commits_for_testing(1);
        EXPECT_THROW(act(session, command), std::runtime_error);
        EXPECT_TRUE(session.recovery_required());
        EXPECT_EQ(session.snapshot().at("phase"), "disconnected");
        EXPECT_EQ(session.snapshot().at("policy_revision"), revision);
        EXPECT_FALSE(session.snapshot().contains("authorization"));
    }
    LiveSession recovered(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
    EXPECT_EQ(recovered.snapshot().at("policy_revision"), revision);
    EXPECT_EQ(decimal(recovered.snapshot().at("risk").at("max_gross_quantity")), "10");
}

TEST_F(Live, AccountRecordOwnershipSurvivesConnectionChangesAndProcessLifetimes)
{
    Directory alias;
    auto alternative = manifest();
    alternative["account_id"] = "another-account";
    alternative["environment"] = "real";
    alternative["broker"]["front"] = "tcp://127.0.0.1:41206";
    alternative["broker"]["app_id"] = "another_app";
    {
        LiveSession owner(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
        ASSERT_EQ(ready(owner).at("phase"), "ready");
        act(owner, authorize());
        EXPECT_THROW(
            (LiveSession{alias.path, ASTERION_TEST_CTP_TRADER, owners.path, alternative}),
            Error);
        EXPECT_TRUE(fs::is_empty(alias.path));
    }
    // Releasing the process lock does not make a second risk history valid.
    EXPECT_THROW(
        (LiveSession{alias.path, ASTERION_TEST_CTP_TRADER, owners.path, alternative}),
        Error);
    EXPECT_TRUE(fs::is_empty(alias.path));
    LiveSession recovered(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
    EXPECT_FALSE(recovered.snapshot().contains("authorization"));
}

TEST_F(Live, DifferentBrokerAccountsCanRunTogether)
{
    LiveSession first(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    Directory other_investor, other_broker;
    auto investor = manifest();
    investor["account_id"] = "other-investor";
    investor["broker"]["user_id"] = "000002";
    LiveSession second(other_investor.path, ASTERION_TEST_CTP_TRADER, owners.path, investor);
    auto broker = manifest();
    broker["account_id"] = "other-broker";
    broker["broker"]["broker_id"] = "8888";
    LiveSession third(other_broker.path, ASTERION_TEST_CTP_TRADER, owners.path, broker);
}

TEST_F(Live, MissingInitializedJournalCannotBeRecreatedAsAnEmptyAccount)
{
    {
        LiveSession owner(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    }
    Directory held;
    fs::rename(directory.path / "journal.sqlite", held.path / "journal.sqlite");
    EXPECT_THROW(
        (LiveSession{directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest()}),
        Error);
    EXPECT_FALSE(fs::exists(directory.path / "journal.sqlite"));
    fs::rename(held.path / "journal.sqlite", directory.path / "journal.sqlite");
    EXPECT_NO_THROW((LiveSession{directory.path, ASTERION_TEST_CTP_TRADER, owners.path}));
}

TEST_F(Live, CancellationJournalFailureCannotReachTheBroker)
{
    {
        LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
        ASSERT_EQ(ready(session).at("phase"), "ready");
        act(session, authorize());
        act(session, submit("resting", "4"));
        ASSERT_EQ(
            find_order(
                wait_for(
                    session,
                    [](const Json& state) {
                        return order_has_status(state, "resting", "accepted");
                    }),
                "resting")
                .at("status"),
            "accepted");
        const auto effects = exchange.call<int (*)()>("asterion_fake_catalog_side_effects");
        sqlite::fail_next_commits_for_testing(1);
        EXPECT_THROW(
            act(session, {{"request_id", "cancel"}, {"action", "cancel"}, {"order_id", "resting"}}),
            std::runtime_error);
        EXPECT_EQ(exchange.call<int (*)()>("asterion_fake_catalog_side_effects"), effects);
        EXPECT_EQ(find_order(session.snapshot(), "resting").at("status"), "accepted");
        EXPECT_THROW(act(session, submit("blocked", "1")), std::invalid_argument);
    }
    EXPECT_EQ(test::journal_size(directory.path), 3U);
}

TEST_F(Live, FailedCancellationRecordsUnknownOutcomeAndNeverReplaysTheRequest)
{
    const Json cancel{{"request_id", "cancel"}, {"action", "cancel"}, {"order_id", "resting"}};
    {
        LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
        ASSERT_EQ(ready(session).at("phase"), "ready");
        act(session, authorize());
        act(session, submit("resting", "4"));
        exchange.call<void (*)(int)>("asterion_fake_trader_cancel_code", -2);
        EXPECT_THROW(act(session, cancel), Error);
        exchange.call<void (*)(int)>("asterion_fake_trader_cancel_code", 0);
        const auto effects = exchange.call<int (*)()>("asterion_fake_catalog_side_effects");
        EXPECT_NO_THROW(act(session, cancel));
        EXPECT_EQ(exchange.call<int (*)()>("asterion_fake_catalog_side_effects"), effects);
    }
    ASSERT_EQ(test::journal_size(directory.path), 5U);
    EXPECT_EQ(
        test::read_record(test::journal_record(directory.path, 3)),
        Json(
            {{"command", cancel},
                {"policy_revision",
                    test::read_record(test::journal_record(directory.path, 0))
                        .at("policy_revision")}}));
    EXPECT_EQ(
        test::read_record(test::journal_record(directory.path, 4)),
        Json({{"cancel_result", "cancel"}, {"outcome", "unknown"}}));
    {
        LiveSession recovered(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
        ASSERT_EQ(ready(recovered).at("phase"), "ready");
        const auto effects = exchange.call<int (*)()>("asterion_fake_catalog_side_effects");
        EXPECT_NO_THROW(act(recovered, cancel));
        EXPECT_EQ(exchange.call<int (*)()>("asterion_fake_catalog_side_effects"), effects);
        EXPECT_EQ(find_order(recovered.snapshot(), "resting").at("status"), "accepted");
    }
}

TEST_F(Live, IncompleteFailedOrForeignDayQuotesNeverJournalAnOrder)
{
    {
        LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
        ASSERT_EQ(ready(session).at("phase"), "ready");
        act(session, authorize());
        for (const int mode : {1, 3, 4}) {
            exchange.call<void (*)(int)>("asterion_fake_trader_quote_mode", mode);
            EXPECT_THROW(
                act(session, submit("quote." + std::to_string(mode), "1")),
                std::invalid_argument);
            EXPECT_TRUE(session.snapshot().at("orders").empty());
        }
        exchange.call<void (*)(int)>("asterion_fake_trader_quote_mode", 5);
        EXPECT_NO_THROW(act(session, submit("completed", "1")));
    }
    SqliteJournal journal(directory.path, {"plugins", "ctp-flow", "owner"});
    journal.start();
    for (const int mode : {1, 3, 4})
        EXPECT_TRUE(journal.command("submit.quote." + std::to_string(mode)).is_null());
    EXPECT_EQ(journal.command("submit.completed").at("command"), submit("completed", "1"));
}

TEST_F(Live, MissingFundsRejectOpeningWithoutOptionalCapitalLimits)
{
    exchange.call<void (*)(int)>("asterion_fake_trader_empty_funds", 1);
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    ASSERT_FALSE(session.wire_snapshot().has_funds());
    act(session, authorize());
    EXPECT_THROW(act(session, submit("no.funds", "1")), Error);
    EXPECT_EQ(session.orders().orders_size(), 0);
}

TEST_F(Live, FilledOrdersDoNotCreateRiskHeadroomBeforePositionsAreReconciled)
{
    auto input = manifest(3);
    input["policy"]["risk"]["max_gross_quantity"] =
        input["policy"]["risk"]["max_contract_quantity"] = "2";
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, input);
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    exchange.call<void (*)(int)>("asterion_fake_trader_hold_positions", 1);
    act(session, submit("filled", "2"));
    auto state = wait_for(session, [](const Json& s) {
        const auto order = find_order(s, "filled");
        return order.is_object() && order.at("status") == "filled";
    });
    ASSERT_EQ(find_order(state, "filled").at("status"), "filled");
    ASSERT_TRUE(state.at("positions").empty());
    EXPECT_THROW(act(session, submit("too.early", "2")), Error);
    EXPECT_TRUE(find_order(session.snapshot(), "too.early").is_null());
    exchange.call<void (*)(int)>("asterion_fake_trader_hold_positions", 0);
    state = wait_for(session, [](const Json& s) {
        return !s.at("positions").empty();
    });
    ASSERT_EQ(decimal(state.at("positions")[0].at("today")), "2");
    EXPECT_THROW(act(session, submit("over.limit", "2")), std::invalid_argument);
    EXPECT_TRUE(find_order(session.snapshot(), "over.limit").is_null());
    EXPECT_EQ(session.snapshot().at("orders").size(), 1U);
}

TEST_F(Live, SlowDurabilityAllowsSnapshotsAndRevocationWithoutSendingThePreparedOrder)
{
    test::SmallJournalSegments budget(1);
    {
        LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
        ASSERT_EQ(ready(session).at("phase"), "ready");
        act(session, authorize());
        const auto before = session.snapshot();
        std::future<void> placed, revoked, queued_grant, queued_order, queued_cancel;
        std::vector<std::future<void>> backlog;
        std::future<Json> observed;
        // Declared after futures so the barrier is released before they are destroyed.
        HeldCommit held;
        placed = std::async(std::launch::async, [&] {
            act(session, submit("slow", "2"));
        });
        ASSERT_TRUE(held.waiting());
        std::this_thread::sleep_for(750ms);
        const auto health = session.health();
        EXPECT_TRUE(health.state.observed);
        EXPECT_LT(health.state.age_ms, 700U);
        EXPECT_TRUE(health.persistence.pending);
        EXPECT_GE(health.persistence.age_ms, 750U);
        EXPECT_TRUE(health.command.pending);
        EXPECT_FALSE(session.recovery_required());
        observed = std::async(std::launch::async, [&] {
            return session.snapshot();
        });
        ASSERT_EQ(observed.wait_for(1s), std::future_status::ready);
        const auto during = observed.get();
        EXPECT_TRUE(during.at("orders").empty());
        EXPECT_EQ(during.at("capacity"), before.at("capacity"));
        const auto record = before.at("account_id").get<std::string>();
        const auto policy = before.at("policy_revision").get<std::string>();
        queued_grant = session.execute(record, policy, authorize("queued.grant"));
        queued_order = session.execute(record, policy, submit("queued.order", "1"));
        for (int i = 0; i < 61; ++i)
            backlog.push_back(session.execute(record, policy, submit("queued.order", "1")));
        EXPECT_THROW(
            static_cast<void>(session.execute(record, policy, submit("overflow", "1"))),
            Error);
        // Controls and durable completion retain capacity with all 64 normal slots occupied.
        queued_cancel = session.execute(
            record,
            policy,
            {{"request_id", "cancel.slow"}, {"action", "cancel"}, {"order_id", "slow"}});
        // FIFO read proves these requests have entered account admission before revoke.
        EXPECT_TRUE(session.snapshot().contains("authorization"));
        revoked = std::async(std::launch::async, [&] {
            act(session, {{"request_id", "revoke.slow"}, {"action", "live_revoke"}});
        });
        observed = std::async(std::launch::async, [&] {
            return wait_for(session, [](const Json& state) {
                return !state.contains("authorization");
            });
        });
        ASSERT_EQ(observed.wait_for(1s), std::future_status::ready);
        EXPECT_FALSE(observed.get().contains("authorization"));
        EXPECT_EQ(placed.wait_for(0ms), std::future_status::timeout);
        EXPECT_EQ(revoked.wait_for(0ms), std::future_status::timeout);
        held.release();
        EXPECT_THROW(placed.get(), Error);
        EXPECT_THROW(queued_grant.get(), Error);
        EXPECT_THROW(queued_order.get(), Error);
        for (auto& request : backlog)
            EXPECT_THROW(request.get(), Error);
        EXPECT_THROW(queued_cancel.get(), Error) << "the completed intent is definitely not sent";
        EXPECT_NO_THROW(revoked.get());
        const auto after = session.snapshot();
        EXPECT_TRUE(after.at("orders").empty());
        EXPECT_TRUE(after.at("unconfirmed").empty());
        EXPECT_GT(number(after.at("segment_count")), number(before.at("segment_count")));
        EXPECT_EQ(after.at("account_id"), before.at("account_id"));
        EXPECT_NO_THROW(act(session, submit("slow", "2"))) << "retry acknowledges, never resends";
    }
    LiveSession recovered(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
    const auto state = ready(recovered);
    EXPECT_TRUE(state.at("orders").empty());
    EXPECT_TRUE(state.at("unconfirmed").empty());
    EXPECT_FALSE(state.contains("authorization"));
}

TEST_F(Live, CommandsQueuedBeforeEntryRevocationCannotBorrowItsNewRevision)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    std::future<void> evaluating, queued_grant, revoked;
    act(session, authorize());
    const auto before = session.snapshot();
    const auto record = before.at("account_id").get<std::string>();
    const auto policy = before.at("policy_revision").get<std::string>();
    HeldCommit held;
    evaluating = session.execute(record, policy, submit("slow.risk", "1"));
    ASSERT_TRUE(held.waiting());
    const auto blocked = session.health();
    std::this_thread::sleep_for(50ms);
    const auto later = session.health();
    EXPECT_TRUE(later.persistence.pending);
    EXPECT_GE(later.persistence.age_ms, blocked.persistence.age_ms + 50);
    EXPECT_TRUE(later.persistence.pending);
    EXPECT_FALSE(session.recovery_required());
    // Both requests enter while the order intent is being made durable.
    // Taking the old grant's revision only when the owner dequeues it would let
    // that grant borrow the revoke's newer revision and restore authorization.
    queued_grant = session.execute(record, policy, authorize("queued.before.revoke"));
    revoked = session.execute(
        record,
        policy,
        {{"request_id", "entry.revoke"}, {"action", "live_revoke"}});
    held.release();
    EXPECT_THROW(evaluating.get(), Error);
    EXPECT_THROW(queued_grant.get(), Error);
    EXPECT_NO_THROW(revoked.get());
    const auto after = session.snapshot();
    EXPECT_FALSE(after.contains("authorization"));
    EXPECT_TRUE(after.at("orders").empty());
    EXPECT_EQ(
        number(after.at("capacity").at("records_used")),
        number(before.at("capacity").at("records_used")) + 3);
    EXPECT_NO_THROW(act(session, authorize("fresh.after.revoke")));
    EXPECT_TRUE(session.snapshot().contains("authorization"));
}

TEST_F(Live, RevocationDuringDurableAuthorizationCannotRestorePermission)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize("initial.grant"));
    std::future<void> granted, revoked;
    std::future<Json> observed;
    HeldCommit held;
    granted = std::async(std::launch::async, [&] {
        act(session, authorize("renew.grant"));
    });
    ASSERT_TRUE(held.waiting());
    revoked = std::async(std::launch::async, [&] {
        act(session, {{"request_id", "revoke.pending.grant"}, {"action", "live_revoke"}});
    });
    observed = std::async(std::launch::async, [&] {
        return wait_for(session, [](const Json& state) {
            return !state.contains("authorization");
        });
    });
    ASSERT_EQ(observed.wait_for(1s), std::future_status::ready);
    EXPECT_FALSE(observed.get().contains("authorization"));
    held.release();
    EXPECT_THROW(granted.get(), Error);
    EXPECT_NO_THROW(revoked.get());
    EXPECT_FALSE(session.snapshot().contains("authorization"));
    EXPECT_THROW(act(session, submit("after.revocation", "1")), std::invalid_argument);
}

TEST_F(Live, SlowSdkReturnsKeepObservationAndRevocationResponsiveAndRetainLateResults)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    const auto identity = session.snapshot();
    const auto record = identity.at("account_id").get<std::string>();
    const auto policy = identity.at("policy_revision").get<std::string>();
    std::future<void> placed, revoked, cancelled;
    std::future<Json> observed;

    struct Release
    {
        ~Release()
        {
            exchange.call<void (*)(int, int)>("asterion_fake_trader_hold_insert_return", 0, 0);
            exchange.call<void (*)(int)>("asterion_fake_trader_hold_cancel_return", 0);
        }

        FakeExchange& exchange;
    } release{exchange};

    exchange.call<void (*)(int, int)>("asterion_fake_trader_hold_insert_return", 1, -7);
    placed = session.execute(record, policy, submit("slow.sdk", "3"));
    observed = std::async(std::launch::async, [&] {
        return wait_for(session, [](const Json& state) {
            return order_has_status(state, "slow.sdk", "accepted");
        });
    });
    ASSERT_EQ(observed.wait_for(3s), std::future_status::ready);
    EXPECT_TRUE(order_has_status(observed.get(), "slow.sdk", "accepted"));
    revoked =
        session.execute(record, policy, {{"request_id", "revoke.sdk"}, {"action", "live_revoke"}});
    observed = std::async(std::launch::async, [&] {
        return session.snapshot();
    });
    ASSERT_EQ(observed.wait_for(1s), std::future_status::ready);
    EXPECT_FALSE(observed.get().contains("authorization"));
    // A response taking longer than the previous five-second wait is still owned.
    EXPECT_EQ(placed.wait_for(5100ms), std::future_status::timeout);
    exchange.call<void (*)(int, int)>("asterion_fake_trader_hold_insert_return", 0, -7);
    EXPECT_NO_THROW(placed.get()) << "late SDK error cannot replace an actual accepted report";
    EXPECT_NO_THROW(revoked.get());
    exchange.call<void (*)(int)>("asterion_fake_trader_hold_cancel_return", 1);
    cancelled = session.execute(
        record,
        policy,
        {{"request_id", "cancel.sdk"}, {"action", "cancel"}, {"order_id", "slow.sdk"}});
    observed = std::async(std::launch::async, [&] {
        return wait_for(session, [](const Json& state) {
            return order_has_status(state, "slow.sdk", "cancelled");
        });
    });
    ASSERT_EQ(observed.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(order_has_status(observed.get(), "slow.sdk", "cancelled"));
    EXPECT_EQ(cancelled.wait_for(0s), std::future_status::timeout);
    session.disconnect();
    EXPECT_EQ(session.snapshot().at("phase"), "disconnected");
    exchange.call<void (*)(int)>("asterion_fake_trader_hold_cancel_return", 0);
    EXPECT_NO_THROW(cancelled.get());
}

TEST_F(Live, ThrottledQuoteWaitAllowsControlAndDisconnectRetiresItWithoutAnOrder)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    const auto identity = session.snapshot();
    const auto record = identity.at("account_id").get<std::string>();
    const auto policy = identity.at("policy_revision").get<std::string>();
    std::future<void> placed, revoked;
    std::future<Json> observed;

    struct Release
    {
        ~Release()
        {
            exchange.call<void (*)(int, int)>("asterion_fake_trader_reject_quotes", 0, 0);
        }

        FakeExchange& exchange;
    } release{exchange};

    exchange.call<void (*)(int, int)>("asterion_fake_trader_reject_quotes", -2, 100000);
    placed = session.execute(record, policy, submit("waiting.quote", "1"));
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!exchange.call<int (*)()>("asterion_fake_trader_query_rejections")
        && std::chrono::steady_clock::now() < deadline) {
        clock->advance(250ms);
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_GT(exchange.call<int (*)()>("asterion_fake_trader_query_rejections"), 0);
    revoked = session.execute(
        record,
        policy,
        {{"request_id", "revoke.quote"}, {"action", "live_revoke"}});
    observed = std::async(std::launch::async, [&] {
        return session.snapshot();
    });
    ASSERT_EQ(observed.wait_for(1s), std::future_status::ready);
    EXPECT_FALSE(observed.get().contains("authorization"));
    EXPECT_EQ(placed.wait_for(0s), std::future_status::timeout);
    session.disconnect();
    EXPECT_THROW(placed.get(), Error);
    EXPECT_NO_THROW(revoked.get());
    const auto after = session.snapshot();
    EXPECT_TRUE(after.at("orders").empty());
    EXPECT_TRUE(after.at("unconfirmed").empty());
    EXPECT_EQ(
        number(after.at("capacity").at("records_used")),
        number(identity.at("capacity").at("records_used")) + 1);
}

// A submission waiting for its broker quote has recorded nothing, so a cancel of
// an order the account already holds does not wait behind it. A cancel of the
// order that submission is about to place keeps its place in the queue.
TEST_F(Live, CancelOfARecordedOrderRunsWhileASubmissionWaitsForItsQuote)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    act(session, submit("resting", "4"));
    const auto identity = wait_for(session, [](const Json& state) {
        return order_has_status(state, "resting", "accepted");
    });
    const auto record = identity.at("account_id").get<std::string>();
    const auto policy = identity.at("policy_revision").get<std::string>();
    const auto cancel = [](const char* request, const char* order) {
        return Json{{"request_id", request}, {"action", "cancel"}, {"order_id", order}};
    };
    std::future<void> placed, premature, cancelled;

    struct Release
    {
        ~Release()
        {
            exchange.call<void (*)(int, int)>("asterion_fake_trader_reject_quotes", 0, 0);
        }

        FakeExchange& exchange;
    } release{exchange};

    exchange.call<void (*)(int, int)>("asterion_fake_trader_reject_quotes", -2, 100000);
    placed = session.execute(record, policy, submit("waiting", "3"));
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!exchange.call<int (*)()>("asterion_fake_trader_query_rejections")
        && std::chrono::steady_clock::now() < deadline) {
        clock->advance(250ms);
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_GT(exchange.call<int (*)()>("asterion_fake_trader_query_rejections"), 0);
    premature = session.execute(record, policy, cancel("cancel.waiting", "waiting"));
    cancelled = session.execute(record, policy, cancel("cancel.resting", "resting"));
    ASSERT_EQ(cancelled.wait_for(3s), std::future_status::ready);
    EXPECT_NO_THROW(cancelled.get());
    EXPECT_EQ(placed.wait_for(0s), std::future_status::timeout);
    EXPECT_EQ(premature.wait_for(0s), std::future_status::timeout);
    EXPECT_EQ(
        find_order(
            wait_for(
                session,
                [](const Json& state) {
                    return order_has_status(state, "resting", "cancelled");
                }),
            "resting")
            .at("status"),
        "cancelled");
    // The cancellation report changed the basis the waiting submission was
    // admitted on. It is refused as any submission overtaken by a broker report
    // is, and its own cancel then finds nothing to cancel.
    exchange.call<void (*)(int, int)>("asterion_fake_trader_reject_quotes", 0, 0);
    clock->advance(1100ms);
    EXPECT_THROW(placed.get(), Error);
    EXPECT_THROW(premature.get(), Error);
    const auto after = session.snapshot();
    EXPECT_TRUE(after.at("unconfirmed").empty());
    EXPECT_EQ(after.at("orders").size(), 1U);
    EXPECT_NO_THROW(act(session, submit("next", "3"))) << "the account keeps trading";
}

TEST_F(Live, QuoteDeadlineAdvancesWithoutPollingWhileTheSdkCallIsBlocked)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    const auto before = session.snapshot();

    struct Release
    {
        ~Release()
        {
            exchange.call<void (*)(int)>("asterion_fake_trader_hold_quote_return", 0);
        }

        FakeExchange& exchange;
    } release{exchange};

    exchange.call<void (*)(int)>("asterion_fake_trader_hold_quote_return", 1);
    auto placed = session.execute(
        before.at("account_id").get<std::string>(),
        before.at("policy_revision").get<std::string>(),
        submit("expired.quote", "1"));
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!exchange.call<int (*)()>("asterion_fake_trader_quote_return_waiting")
        && std::chrono::steady_clock::now() < deadline) {
        clock->advance(250ms);
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_TRUE(exchange.call<int (*)()>("asterion_fake_trader_quote_return_waiting"));
    // No snapshot, control request or external polling drives the account here.
    clock->advance(11s);
    ASSERT_EQ(placed.wait_for(2s), std::future_status::ready);
    EXPECT_THROW(placed.get(), std::invalid_argument);
    EXPECT_TRUE(exchange.call<int (*)()>("asterion_fake_trader_quote_return_waiting"));
    const auto after = session.snapshot();
    EXPECT_TRUE(after.at("orders").empty());
    EXPECT_TRUE(after.at("unconfirmed").empty());
    EXPECT_EQ(after.at("capacity"), before.at("capacity"));
}

TEST_F(Live, ConcurrentRetriesHaveOneDurableIntentAndOneBrokerOrder)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    const auto before = session.snapshot();
    const auto record = before.at("account_id").get<std::string>();
    const auto policy = before.at("policy_revision").get<std::string>();
    const auto command = submit("concurrent", "2");
    std::barrier start(5);
    std::vector<std::future<void>> callers;
    for (int i = 0; i < 4; ++i)
        callers.push_back(std::async(std::launch::async, [&] {
            start.arrive_and_wait();
            session.execute(record, policy, command).get();
            EXPECT_EQ(session.snapshot().at("account_id"), record);
        }));
    start.arrive_and_wait();
    for (auto& caller : callers)
        EXPECT_NO_THROW(caller.get());
    const auto after = wait_for(session, [](const Json& state) {
        return order_has_status(state, "concurrent", "filled") && !state.at("positions").empty();
    });
    EXPECT_EQ(after.at("orders").size(), 1U);
    EXPECT_EQ(after.at("trades").size(), 1U);
    ASSERT_EQ(after.at("positions").size(), 1U);
    EXPECT_EQ(decimal(after.at("positions")[0].at("today")), "2");
    session.stop();
    session.stopped().get();
    SqliteJournal journal(directory.path, {"plugins", "ctp-flow", "owner"});
    journal.start();
    EXPECT_EQ(journal.command("submit.concurrent").at("command"), command);
    EXPECT_EQ(journal.order_sequence("concurrent"), journal.command_sequence("submit.concurrent"));
}

TEST_F(Live, OrdersPassAuthorizationAllowlistUnitsAndRiskBeforeReachingTheBroker)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    EXPECT_EQ(session.snapshot().at("phase"), "disconnected");
    EXPECT_THROW(act(session, submit("early", "1")), std::invalid_argument);
    EXPECT_THROW(act(session, authorize()), Error) << "authorizing needs a ready account";
    ASSERT_EQ(ready(session).at("phase"), "ready");
    EXPECT_THROW(act(session, submit("unauthorized", "1")), std::invalid_argument);
    EXPECT_THROW(act(session, authorize("other", "000002")), std::invalid_argument);
    act(session, authorize());
    EXPECT_TRUE(session.snapshot().contains("authorization"));
    EXPECT_THROW(act(session, submit("listed", "1", "3500", "rb2611")), std::invalid_argument);
    EXPECT_THROW(act(session, submit("tick", "1", "3500.5")), std::invalid_argument);
    EXPECT_THROW(act(session, submit("lot", "0.5")), std::invalid_argument);
    try {
        act(session, submit("far", "1", "3600"));
        ADD_FAILURE() << "2.9% from the latest price exceeds the 2% bound";
    } catch (const std::invalid_argument& error) {
        EXPECT_NE(std::string(error.what()).find("deviates"), std::string::npos);
    }
    try {
        act(session, submit("large", "6"));
        ADD_FAILURE() << "risk must reject an oversized order";
    } catch (const std::invalid_argument& error) {
        EXPECT_NE(std::string(error.what()).find("order_quantity"), std::string::npos);
    }
    EXPECT_TRUE(session.snapshot().at("orders").empty()) << "nothing rejected reaches the broker";
    session.query_costs();
    const auto rates = wait_for(session, [](const Json& s) {
        return s.at("costs").size() == 1 && s.at("costs")[0].at("state") == "ready";
    });
    ASSERT_EQ(rates.at("costs")[0].at("state"), "ready");
    EXPECT_EQ(decimal(rates.at("costs")[0].at("costs").at("margin_rate")), "0.12");

    act(session, submit("filled", "2"));
    auto state = wait_for(session, [](const Json& s) {
        return find_order(s, "filled").is_object() && order_has_status(s, "filled", "filled")
            && !s.at("trades").empty() && !s.at("positions").empty();
    });
    EXPECT_EQ(find_order(state, "filled").at("status"), "filled");
    ASSERT_EQ(state.at("positions").size(), 1U);
    EXPECT_EQ(decimal(state.at("positions")[0].at("today")), "2");
    EXPECT_EQ(state.at("trades").size(), 1U);

    act(session, submit("resting", "3"));
    state = wait_for(session, [](const Json& s) {
        return find_order(s, "resting").is_object() && order_has_status(s, "resting", "accepted");
    });
    EXPECT_EQ(find_order(state, "resting").at("status"), "accepted");
    try {
        act(session, submit("second", "1"));
        ADD_FAILURE() << "one working order is the limit";
    } catch (const std::invalid_argument& error) {
        EXPECT_NE(std::string(error.what()).find("working_orders"), std::string::npos);
    }
    // A retried request is acknowledged without sending again; a reused ID is refused.
    const auto orders = session.snapshot().at("orders").size();
    act(session, submit("resting", "3"));
    EXPECT_EQ(session.snapshot().at("orders").size(), orders);
    EXPECT_THROW(act(session, submit("resting", "4")), Error);

    act(session, {{"request_id", "cancel"}, {"action", "cancel"}, {"order_id", "resting"}});
    state = wait_for(session, [](const Json& s) {
        return order_has_status(s, "resting", "cancelled");
    });
    EXPECT_EQ(find_order(state, "resting").at("status"), "cancelled");
    act(session, {{"request_id", "revoke"}, {"action", "live_revoke"}});
    EXPECT_FALSE(session.snapshot().contains("authorization"));
    EXPECT_THROW(act(session, submit("revoked", "1")), std::invalid_argument);
    EXPECT_THROW(act(session, authorize()), Error) << "authorization IDs are never reused";
    act(session, authorize("authorize.again"));
    session.disconnect();
    EXPECT_TRUE(session.snapshot().contains("authorization"))
        << "a disconnect ends the connection, not the owner's permission";
    try {
        act(session, submit("disconnected", "1"));
        FAIL() << "an order was accepted without a broker connection";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::unavailable);
    }
}

TEST_F(Live, LimitPricesStayWithinExchangeLimitsAndTheMarketReference)
{
    auto wide = manifest(2, "0.1");
    wide["policy"]["contracts"] = Json::array(
        {contract("rb2610"), contract("rb2611", "rb", "2026-11"), contract("zz2610", "zz")});
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, wide);
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    const auto refused = [&](Json command, const char* reason) {
        try {
            act(session, command);
            ADD_FAILURE() << command.dump();
        } catch (const std::invalid_argument& error) {
            EXPECT_NE(std::string(error.what()).find(reason), std::string::npos) << error.what();
        }
    };
    // Within 10% of 3500, but above the exchange's upper limit of 3700.
    refused(submit("limit", "1", "3750"), "price limits");
    refused(submit("quiet", "1", "3500", "zz2610"), "no current market price");
    // rb2611 has not traded: the pre-settlement price 3490 is the reference.
    act(session, submit("settled", "1", "3500", "rb2611"));
    ASSERT_FALSE(wait_for(
        session,
        [](const Json& s) {
            return !s.at("positions").empty();
        })
            .at("positions")
            .empty());
    act(session, submit("near", "1", "3690"));
    EXPECT_EQ(
        wait_for(
            session,
            [](const Json& state) {
                return state.at("orders").size() == 2;
            })
            .at("orders")
            .size(),
        2U);
    EXPECT_EQ(decimal(session.snapshot().at("max_price_deviation")), "0.1");
}

TEST_F(Live, UnreconciledClosingFillsReduceAvailableLotsWithoutBlockingSafeCloses)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    act(session, submit("open", "2"));
    ASSERT_TRUE(wait_for(session, [](const Json& state) {
        return state.at("positions_reconciled") == true && state.at("trades").size() == 1;
    }).at("positions_reconciled"));
    exchange.call<void (*)(int)>("asterion_fake_trader_hold_positions", 1);
    const auto close = [](const char* id, const char* quantity) {
        auto request = submit(id, quantity);
        request["side"] = "sell";
        request["offset"] = "close_today";
        return request;
    };
    act(session, close("first.close", "1"));
    ASSERT_FALSE(wait_for(session, [](const Json& state) {
        return state.at("trades").size() == 2;
    }).at("positions_reconciled"));
    wait_for(session, [&](const Json&) {
        return exchange.call<int (*)()>("asterion_fake_trader_pending_positions") > 0;
    });
    ASSERT_GT(exchange.call<int (*)()>("asterion_fake_trader_pending_positions"), 0);
    // Let the stalled position query expire before asking the counter for a closing price.
    clock->advance(11s);
    EXPECT_FALSE(session.wire_snapshot().positions_reconciled());
    const auto effects = exchange.call<int (*)()>("asterion_fake_catalog_side_effects");
    EXPECT_THROW(act(session, close("over.close", "2")), std::invalid_argument);
    EXPECT_EQ(exchange.call<int (*)()>("asterion_fake_catalog_side_effects"), effects);
    EXPECT_NO_THROW(act(session, close("remaining.close", "1")));
    EXPECT_EQ(
        wait_for(
            session,
            [](const Json& state) {
                return state.at("trades").size() == 3;
            })
            .at("trades")
            .size(),
        3U);
    exchange.call<void (*)(int)>("asterion_fake_trader_hold_positions", 0);
    EXPECT_TRUE(wait_for(
        session,
        [](const Json& state) {
            return state.at("positions_reconciled") == true && state.at("positions").empty();
        })
            .at("positions")
            .empty());
}

TEST_F(Live, RecoveryAttributesRecordedOrdersAndNeverResendsThem)
{
    {
        LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest(2));
        ASSERT_EQ(ready(session).at("phase"), "ready");
        act(session, authorize());
        act(session, submit("resting", "3"));
        ASSERT_EQ(
            find_order(
                wait_for(
                    session,
                    [](const Json& s) {
                        return order_has_status(s, "resting", "accepted");
                    }),
                "resting")
                .at("status"),
            "accepted");
    }
    LiveSession restored(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
    EXPECT_FALSE(restored.snapshot().contains("authorization"));
    EXPECT_THROW(act(restored, submit("after", "1")), std::invalid_argument);
    auto state = ready(restored);
    state = wait_for(restored, [](const Json& s) {
        return find_order(s, "resting").is_object();
    });
    ASSERT_TRUE(find_order(state, "resting").is_object()) << "broker report keeps its order ID";
    EXPECT_TRUE(state.at("unconfirmed").empty());
    const auto orders = state.at("orders").size();
    act(restored, submit("resting", "3"));
    EXPECT_EQ(restored.snapshot().at("orders").size(), orders) << "never resent after recovery";
    EXPECT_THROW(act(restored, authorize()), Error);
}

TEST_F(Live, ReusedBrokerKeyDoesNotConfirmAnOlderUnknownIntent)
{
    const auto key =
        "1:" + std::to_string(exchange.call<int (*)()>("asterion_fake_trader_next_session")) + ":1";
    std::string policy;
    {
        LiveSession initial(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest(2));
        policy = initial.snapshot().at("policy_revision");
    }
    const auto unknown = submit("z.unknown", "1");
    {
        SqliteJournal journal(directory.path, {"plugins", "ctp-flow", "owner"});
        journal.start();
        journal.append(
            {{"command", unknown},
                {"policy_revision", policy},
                {"broker_key", key},
                {"trading_day", "20260928"}},
            unknown.at("request_id").get<std::string>(),
            "z.unknown");
    }
    {
        LiveSession restored(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
        ASSERT_EQ(ready(restored).at("unconfirmed").size(), 1U);
        act(restored, authorize());
        act(restored, submit("a.replacement", "3"));
        const auto state = wait_for(restored, [](const Json& value) {
            return order_has_status(value, "a.replacement", "accepted");
        });
        ASSERT_TRUE(order_has_status(state, "a.replacement", "accepted"));
        EXPECT_EQ(find_order(state, "a.replacement").at("broker_key"), key);
        ASSERT_EQ(state.at("unconfirmed").size(), 1U);
        EXPECT_EQ(state.at("unconfirmed")[0].at("id"), "z.unknown");
        EXPECT_THROW(act(restored, submit("over.limit", "1")), std::invalid_argument);
    }
    // Lexical order is the opposite of durable order. Recovery must keep the
    // later broker attribution and leave the older intent unknown.
    {
        LiveSession again(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
        ASSERT_EQ(ready(again).at("phase"), "ready");
        const auto state = wait_for(again, [](const Json& value) {
            return order_has_status(value, "a.replacement", "accepted");
        });
        ASSERT_TRUE(order_has_status(state, "a.replacement", "accepted"));
        ASSERT_EQ(state.at("unconfirmed").size(), 1U);
        EXPECT_EQ(state.at("unconfirmed")[0].at("id"), "z.unknown");
        exchange.call<void (*)(int, int)>("asterion_fake_trader_fill", 0, 3);
        ASSERT_TRUE(order_has_status(
            wait_for(
                again,
                [](const Json& value) {
                    return order_has_status(value, "a.replacement", "filled");
                }),
            "a.replacement",
            "filled"));
    }
    // The later intent is now retired from memory. Restore its identity before
    // using a terminal report as evidence about the older unresolved intent.
    LiveSession closed(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
    ASSERT_EQ(ready(closed).at("phase"), "ready");
    const auto state = wait_for(closed, [](const Json& value) {
        return order_has_status(value, "a.replacement", "filled");
    });
    ASSERT_TRUE(order_has_status(state, "a.replacement", "filled"));
    ASSERT_EQ(state.at("unconfirmed").size(), 1U);
    EXPECT_EQ(state.at("unconfirmed")[0].at("id"), "z.unknown");
}

TEST_F(Live, RecordedOrdersTheBrokerDoesNotReportCountAsWorkingExposure)
{
    {
        LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
        ASSERT_EQ(ready(session).at("phase"), "ready");
        act(session, authorize());
        act(session, submit("lost", "3"));
    }
    // The exchange forgets the order, as if it never arrived.
    exchange.reset();
    LiveSession restored(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
    const auto state = ready(restored);
    ASSERT_EQ(state.at("unconfirmed").size(), 1U);
    EXPECT_EQ(state.at("unconfirmed")[0].at("id"), "lost");
    EXPECT_THROW(act(restored, policy_command(restored, manifest().at("policy"))), Error);
    EXPECT_TRUE(state.at("orders").empty()) << "the unconfirmed order is not resent";
    act(restored, authorize("authorize.restored"));
    EXPECT_THROW(act(restored, submit("next", "1")), std::invalid_argument)
        << "the unconfirmed order still occupies the working-order limit";
    const auto resolve = [](std::string id, std::string order) {
        return Json{{"request_id", std::move(id)}, {"action", "live_resolve"}, {"order_id", order}};
    };
    EXPECT_THROW(act(restored, resolve("resolve.unknown", "missing")), std::invalid_argument);
    // The owner verified at the broker that "lost" does not exist.
    act(restored, resolve("resolve.lost", "lost"));
    EXPECT_TRUE(restored.snapshot().at("unconfirmed").empty());
    act(restored, resolve("resolve.lost", "lost")); // retried request: acknowledged
    EXPECT_THROW(act(restored, resolve("resolve.again", "lost")), std::invalid_argument)
        << "a resolved order is no longer unconfirmed";
    act(restored, submit("next", "1"));
    const auto confirmed = wait_for(restored, [](const Json& state) {
        return find_order(state, "next").is_object();
    });
    EXPECT_TRUE(find_order(confirmed, "next").is_object());
}

TEST_F(Live, SdkRefusalRecordsTheOrderAsNotSentAndReleasesItsRisk)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    exchange.call<void (*)(int)>("asterion_fake_trader_cancel_code", -3);
    EXPECT_THROW(act(session, submit("refused", "3")), Error);
    exchange.call<void (*)(int)>("asterion_fake_trader_cancel_code", 0);
    const auto state = session.snapshot();
    EXPECT_TRUE(state.at("orders").empty()) << "an SDK return is not a broker order report";
    EXPECT_TRUE(state.at("unconfirmed").empty()) << "the SDK sent nothing it refused";
    const auto effects = exchange.call<int (*)()>("asterion_fake_catalog_side_effects");
    EXPECT_NO_THROW(act(session, submit("refused", "3")));
    EXPECT_EQ(exchange.call<int (*)()>("asterion_fake_catalog_side_effects"), effects)
        << "a retried request is acknowledged, not sent";
    act(session, submit("next", "1"));
    const auto confirmed = wait_for(session, [](const Json& state) {
        return find_order(state, "next").is_object();
    });
    EXPECT_TRUE(find_order(confirmed, "next").is_object());
}

TEST_F(Live, DurableNotSentEvidenceAcrossPagesReleasesIntentWithoutResendingOnRecovery)
{
    std::string revision;
    {
        LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
        revision = session.snapshot().at("policy_revision").get<std::string>();
    }
    // Recovery fixture: a committed intent followed by a definite pre-SDK refusal.
    const auto command = submit("not.sent", "3");
    {
        SqliteJournal journal(directory.path, {"ctp-flow", "plugins", "archives", "owner"});
        journal.start();
        // Put the intent at the end of one page and its result on the next page.
        for (std::size_t i = 1; i < SqliteJournal::page_size - 1; ++i) {
            const auto id = "revoke." + std::to_string(i);
            journal.append(
                {{"command", {{"request_id", id}, {"action", "live_revoke"}}},
                    {"policy_revision", revision}},
                id);
        }
        journal.append(
            {{"command", command},
                {"policy_revision", revision},
                {"broker_key", "1:99:1"},
                {"trading_day", "20260928"}},
            command.at("request_id").get<std::string>(),
            "not.sent");
        journal.append({{"order_not_sent", "not.sent"}, {"error_code", -1005}}, {}, {}, "not.sent");
    }
    LiveSession restored(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
    const auto state = ready(restored);
    EXPECT_TRUE(state.at("unconfirmed").empty());
    EXPECT_TRUE(state.at("orders").empty());
    act(restored, command);
    EXPECT_TRUE(restored.snapshot().at("orders").empty()) << "retry only acknowledges the intent";
    act(restored, authorize());
    auto reused = command;
    reused["request_id"] = "different.request";
    EXPECT_THROW(act(restored, reused), Error)
        << "a different request cannot reuse an old order ID";
    EXPECT_NO_THROW(act(restored, submit("next", "3"))) << "unsent intent occupies no working slot";
}

TEST_F(Live, ResolvedOrdersStayResolvedAcrossRecovery)
{
    {
        LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
        ASSERT_EQ(ready(session).at("phase"), "ready");
        act(session, authorize());
        act(session, submit("lost", "3"));
    }
    exchange.reset();
    {
        LiveSession restored(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
        ASSERT_EQ(ready(restored).at("unconfirmed").size(), 1U);
        act(restored,
            {{"request_id", "resolve"}, {"action", "live_resolve"}, {"order_id", "lost"}});
    }
    LiveSession again(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
    EXPECT_TRUE(ready(again).at("unconfirmed").empty());
}

TEST_F(Live, MissingOrderIdentityIndexRejectsRecoveryWithoutChangingEvidence)
{
    std::string revision;
    {
        LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
        revision = session.snapshot().at("policy_revision");
    }
    const auto command = submit("missing.index", "3");
    {
        SqliteJournal journal(directory.path, {"ctp-flow", "owner"});
        journal.start();
        journal.append(
            {{"command", command},
                {"policy_revision", revision},
                {"broker_key", "1:99:1"},
                {"trading_day", "20260928"}},
            command.at("request_id").get<std::string>());
    }
    const auto digest = sha256_file(directory.path / "journal.sqlite");
    EXPECT_THROW(
        (LiveSession{directory.path, ASTERION_TEST_CTP_TRADER, owners.path}),
        std::invalid_argument);
    EXPECT_EQ(sha256_file(directory.path / "journal.sqlite"), digest);
}

TEST_F(Live, CredentialsAreNeverWrittenAndHeadersIdentifyTheAccount)
{
    {
        LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
        ASSERT_EQ(ready(session).at("phase"), "ready");
        act(session, authorize());
    }
    for (const auto& entry : fs::recursive_directory_iterator(directory.path)) {
        if (!entry.is_regular_file())
            continue;
        std::ifstream in(entry.path(), std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)), {});
        EXPECT_EQ(bytes.find("secret"), std::string::npos) << entry.path();
        EXPECT_EQ(bytes.find("auth-code"), std::string::npos) << entry.path();
    }
    const auto header_file = test::journal_record(directory.path, 0);
    auto header = test::read_record(header_file);
    EXPECT_EQ(header.at("manifest"), manifest());
    header["account_id"] = "different-account";
    test::write_record(header_file, header);
    const auto before = sha256_file(directory.path / "journal.sqlite");
    EXPECT_THROW(
        (LiveSession{directory.path, ASTERION_TEST_CTP_TRADER, owners.path}),
        std::invalid_argument);
    EXPECT_EQ(sha256_file(directory.path / "journal.sqlite"), before);
}

TEST_F(Live, InvalidInputsAndMissingSdkWriteNothing)
{
    auto bad = manifest();
    bad["broker"]["front"] = "http://127.0.0.1:41205";
    EXPECT_THROW(
        (LiveSession{directory.path, ASTERION_TEST_CTP_TRADER, owners.path, bad}),
        std::invalid_argument);
    bad = manifest();
    bad["policy"]["contracts"].push_back(contract("rb2610"));
    EXPECT_THROW(
        (LiveSession{directory.path, ASTERION_TEST_CTP_TRADER, owners.path, bad}),
        std::invalid_argument);
    EXPECT_THROW(
        (LiveSession{directory.path, directory.path / "missing.dylib", owners.path, manifest()}),
        Error);
    EXPECT_EQ(test::journal_size(directory.path), 0U);
    EXPECT_FALSE(fs::exists(directory.path / "plugins"));
}

TEST_F(Live, WireObservationPreservesExactValuesAndDetachedState)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    const auto disconnected = session.wire_snapshot();
    EXPECT_EQ(disconnected.broker_state(), ::trading::BROKER_STATE_DISCONNECTED);
    EXPECT_FALSE(disconnected.positions_reconciled());
    EXPECT_FALSE(disconnected.has_funds());
    EXPECT_FALSE(disconnected.has_authorized_ms());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    session.query_costs();
    ASSERT_EQ(
        wait_for(
            session,
            [](const Json& state) {
                return !state.at("costs").empty() && state.at("costs")[0].at("state") == "ready";
            })
            .at("costs")[0]
            .at("state"),
        "ready");
    act(session, authorize());
    act(session, submit("observed", "2"));
    ASSERT_TRUE(order_has_status(
        wait_for(
            session,
            [](const Json& state) {
                return order_has_status(state, "observed", "filled");
            }),
        "observed",
        "filled"));
    const auto snapshot = session.wire_snapshot();
    EXPECT_EQ(snapshot.policy().limits().max_order_lots().text(), "5");
    EXPECT_EQ(snapshot.policy().max_price_deviation().text(), "0.02");
    ASSERT_EQ(snapshot.policy().contracts_size(), 1);
    EXPECT_EQ(snapshot.policy().contracts(0).multiplier().text(), "10");
    ASSERT_TRUE(snapshot.has_funds());
    EXPECT_EQ(snapshot.funds().commission().text(), "12.34");
    EXPECT_EQ(snapshot.working_orders_size(), 0);
    EXPECT_EQ(snapshot.order_count(), 1);
    const auto orders = session.orders();
    ASSERT_EQ(orders.orders_size(), 1);
    EXPECT_EQ(orders.orders(0).side(), ::common::SIDE_BUY);
    EXPECT_EQ(orders.orders(0).offset(), ::common::OFFSET_OPEN);
    EXPECT_EQ(orders.orders(0).price().text(), "3500");
    EXPECT_EQ(orders.orders(0).filled_lots().text(), "2");
    ASSERT_EQ(snapshot.costs_size(), 1);
    EXPECT_EQ(snapshot.costs(0).costs().close_today_fee().text(), "1.5");
    EXPECT_EQ(snapshot.costs(0).costs().open_fee_rate().text(), "0.0001");
    ASSERT_TRUE(snapshot.has_authorized_ms());
    act(session, {{"request_id", "revoke"}, {"action", "live_revoke"}});
    EXPECT_FALSE(session.wire_snapshot().has_authorized_ms());
    EXPECT_TRUE(snapshot.has_authorized_ms()) << "a published value cannot follow later mutations";
    session.disconnect();
    EXPECT_FALSE(session.wire_snapshot().positions_reconciled());
}

TEST(LiveProtocol, TypedCommandsKeepDurableContentAndPolicyValues)
{
    const Json command{
        {"request_id", "change"},
        {"action", "live_policy"},
        {"policy", manifest().at("policy")}
    };
    const auto wire = protocol::encode_command(command);
    const auto admitted = asterion::trading::AccountRequest::from_wire(wire);
    ASSERT_NE(admitted.as<asterion::trading::ChangePolicy>(), nullptr);
    EXPECT_EQ(
        admitted.as<asterion::trading::ChangePolicy>()->definition.limits.max_order_quantity,
        Decimal::parse("5"));
    EXPECT_EQ(
        admitted.as<asterion::trading::ChangePolicy>()->definition.max_price_deviation,
        Decimal::parse("0.02"));
    EXPECT_EQ(admitted.command, command);
    const auto restored = asterion::trading::AccountRequest::from_record(command);
    EXPECT_EQ(restored.command, admitted.command);
    EXPECT_EQ(restored.id, admitted.id);
    auto invalid = wire;
    invalid.mutable_change_policy()->clear_limits();
    EXPECT_THROW(asterion::trading::AccountRequest::from_wire(invalid), std::invalid_argument);
    invalid = wire;
    auto* policy = invalid.mutable_change_policy();
    *policy->add_contracts() = policy->contracts(0);
    EXPECT_THROW(asterion::trading::AccountRequest::from_wire(invalid), std::invalid_argument);
    auto order = protocol::encode_command(submit("exact", "2", "3500.25"));
    const auto typed = asterion::trading::AccountRequest::from_wire(order);
    EXPECT_EQ(
        typed.as<asterion::trading::SubmitOrder>()->order.limit_price,
        Decimal::parse("3500.25"));
    order.mutable_submit_order()->set_side(::common::SIDE_UNSPECIFIED);
    EXPECT_THROW(asterion::trading::AccountRequest::from_wire(order), std::invalid_argument);
}

TEST(LiveProtocol, CommandsRoundTripAndBrokerIdentityValidation)
{
    for (const auto& command :
        {authorize(),
            Json{{"request_id", "r"}, {"action", "live_revoke"}},
            Json{{"request_id", "s"}, {"action", "live_resolve"}, {"order_id", "o"}}})
        EXPECT_EQ(protocol::decode_command(protocol::encode_command(command)), command);
    auto spaced = manifest();
    spaced["broker"]["user_id"] = "000 001";
    EXPECT_THROW(protocol::live_input(spaced), std::invalid_argument);
}

TEST_F(Live, AuthorizationOutlivesAReconnectAndEndsOnlyWhenRevoked)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    ctp::SharedLibrary reconnect(ASTERION_TEST_CTP_TRADER, "asterion_fake_trader_reconnect");
    reconnect.symbol<void (*)()>()();
    // Nothing is sent while the new connection synchronizes; the permission stays.
    auto state = wait_for(session, [](const Json& s) {
        return s.at("phase") != "ready";
    });
    ASSERT_NE(state.at("phase"), "ready");
    EXPECT_TRUE(state.contains("authorization"));
    try {
        act(session, submit("unsynchronized", "1"));
        FAIL() << "an order was accepted before the new connection synchronized";
    } catch (const Error& error) {
        // Refused as unsynchronized, or as admitted under a control revision that
        // the reconnect advanced before the order ran. Neither sends anything.
        EXPECT_TRUE(error.code() == ErrorCode::unavailable || error.code() == ErrorCode::conflict)
            << error.what();
    }
    ASSERT_EQ(
        wait_for(
            session,
            [](const Json& s) {
                return s.at("phase") == "ready";
            })
            .at("phase"),
        "ready");
    EXPECT_NO_THROW(act(session, submit("same.authorization", "1")));
    act(session, Json{{"request_id", "revoke"}, {"action", "live_revoke"}});
    EXPECT_FALSE(session.snapshot().contains("authorization"));
    const auto effects = exchange.call<int (*)()>("asterion_fake_catalog_side_effects");
    try {
        act(session, submit("revoked", "1"));
        FAIL() << "an order was accepted after authorization was revoked";
    } catch (const std::invalid_argument& error) {
        EXPECT_STREQ(error.what(), "authorize live trading for this account first");
    } catch (const Error& error) {
        // The preceding fill can invalidate admission before the authorization check runs.
        EXPECT_EQ(error.code(), ErrorCode::conflict) << error.what();
    }
    EXPECT_EQ(exchange.call<int (*)()>("asterion_fake_catalog_side_effects"), effects);
}

TEST_F(Live, TradingDayRolloverRebuildsReportsAndRatesWithoutResending)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    act(session, submit("day0.filled", "1"));
    ASSERT_FALSE(wait_for(
        session,
        [](const Json& s) {
            return !s.at("positions").empty();
        })
            .at("positions")
            .empty());
    act(session, submit("day0.resting", "3"));
    session.query_costs();
    ASSERT_EQ(
        wait_for(
            session,
            [](const Json& s) {
                return !s.at("costs").empty() && s.at("costs")[0].at("state") == "ready";
            })
            .at("costs")[0]
            .at("state"),
        "ready");
    ctp::SharedLibrary rollover(ASTERION_TEST_CTP_TRADER, "asterion_fake_trader_next_day");
    int index = 0;
    for (const auto& [counter, day] :
        {std::pair{"20260929", "2026-09-29"}, std::pair{"20260930", "2026-09-30"}}) {
        SCOPED_TRACE(day);
        rollover.symbol<void (*)(const char*)>()(counter);
        const auto state = wait_for(session, [&](const Json& s) {
            return s.at("phase") == "ready" && s.at("trading_day") == day
                && s.at("unconfirmed").size() == (index == 0 ? 1U : 0U);
        });
        ASSERT_EQ(state.at("trading_day"), day);
        ASSERT_EQ(state.at("phase"), "ready");
        EXPECT_TRUE(state.contains("authorization"))
            << "a new trading day asks for no new authorization";
        EXPECT_TRUE(state.at("orders").empty());
        EXPECT_TRUE(state.at("trades").empty());
        EXPECT_TRUE(state.at("costs").empty());
        if (index == 0) {
            EXPECT_EQ(state.at("unconfirmed")[0].at("id"), "day0.resting");
            EXPECT_EQ(state.at("unconfirmed")[0].at("trading_day"), "2026-09-28");
        }
        ASSERT_EQ(state.at("positions").size(), 1U);
        EXPECT_EQ(decimal(state.at("positions")[0].at("today")), "0");
        EXPECT_EQ(decimal(state.at("positions")[0].at("yesterday")), std::to_string(index + 1));
        act(session, submit("day0.filled", "1")); // historical request: acknowledge only
        EXPECT_TRUE(session.snapshot().at("orders").empty());
        if (++index == 1) {
            EXPECT_THROW(act(session, submit("still.unknown", "1")), std::invalid_argument);
            act(session,
                {{"request_id", "resolve.yesterday"},
                    {"action", "live_resolve"},
                    {"order_id", "day0.resting"}});
        }
        const auto id = "day" + std::to_string(index) + ".filled";
        act(session, submit(id, "1"));
        const auto filled = wait_for(session, [&](const Json& s) {
            return !s.at("trades").empty() && s.at("trades").back().at("order_id") == id;
        });
        ASSERT_EQ(filled.at("trades").size(), 1U);
        EXPECT_EQ(filled.at("trades")[0].at("order_id"), id);
        EXPECT_EQ(filled.at("trades")[0].at("trading_day"), day);
    }
}

TEST_F(Live, QuotesAndReadsPreserveThePreviousCloseWhileJournalWritesArePending)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    ASSERT_EQ(session.day("").close().trading_day(), "2026-09-28");
    const auto state = session.wire_snapshot();
    std::future<void> pending;
    HeldCommit held;
    pending = session.execute("fixture-account", state.policy_revision(), authorize());
    ASSERT_TRUE(held.waiting());
    exchange.call<void (*)(const char*)>("asterion_fake_trader_next_day", "20260929");
    wait_for(session, [&](const Json&) {
        const auto next = session.wire_snapshot();
        return next.trading_day() == "2026-09-29" && next.has_funds()
            && next.positions_reconciled();
    });
    MarketQuote quote;
    quote.instrument = {"SHFE", "rb2610"};
    quote.trading_day = "20260929";
    quote.last = Decimal::parse("3500");
    quote.source_ms = quote.received_ms = utc_now_ms(*clock);
    session.advance_quotes({quote});
    ASSERT_EQ(session.day("").close().trading_day(), "2026-09-29");
    held.release();
    EXPECT_THROW(pending.get(), std::exception);
    wait_for(session, [&](const Json&) {
        return session.day("2026-09-28").has_close();
    });
    EXPECT_TRUE(session.day("2026-09-28").has_close());
}

TEST_F(Live, TerminalEvidenceSurvivesSegmentsAndRestoresCurrentDayAttribution)
{
    test::SmallJournalSegments budget(1);
    {
        LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
        ASSERT_EQ(ready(session).at("phase"), "ready");
        act(session, authorize());
        act(session, submit("finished", "1"));
        ASSERT_TRUE(order_has_status(
            wait_for(
                session,
                [](const Json& state) {
                    return order_has_status(state, "finished", "filled");
                }),
            "finished",
            "filled"));
    }
    {
        LiveSession restored(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
        ASSERT_EQ(ready(restored).at("phase"), "ready");
        const auto state = wait_for(restored, [](const Json& value) {
            return order_has_status(value, "finished", "filled");
        });
        ASSERT_TRUE(order_has_status(state, "finished", "filled"));
        EXPECT_TRUE(state.at("unconfirmed").empty());
        ASSERT_FALSE(state.at("trades").empty());
        EXPECT_EQ(state.at("trades")[0].at("order_id"), "finished");
        const auto effects = exchange.call<int (*)()>("asterion_fake_catalog_side_effects");
        act(restored, submit("finished", "1"));
        EXPECT_EQ(exchange.call<int (*)()>("asterion_fake_catalog_side_effects"), effects);
    }
    exchange.call<void (*)(const char*)>("asterion_fake_trader_next_day", "20260929");
    LiveSession next_day(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
    const auto state = ready(next_day);
    EXPECT_TRUE(state.at("unconfirmed").empty());
    EXPECT_TRUE(state.at("orders").empty());
    EXPECT_GE(number(state.at("segment_count")), 2);
}

TEST_F(Live, AnotherFrontOfTheSameAccountContinuesItsRecordWithoutAuthorization)
{
    {
        LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
        ASSERT_EQ(ready(session).at("phase"), "ready");
        act(session, authorize());
        act(session, submit("before", "1"));
        wait_for(session, [](const Json& state) {
            return order_has_status(state, "before", "filled");
        });
    }
    LiveSession moved(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
    moved.connect("secret", "auth-code", "tcp://127.0.0.1:41299");
    const auto state = wait_for(moved, [](const Json& s) {
        return s.at("phase") == "ready" && order_has_status(s, "before", "filled");
    });
    EXPECT_STREQ(
        exchange.call<const char* (*)()>("asterion_fake_trader_front"),
        "tcp://127.0.0.1:41299");
    EXPECT_FALSE(state.contains("authorization"));
}

TEST_F(Live, FillsAreRecordedOnceAndRemainReadableAfterRestartAndRollover)
{
    const auto recorded = [](const LiveSession& session, const std::string& day) {
        for (int attempt = 0; attempt < 200; ++attempt) {
            auto trades = session.day(day);
            if (trades.trades_size())
                return trades;
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        return session.day(day);
    };
    {
        LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
        ASSERT_EQ(ready(session).at("phase"), "ready");
        act(session, authorize());
        act(session, submit("filled", "1"));
        const auto trades = recorded(session, "2026-09-28");
        ASSERT_EQ(trades.trades_size(), 1);
        EXPECT_EQ(trades.trades(0).order_id(), "filled");
        EXPECT_EQ(trades.trades(0).lots().text(), "1");
        EXPECT_FALSE(trades.trades(0).price().text().empty());
        EXPECT_EQ(trades.trades(0).trade_time(), "20260928 09:30:00");
        EXPECT_GT(trades.trades(0).received_ms(), 0);
        // The counter reports the same fill again after a reconnect.
        exchange.call<void (*)()>("asterion_fake_trader_reconnect");
        ready(session);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        EXPECT_EQ(session.day("2026-09-28").trades_size(), 1);
        // The broker rebuilt its fill list; a fill after it is still recorded.
        act(session, authorize("authorize.after.reconnect"));
        act(session, submit("after", "1"));
        for (int attempt = 0; attempt < 200 && session.day("2026-09-28").trades_size() < 2;
            ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        EXPECT_EQ(session.day("2026-09-28").trades_size(), 2);
        wait_for(session, [](const Json& s) {
            return s.at("positions_reconciled") == true && s.contains("funds")
                && s.at("positions").size() == 1 && s.at("positions")[0].at("today") == "2";
        });
        // The next trading day begins while Trading runs: the day's close is recorded.
        EXPECT_FALSE(session.day("2026-09-28").has_close());
        const auto first = session.day("2026-09-28", 0, 1, true);
        ASSERT_EQ(first.trades_size(), 1);
        EXPECT_EQ(first.trades(0).order_id(), "filled");
        EXPECT_EQ(first.total(), 2);
        EXPECT_EQ(first.close().trading_day(), "2026-09-28");
        exchange.call<void (*)(const char*)>("asterion_fake_trader_next_day", "20260929");
        for (int attempt = 0; attempt < 400 && !session.day("2026-09-28").has_close(); ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    LiveSession next_day(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
    EXPECT_TRUE(ready(next_day).at("trades").empty());
    const auto day = next_day.day("2026-09-28");
    const auto second = next_day.day("2026-09-28", 1, 1, true);
    ASSERT_EQ(second.trades_size(), 1);
    EXPECT_EQ(second.trades(0).order_id(), "after");
    ASSERT_EQ(day.trades_size(), 2);
    EXPECT_EQ(day.trades(1).order_id(), "filled");
    EXPECT_EQ(day.trading_days_size(), 1);
    EXPECT_EQ(day.trading_days(0), "2026-09-28");
    ASSERT_TRUE(day.has_close());
    EXPECT_FALSE(day.close().funds().balance().text().empty());
    ASSERT_EQ(day.close().positions_size(), 1);
    EXPECT_EQ(day.close().positions(0).side(), ::common::SIDE_BUY);
    EXPECT_EQ(day.close().positions(0).today_lots().text(), "2");
}

TEST_F(Live, ExternalCancellationCountRemainsStableWhilePositionsReconcile)
{
    {
        Directory seed_directory, seed_owners;
        LiveSession seed(
            seed_directory.path,
            ASTERION_TEST_CTP_TRADER,
            seed_owners.path,
            manifest());
        ASSERT_EQ(ready(seed).at("phase"), "ready");
        act(seed, authorize());
        act(seed, submit("outside", "3"));
        ASSERT_TRUE(order_has_status(
            wait_for(
                seed,
                [](const Json& state) {
                    return order_has_status(state, "outside", "accepted");
                }),
            "outside",
            "accepted"));
        act(seed,
            {{"request_id", "cancel.outside"}, {"action", "cancel"}, {"order_id", "outside"}});
        ASSERT_TRUE(order_has_status(
            wait_for(
                seed,
                [](const Json& state) {
                    return order_has_status(state, "outside", "cancelled");
                }),
            "outside",
            "cancelled"));
    }
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    const auto orders = session.orders();
    ASSERT_EQ(orders.orders_size(), 1);
    ASSERT_TRUE(orders.orders(0).id().empty());
    act(session, authorize());
    exchange.call<void (*)(int)>("asterion_fake_trader_hold_positions", 1);
    act(session, submit("new.fill", "1"));
    wait_for(session, [&](const Json& state) {
        return state.at("positions_reconciled") == false
            && session.day("2026-09-28").trades_size() == 1;
    });
    const auto reconciled = session.wire_snapshot().positions_reconciled();
    const auto today = session.day("");
    const auto exported = session.day("2026-09-28", 0, 200, true);
    exchange.call<void (*)(int)>("asterion_fake_trader_hold_positions", 0);
    EXPECT_FALSE(reconciled);
    for (const auto* day : {&today, &exported}) {
        ASSERT_EQ(day->close().cancels_size(), 1);
        EXPECT_EQ(day->close().cancels(0).cancels(), 1U);
    }
}

TEST_F(Live, SameDayCloseAndCrossDayOfflineRestartPreserveCloseAndCancels)
{
    {
        LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
        ASSERT_EQ(ready(session).at("phase"), "ready");
        act(session, authorize());
        act(session, submit("resting", "3"));
        wait_for(session, [](const Json& s) {
            return order_has_status(s, "resting", "accepted");
        });
        exchange.call<void (*)(int)>("asterion_fake_trader_cancel_code", -2);
        EXPECT_THROW(
            act(session,
                {{"request_id", "cancel.rejected"}, {"action", "cancel"}, {"order_id", "resting"}}),
            Error);
        exchange.call<void (*)(int)>("asterion_fake_trader_cancel_code", 0);
        act(session,
            {{"request_id", "cancel.accepted"}, {"action", "cancel"}, {"order_id", "resting"}});
        wait_for(session, [](const Json& s) {
            return order_has_status(s, "resting", "cancelled");
        });
        // Both requests count toward the daily cancel limit; one was refused.
        const auto cancels = session.wire_snapshot().cancels();
        ASSERT_EQ(cancels.size(), 1);
        EXPECT_EQ(cancels[0].cancels(), 2U);
        EXPECT_EQ(cancels[0].rejected_cancels(), 1U);
        act(session, submit("filled", "1"));
        wait_for(session, [](const Json& s) {
            return s.at("positions_reconciled") == true && s.contains("funds")
                && s.at("positions").size() == 1 && s.at("positions")[0].at("today") == "1";
        });
        const auto today = session.day("");
        ASSERT_TRUE(today.has_close());
        EXPECT_EQ(today.close().trading_day(), "2026-09-28");
        EXPECT_FALSE(today.close().funds().balance().text().empty());
        ASSERT_EQ(today.close().positions_size(), 1);
        ASSERT_EQ(today.close().cancels_size(), 1);
        EXPECT_EQ(today.close().cancels(0).instrument().symbol(), "rb2610");
        EXPECT_EQ(today.close().cancels(0).cancels(), 2U);
        EXPECT_EQ(today.close().cancels(0).rejected_cancels(), 1U);
    }
    // Trading was stopped before rollover; next trading day starts when it restarts.
    exchange.call<void (*)(const char*)>("asterion_fake_trader_next_day", "20260929");
    LiveSession restarted(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
    ASSERT_EQ(ready(restarted).at("phase"), "ready");
    for (int attempt = 0; attempt < 400 && !restarted.day("2026-09-28").has_close(); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    const auto closed = restarted.day("2026-09-28");
    ASSERT_TRUE(closed.has_close());
    EXPECT_FALSE(closed.close().funds().balance().text().empty());
    ASSERT_EQ(closed.close().positions_size(), 1);
    EXPECT_EQ(closed.close().positions(0).today_lots().text(), "1");
    ASSERT_EQ(closed.close().cancels_size(), 1);
    EXPECT_EQ(closed.close().cancels(0).cancels(), 2U);
    EXPECT_EQ(closed.close().cancels(0).rejected_cancels(), 1U);
}

TEST_F(Live, FailedTerminalCommitRetainsUnknownRiskAcrossRolloverAndRecovery)
{
    {
        LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
        ASSERT_EQ(ready(session).at("phase"), "ready");
        act(session, authorize());
        act(session, submit("unsettled", "3"));
        ASSERT_TRUE(order_has_status(
            wait_for(
                session,
                [](const Json& state) {
                    return order_has_status(state, "unsettled", "accepted");
                }),
            "unsettled",
            "accepted"));
        sqlite::fail_next_commits_for_testing(1);
        exchange.call<void (*)(int, int)>("asterion_fake_trader_fill", 0, 3);
        const auto failed = wait_for(session, [](const Json& state) {
            return state.at("recovery_required") == true;
        });
        ASSERT_EQ(failed.at("recovery_required"), true);
    }
    exchange.call<void (*)(const char*)>("asterion_fake_trader_next_day", "20260929");
    LiveSession restored(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
    const auto state = ready(restored);
    ASSERT_EQ(state.at("unconfirmed").size(), 1U);
    EXPECT_EQ(state.at("unconfirmed")[0].at("id"), "unsettled");
    EXPECT_EQ(state.at("unconfirmed")[0].at("trading_day"), "2026-09-28");
    act(restored, authorize("after.failure"));
    EXPECT_THROW(act(restored, submit("blocked", "1")), std::invalid_argument);
}

TEST_F(Live, SameDayReconnectDoesNotMistakeCachedOrdersForBrokerConfirmation)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    act(session, submit("missing", "3"));
    ASSERT_TRUE(wait_for(
        session,
        [](const Json& s) {
            return !s.at("orders").empty();
        })
            .at("orders")
            .size());
    exchange.reset(); // broker no longer reports the order
    ctp::SharedLibrary reconnect(ASTERION_TEST_CTP_TRADER, "asterion_fake_trader_reconnect");
    reconnect.symbol<void (*)()>()();
    const auto state = wait_for(session, [](const Json& s) {
        return s.at("phase") == "ready" && !s.contains("authorization");
    });
    ASSERT_EQ(state.at("phase"), "ready");
    EXPECT_TRUE(state.at("orders").empty());
    ASSERT_EQ(state.at("unconfirmed").size(), 1U);
    EXPECT_EQ(state.at("unconfirmed")[0].at("id"), "missing");
    act(session, authorize("again"));
    EXPECT_THROW(act(session, submit("next", "1")), std::invalid_argument);
}

TEST_F(Live, ForeignJournalIsRejectedWithoutAddingTablesOrChangingBytes)
{
    {
        sqlite::Database database(directory.path / "journal.sqlite");
        database.execute("CREATE TABLE foreign_data(value TEXT)");
        database.execute("INSERT INTO foreign_data VALUES('preserve')");
    }
    const auto before = sha256_file(directory.path / "journal.sqlite");
    SqliteJournal journal(directory.path);
    EXPECT_THROW(journal.start(), std::invalid_argument);
    EXPECT_EQ(sha256_file(directory.path / "journal.sqlite"), before);
    sqlite::Database inspect(
        directory.path / "journal.sqlite",
        sqlite::Database::Access::read_only);
    sqlite::Database::Statement tables(
        inspect,
        "SELECT COUNT(*) FROM sqlite_schema WHERE type='table'");
    ASSERT_TRUE(tables.step());
    EXPECT_EQ(tables.integer(0), 1);
}

TEST_F(Live, AutomaticSegmentsPreserveAccountAuthorizationAndDeduplicationAcrossRecovery)
{
    test::SmallJournalSegments budget(1);
    std::string account, policy;
    {
        LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
        ASSERT_EQ(ready(session).at("phase"), "ready");
        act(session, authorize());
        const auto before = session.snapshot();
        account = before.at("account_id");
        policy = before.at("policy_revision");
        act(session, submit("resting", "3"));
        const auto after = session.snapshot();
        EXPECT_EQ(after.at("account_id"), account);
        EXPECT_EQ(after.at("policy_revision"), policy);
        EXPECT_EQ(after.at("authorization"), before.at("authorization"));
        EXPECT_EQ(after.at("phase"), "ready");
        EXPECT_EQ(number(after.at("segment_count")), 1);
        const auto effects = exchange.call<int (*)()>("asterion_fake_catalog_side_effects");
        EXPECT_NO_THROW(act(session, submit("resting", "3")));
        EXPECT_THROW(act(session, submit("resting", "2")), Error);
        EXPECT_EQ(exchange.call<int (*)()>("asterion_fake_catalog_side_effects"), effects);
    }
    LiveSession restored(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
    const auto state = ready(restored);
    EXPECT_EQ(state.at("account_id"), account);
    EXPECT_EQ(state.at("policy_revision"), policy);
    EXPECT_FALSE(state.contains("authorization"));
    const auto effects = exchange.call<int (*)()>("asterion_fake_catalog_side_effects");
    EXPECT_NO_THROW(act(restored, submit("resting", "3")));
    EXPECT_THROW(act(restored, submit("resting", "2")), Error);
    EXPECT_EQ(exchange.call<int (*)()>("asterion_fake_catalog_side_effects"), effects);
    act(restored, {{"request_id", "cancel"}, {"action", "cancel"}, {"order_id", "resting"}});
    EXPECT_GT(number(restored.snapshot().at("segment_count")), 1);
}

TEST_F(Live, IdleAccountPublishesItsOwnProgressWithoutClientOrBrokerEvents)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    std::this_thread::sleep_for(1250ms);
    const auto idle = session.health();
    EXPECT_TRUE(idle.state.observed);
    EXPECT_LT(idle.state.age_ms, 700U);
    EXPECT_TRUE(idle.persistence.observed);
    EXPECT_FALSE(idle.persistence.pending);
    EXPECT_FALSE(idle.business_ready);
    ASSERT_EQ(ready(session).at("phase"), "ready");
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (!session.health().business_ready && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
    EXPECT_TRUE(session.health().business_ready);
}

TEST_F(Live, ShutdownFencesPendingSendsDrainsRepliesAndWaitsForSdkRelease)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    const auto state = session.snapshot();
    HeldCommit commit;
    auto placed = session.execute(
        state.at("account_id").get<std::string>(),
        state.at("policy_revision").get<std::string>(),
        submit("stopping", "1"));
    ASSERT_TRUE(commit.waiting());
    session.close_admission();
    EXPECT_THROW(session.connect("test-only", "test-only"), Error);
    EXPECT_TRUE(session.snapshot().at("orders").empty()); // Accepted replies can still read.
    commit.release();
    EXPECT_THROW(placed.get(), Error);
    EXPECT_TRUE(session.snapshot().at("unconfirmed").empty());

    struct Release
    {
        ~Release()
        {
            exchange.call<void (*)(int)>("asterion_fake_trader_hold_release", 0);
        }

        FakeExchange& exchange;
    } release{exchange};

    exchange.call<void (*)(int)>("asterion_fake_trader_hold_release", 1);
    session.stop();
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!exchange.call<int (*)()>("asterion_fake_trader_sdk_releases")
        && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
    ASSERT_EQ(exchange.call<int (*)()>("asterion_fake_trader_sdk_releases"), 1);
    EXPECT_EQ(session.stopped().wait_for(100ms), std::future_status::timeout);
    EXPECT_FALSE(session.health().business_ready);
    EXPECT_THROW(session.snapshot(), Error);
    exchange.call<void (*)(int)>("asterion_fake_trader_hold_release", 0);
    ASSERT_EQ(session.stopped().wait_for(2s), std::future_status::ready);
    EXPECT_NO_THROW(session.stopped().get());
    EXPECT_EQ(exchange.call<int (*)()>("asterion_fake_trader_sdk_thread_mismatches"), 0);
}

TEST_F(Live, StatusExcludesHistoryAndHistoryPagesStayComplete)
{
    exchange.call<void (*)(int)>("asterion_fake_trader_seed_history", 125);
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    const auto snapshot = session.wire_snapshot();
    EXPECT_EQ(snapshot.order_count(), 125);
    EXPECT_EQ(snapshot.trade_count(), 125);
    EXPECT_TRUE(snapshot.working_orders().empty());
    std::set<std::string> orders, trades;
    for (std::uint32_t offset = 0; offset < 150; offset += 50) {
        const auto page = session.orders(offset, 50);
        const auto fills = session.day("", offset, 50);
        EXPECT_EQ(page.total(), 125);
        EXPECT_EQ(fills.total(), 125);
        EXPECT_EQ(page.orders_size(), offset == 100 ? 25 : 50);
        EXPECT_EQ(fills.trades_size(), page.orders_size());
        for (const auto& order : page.orders())
            EXPECT_TRUE(orders.insert(order.broker_key()).second);
        for (const auto& trade : fills.trades())
            EXPECT_TRUE(trades.insert(trade.id()).second);
    }
    EXPECT_EQ(orders.size(), 125);
    EXPECT_EQ(trades.size(), 125);
    EXPECT_TRUE(session.orders(125, 50).orders().empty());
    EXPECT_TRUE(session.day("", 125, 50).trades().empty());
}

// Explicit measurement, excluded from ordinary regression runs. Uses only SDK
// fixtures; no real broker, trading authorization or orders are submitted.
TEST_F(Live, DISABLED_ObservationScaleMeasurements)
{
    Json measurements = Json::array();
    for (const auto [accounts, rows] : {std::pair{1, 0}, {1, 250}, {1, 1000}, {4, 1000}}) {
        exchange.reset();
        exchange.call<void (*)(int)>("asterion_fake_trader_seed_history", rows);
        std::vector<std::unique_ptr<asterion::trading::LiveSession>> sessions;
        for (int i = 0; i < accounts; ++i) {
            const auto id =
                std::to_string(accounts) + "." + std::to_string(rows) + "." + std::to_string(i);
            auto config = manifest();
            config["account_id"] = "measure." + id;
            config["broker"]["user_id"] = "u" + id;
            fs::create_directory(directory.path / id);
            auto session = std::make_unique<asterion::trading::LiveSession>(
                directory.path / id,
                ASTERION_TEST_CTP_TRADER,
                owners.path,
                config);
            session->initialized().get();
            session->connect({"tcp://127.0.0.1:41205", "client_app", "test-only", "test-only"})
                .get();
            const auto deadline = std::chrono::steady_clock::now() + 20s;
            auto snapshot = session->snapshot().get();
            while (snapshot.broker_state() != ::trading::BROKER_STATE_READY
                && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(20ms);
                snapshot = session->snapshot().get();
            }
            ASSERT_EQ(snapshot.broker_state(), ::trading::BROKER_STATE_READY);
            ASSERT_EQ(snapshot.order_count(), rows);
            ASSERT_EQ(snapshot.trade_count(), rows);
            sessions.push_back(std::move(session));
        }
        std::vector<double> observation_ms, encoding_ms, presentation_ms;
        std::size_t wire_bytes = 0, json_bytes = 0;
        const auto elapsed = [](auto begin) {
            return std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - begin)
                .count();
        };
        for (int sample = 0; sample < 35; ++sample) {
            const auto begin = std::chrono::steady_clock::now();
            std::vector<std::future<::trading::Session>> pending;
            for (const auto& session : sessions)
                pending.push_back(session->snapshot());
            std::vector<::trading::Session> snapshots;
            for (auto& future : pending)
                snapshots.push_back(future.get());
            const auto observation = elapsed(begin);
            const auto encode_begin = std::chrono::steady_clock::now();
            wire_bytes = 0;
            for (const auto& snapshot : snapshots)
                wire_bytes += snapshot.SerializeAsString().size();
            const auto encoding = elapsed(encode_begin);
            const auto present_begin = std::chrono::steady_clock::now();
            json_bytes = 0;
            for (const auto& snapshot : snapshots) {
                // What the Terminal relay sends to the page.
                std::string text;
                ASSERT_TRUE(google::protobuf::util::MessageToJsonString(snapshot, &text).ok());
                json_bytes += text.size();
            }
            const auto presentation = elapsed(present_begin);
            if (sample >= 5) {
                observation_ms.push_back(observation);
                encoding_ms.push_back(encoding);
                presentation_ms.push_back(presentation);
            }
        }
        const auto quantiles = [](std::vector<double> values) {
            std::sort(values.begin(), values.end());
            return Json{
                {"p50_ms", values[values.size() / 2]},
                {"p95_ms", values[values.size() * 95 / 100]}
            };
        };
        rusage usage{};
        ASSERT_EQ(getrusage(RUSAGE_SELF, &usage), 0);
#ifdef __APPLE__
        const auto peak_rss_bytes = usage.ru_maxrss;
#else
        const auto peak_rss_bytes = usage.ru_maxrss * 1024L;
#endif
        measurements.push_back(
            {{"accounts", accounts},
                {"orders_per_account", rows},
                {"trades_per_account", rows},
                {"observation", quantiles(observation_ms)},
                {"protobuf_encoding", quantiles(encoding_ms)},
                {"relay_json", quantiles(presentation_ms)},
                {"wire_bytes_total", wire_bytes},
                {"json_bytes_total", json_bytes},
                {"process_peak_rss_bytes", peak_rss_bytes}});
    }
    RecordProperty("measurements", measurements.dump());
}

TEST_F(Live, MarginCapReservesWorkingAndUnconfirmedOpenOrders)
{
    auto definition = manifest(4);
    definition["policy"]["max_margin_ratio"] = "0.02";
    const auto balance = [&] {
        exchange.call<void (*)(double, double, double, double, double)>(
            "asterion_fake_trader_funds",
            1000000.0,
            1000000.0,
            0.0,
            0.0,
            0.0);
    };
    balance();
    {
        LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, definition);
        ready(session);
        act(session, authorize());
        // Each lot needs 4,200 yuan. The first order must use the counter's rate.
        EXPECT_THROW(act(session, submit("too.large", "5")), std::invalid_argument);
        exchange.call<void (*)(double)>("asterion_fake_trader_frozen_margin", 6000.0);
        exchange.call<void (*)(double, double, double, double, double)>(
            "asterion_fake_trader_funds",
            1000000.0,
            1000001.0,
            1.0,
            0.0,
            0.0);
        wait_for(session, [](const Json& s) {
            return s.contains("funds") && decimal(s.at("funds").at("balance")) == "1000001";
        });
        EXPECT_THROW(act(session, submit("broker.frozen", "4")), std::invalid_argument);
        act(session, submit("resting", "3"));
        // The 6,000 yuan frozen before submission cannot cover this new 12,600 yuan order.
        EXPECT_THROW(act(session, submit("newer.than.freeze", "1")), std::invalid_argument);
        EXPECT_THROW(act(session, submit("over.cap", "2")), std::invalid_argument);
    }
    exchange.reset();
    balance();
    LiveSession restored(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
    ASSERT_EQ(ready(restored).at("unconfirmed").size(), 1U);
    act(restored, authorize("auth.restored"));
    EXPECT_THROW(act(restored, submit("over.cap.restored", "2")), std::invalid_argument);
    EXPECT_NO_THROW(act(restored, submit("headroom", "1")));
    act(restored,
        {{"request_id", "resolve.resting"}, {"action", "live_resolve"}, {"order_id", "resting"}});
    ready(restored);
    act(restored, submit("partial", "3"));
    exchange.call<void (*)(double)>("asterion_fake_trader_frozen_margin", 8400.0);
    exchange.call<void (*)(double)>("asterion_fake_trader_balance", 1000001.0);
    exchange.call<void (*)(int, int)>("asterion_fake_trader_fill", 1, 1);
    wait_for(restored, [](const Json& s) {
        return s.at("trades").size() == 2 && s.at("positions_reconciled").get<bool>()
            && s.contains("funds") && decimal(s.at("funds").at("balance")) == "1000001";
    });
    EXPECT_THROW(act(restored, submit("still.frozen", "4")), std::invalid_argument);
    exchange.call<void (*)(double)>("asterion_fake_trader_frozen_margin", 0.0);
    exchange.call<void (*)(double)>("asterion_fake_trader_balance", 1000002.0);
    act(restored,
        {{"request_id", "cancel.partial"}, {"action", "cancel"}, {"order_id", "partial"}});
    wait_for(restored, [](const Json& s) {
        return s.contains("funds") && decimal(s.at("funds").at("balance")) == "1000002";
    });
    EXPECT_NO_THROW(act(restored, submit("released", "4")));
}

TEST_F(Live, SelfTradePreventionBlocksCrossingActiveAndUnconfirmedOrders)
{
    {
        LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest(4));
        ASSERT_EQ(ready(session).at("phase"), "ready");
        act(session, authorize());

        // Hold 1 long lot so a closing sell can also be tested against a resting buy.
        act(session, submit("seed-long", "1", "3500"));
        wait_for(session, [](const Json& s) {
            return order_has_status(s, "seed-long", "filled")
                && s.at("positions_reconciled") == true;
        });

        // Resting buy at 3500 (3 lots rests on the fake exchange).
        act(session, submit("resting-buy", "3", "3500"));
        wait_for(session, [](const Json& s) {
            return order_has_status(s, "resting-buy", "accepted");
        });

        auto sell_touch = submit("sell-touch", "1", "3500");
        sell_touch["side"] = "sell";
        EXPECT_THROW(act(session, sell_touch), std::invalid_argument);

        auto sell_cross = submit("sell-cross", "1", "3499");
        sell_cross["side"] = "sell";
        EXPECT_THROW(act(session, sell_cross), std::invalid_argument);

        auto sell_close = submit("sell-close-cross", "1", "3500");
        sell_close["side"] = "sell";
        sell_close["offset"] = "close_today";
        EXPECT_THROW(act(session, sell_close), std::invalid_argument);

        // Non-crossing passive sell above 3500 is allowed.
        auto sell_above = submit("resting-sell", "3", "3501");
        sell_above["side"] = "sell";
        EXPECT_NO_THROW(act(session, sell_above));
        wait_for(session, [](const Json& s) {
            return order_has_status(s, "resting-sell", "accepted");
        });

        // A buy crossing the resting sell at 3501 is blocked; one below it is allowed.
        EXPECT_THROW(act(session, submit("buy-cross", "1", "3501")), std::invalid_argument);

        // Cancelling the resting buy lifts the block on selling at 3500.
        act(session,
            {{"request_id", "cancel.buy"}, {"action", "cancel"}, {"order_id", "resting-buy"}});
        wait_for(session, [](const Json& s) {
            return order_has_status(s, "resting-buy", "cancelled");
        });
        auto sell_after_cancel = submit("sell-after-cancel", "1", "3500");
        sell_after_cancel["side"] = "sell";
        EXPECT_NO_THROW(act(session, sell_after_cancel));
        wait_for(session, [](const Json& s) {
            return order_has_status(s, "sell-after-cancel", "filled")
                && s.at("positions_reconciled") == true;
        });

        act(session,
            {{"request_id", "cancel.sell"}, {"action", "cancel"}, {"order_id", "resting-sell"}});
        wait_for(session, [](const Json& s) {
            return order_has_status(s, "resting-sell", "cancelled");
        });

        // Leave a resting buy at 3500 that becomes unconfirmed after exchange reset.
        act(session, submit("unconfirmed-buy", "3", "3500"));
        wait_for(session, [](const Json& s) {
            return order_has_status(s, "unconfirmed-buy", "accepted");
        });
    }

    exchange.reset();
    LiveSession restored(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
    ASSERT_EQ(ready(restored).at("unconfirmed").size(), 1U);
    act(restored, authorize("auth.restored"));

    auto cross_unconfirmed = submit("sell-cross-unconfirmed", "1", "3500");
    cross_unconfirmed["side"] = "sell";
    EXPECT_THROW(act(restored, cross_unconfirmed), std::invalid_argument);

    auto clear_unconfirmed = submit("sell-above-unconfirmed", "1", "3501");
    clear_unconfirmed["side"] = "sell";
    EXPECT_NO_THROW(act(restored, clear_unconfirmed));
}

TEST_F(Live, DeliveryMonthBlocksPhysicalOpensWhileAllowingClosesAndCashIndexFutures)
{
    auto m = manifest(4);
    m["policy"]["contracts"].push_back(
        {{"venue", "CZCE"},
            {"symbol", "TA"},
            {"currency", "CNY"},
            {"price_increment", "1"},
            {"multiplier", "5"},
            {"quantity_increment", "1"},
            {"product", "TA"},
            {"delivery_month", ""}});
    m["policy"]["contracts"].push_back(
        {{"venue", "CFFEX"},
            {"symbol", "T2610"},
            {"currency", "CNY"},
            {"price_increment", "1"},
            {"multiplier", "10000"},
            {"quantity_increment", "1"},
            {"product", "T"},
            {"delivery_month", "2026-10"}});
    m["policy"]["contracts"].push_back(
        {{"venue", "CFFEX"},
            {"symbol", "IF2610"},
            {"currency", "CNY"},
            {"price_increment", "1"},
            {"multiplier", "300"},
            {"quantity_increment", "1"},
            {"product", "IF"},
            {"delivery_month", "2026-10"}});

    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, m);
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());

    // On 2026-09-28 (before delivery month 2026-10), opening SHFE/rb2610 succeeds.
    act(session, submit("pre-delivery-open", "2", "3500"));
    wait_for(session, [](const Json& s) {
        return order_has_status(s, "pre-delivery-open", "filled")
            && s.at("positions_reconciled") == true;
    });

    // Advance into delivery month 2026-10.
    ctp::SharedLibrary(ASTERION_TEST_CTP_TRADER, "asterion_fake_trader_next_day")
        .symbol<void (*)(const char*)>()("20261009");
    ASSERT_EQ(
        wait_for(
            session,
            [](const Json& s) {
                return s.at("phase") == "ready" && s.at("trading_day") == "2026-10-09";
            })
            .at("trading_day"),
        "2026-10-09");

    // Physical commodity fixed contract open is rejected, while close is allowed.
    EXPECT_THROW(act(session, submit("rb-delivery-open", "1", "3500")), std::invalid_argument);
    auto close_rb = submit("rb-delivery-close", "2", "3500");
    close_rb["side"] = "sell";
    close_rb["offset"] = "close_yesterday";
    EXPECT_NO_THROW(act(session, close_rb));
    wait_for(session, [](const Json& s) {
        return order_has_status(s, "rb-delivery-close", "filled")
            && s.at("positions_reconciled") == true;
    });

    // Product-wide CZCE commodity open in delivery month is rejected.
    auto czce_open = submit("czce-delivery-open", "1", "3500", "TA610");
    czce_open["venue"] = "CZCE";
    EXPECT_THROW(act(session, czce_open), std::invalid_argument);

    // CFFEX physical treasury bond open in delivery month is rejected.
    auto bond_open = submit("bond-delivery-open", "1", "3500", "T2610");
    bond_open["venue"] = "CFFEX";
    EXPECT_THROW(act(session, bond_open), std::invalid_argument);

    // CFFEX cash-settled stock index open in delivery month remains allowed.
    auto idx_open = submit("index-delivery-open", "1", "3500", "IF2610");
    idx_open["venue"] = "CFFEX";
    EXPECT_NO_THROW(act(session, idx_open));
}
