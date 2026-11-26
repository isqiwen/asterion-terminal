#include "trading/live_session_fixture.hpp"

using namespace asterion;
using namespace asterion::test::live;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

class StrategyTarget : public Live
{
protected:
    // A later bar's decision that gives no new targets.
    static ::strategy::Decision none(std::uint64_t sequence, bool ready = true)
    {
        ::strategy::Decision decision;
        decision.set_sequence(sequence);
        decision.set_ready(ready);
        return decision;
    }

    // Three lots wanted, one filled, then the batch expires: two lots are left.
    void fill_one_of_three(LiveSession& session, asterion::trading::StrategyAuthorityPtr owner)
    {
        session.accept_target(owner, lots(3), {quote()}, steady_now(*clock) + 1500ms);
        session.advance_target(owner).get();
        exchange.call<void (*)(int, int)>("asterion_fake_trader_fill", 0, 1);
        wait_for(session, [](const Json& s) {
            return !s.at("orders").empty() && s.at("orders")[0].at("status") == "partially_filled";
        });
        progress(session, owner, [](const Json& s) {
            return s.at("orders")[0].at("status") == "cancelled"
                && s.at("positions_reconciled") == true;
        });
    }

    static ::strategy::Decision lots(std::int64_t quantity, std::uint64_t sequence = 1)
    {
        ::strategy::Decision decision;
        decision.set_sequence(sequence);
        decision.set_ready(true);
        auto* target = decision.add_targets();
        target->mutable_instrument()->set_exchange("SHFE");
        target->mutable_instrument()->set_symbol("rb2610");
        target->set_lots(quantity);
        return decision;
    }

    static MarketQuote quote()
    {
        MarketQuote quote;
        quote.instrument = {"SHFE", "rb2610"};
        quote.trading_day = "20260928";
        quote.last = Decimal::parse("3500");
        quote.bid = Decimal::parse("3499");
        quote.ask = Decimal::parse("3501");
        quote.bid_quantity = quote.ask_quantity = 100;
        quote.lower_limit = Decimal::parse("3300");
        quote.upper_limit = Decimal::parse("3700");
        quote.source_ms = quote.received_ms = test_clock
            ? utc_now_ms(*test_clock)
            : std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count();
        return quote;
    }

    static auto filled(std::size_t count)
    {
        return [count](const Json& s) {
            return s.at("orders").size() == count && s.at("orders").back().at("status") == "filled"
                && s.at("positions_reconciled") == true;
        };
    }

    static auto resting(std::size_t count)
    {
        return [count](const Json& s) {
            return s.at("orders").size() == count
                && s.at("orders").back().at("status") == "accepted";
        };
    }

    static std::string held(const Json& state)
    {
        Decimal lots;
        for (const auto& position : state.at("positions"))
            lots = lots + Decimal::parse(position.at("today").get<std::string>())
                + Decimal::parse(position.at("yesterday").get<std::string>());
        return lots.str();
    }

    template <class Predicate>
    static Json
    progress(LiveSession& session, asterion::trading::StrategyAuthorityPtr owner, Predicate done)
    {
        const auto deadline = std::chrono::steady_clock::now() + 9s;
        for (;;) {
            if (test_clock)
                test_clock->advance(50ms);
            session.advance_target(owner).get();
            auto state = session.snapshot();
            if (done(state) || std::chrono::steady_clock::now() >= deadline)
                return state;
            std::this_thread::sleep_for(test_clock ? 5ms : 20ms);
        }
    }
};

} // namespace

TEST_F(StrategyTarget, DirectQuotesDriveSerialCloseThenOpenFromReconciledPositions)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    auto owner = session.register_strategy().get();
    // Execution must use this feed, even when the trading-front quote query fails.
    exchange.call<void (*)(int, int)>("asterion_fake_trader_reject_quotes", 31, 100);
    session.accept_target(owner, lots(1), {quote()});
    auto state = progress(session, owner, [](const Json& s) {
        return s.at("orders").size() == 1 && s.at("orders")[0].at("status") == "filled"
            && s.at("positions_reconciled") == true;
    });
    ASSERT_EQ(state.at("orders").size(), 1U);
    EXPECT_EQ(decimal(state.at("orders")[0].at("limit_price")), "3501");
    session.advance_target(owner).get(); // Retire the settled first batch.
    session.accept_target(owner, lots(-1, 2), {quote()});
    state = progress(session, owner, [](const Json& s) {
        return s.at("orders").size() == 3 && s.at("orders")[2].at("status") == "filled"
            && s.at("positions_reconciled") == true;
    });
    ASSERT_EQ(state.at("orders").size(), 3U);
    EXPECT_EQ(state.at("orders")[1].at("offset"), "close_today");
    EXPECT_EQ(state.at("orders")[2].at("offset"), "open");
    for (int i : {1, 2}) {
        EXPECT_EQ(state.at("orders")[i].at("side"), "sell");
        EXPECT_EQ(decimal(state.at("orders")[i].at("limit_price")), "3499");
        EXPECT_EQ(state.at("orders")[i].at("status"), "filled");
    }
    EXPECT_TRUE(session.strategy_failure(owner).empty());
}

TEST_F(StrategyTarget, WarmingCannotSendAndUnauthorizedTargetsAreSkipped)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    auto owner = session.register_strategy().get();
    auto warming = lots(1);
    warming.set_ready(false);
    warming.clear_targets();
    session.accept_target(owner, warming, {});
    session.advance_target(owner).get();
    session.accept_target(owner, lots(1, 2), {quote()});
    session.advance_target(owner).get();
    EXPECT_TRUE(session.snapshot().at("orders").empty());
    EXPECT_TRUE(session.strategy_failure(owner).empty());
    act(session, authorize());
    session.accept_target(owner, none(3), {quote()});
    const auto state = progress(session, owner, [](const Json& s) {
        return s.at("orders").size() == 1;
    });
    EXPECT_EQ(state.at("orders").size(), 1U);
}

TEST_F(StrategyTarget, StaleQuotesAndMissingOppositeBookCannotProduceOrders)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    auto owner = session.register_strategy("stale").get();
    auto stale = quote();
    stale.source_ms -= 10000;
    session.accept_target(owner, lots(1), {stale});
    auto missing = quote();
    missing.ask.reset();
    session.accept_target(owner, lots(1, 2), {missing});
    session.advance_target(owner).get();
    EXPECT_TRUE(session.snapshot().at("orders").empty());
    EXPECT_TRUE(session.strategy_failure(owner).empty());
}

TEST_F(StrategyTarget, PartialFillWaitsAndOriginalBatchExpiryCancelsWithoutResending)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    auto owner = session.register_strategy().get();
    session.accept_target(owner, lots(3), {quote()}, steady_now(*clock) + 1500ms);
    session.advance_target(owner).get();
    exchange.call<void (*)(int, int)>("asterion_fake_trader_fill", 0, 1);
    auto state = wait_for(session, [](const Json& s) {
        return !s.at("orders").empty() && s.at("orders")[0].at("status") == "partially_filled";
    });
    ASSERT_EQ(state.at("orders").size(), 1U);
    // Advance ManualClock past the 1500 ms batch deadline without real-time sleep.
    clock->advance(2s);
    session.advance_target(owner).get();
    state = wait_for(session, [](const Json& s) {
        return s.at("orders")[0].at("status") == "cancelled";
    });
    EXPECT_EQ(state.at("orders").size(), 1U);
    EXPECT_EQ(decimal(state.at("orders")[0].at("filled")), "1");
    // Expiry drops the batch, not the instance; the next target is admitted.
    EXPECT_TRUE(session.strategy_failure(owner).empty());
    EXPECT_NO_THROW(session.accept_target(owner, lots(3, 2), {quote()}, steady_now(*clock) + 10s));
}

TEST_F(StrategyTarget, ATargetStandsAcrossLaterBarsUntilItIsReached)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    auto owner = session.register_strategy().get();
    fill_one_of_three(session, owner);
    // Paused: the next bar only reduces, so nothing is sent.
    session.pause_strategy(owner, true);
    session.accept_target(owner, none(2), {quote()});
    for (int i = 0; i < 5; ++i)
        session.advance_target(owner).get();
    EXPECT_EQ(session.snapshot().at("orders").size(), 1U);
    // Resumed: the bar after sends what is left of the three lots.
    session.pause_strategy(owner, false);
    session.accept_target(owner, none(3), {quote()});
    const auto state = progress(session, owner, filled(2));
    ASSERT_EQ(state.at("orders").size(), 2U);
    EXPECT_EQ(decimal(state.at("orders")[1].at("quantity")), "2");
    EXPECT_EQ(held(state), "3");
    EXPECT_TRUE(session.strategy_failure(owner).empty());
}

TEST_F(StrategyTarget, PartialLotTargetsKeepOtherContractsStandingTargets)
{
    auto definition = manifest(3);
    definition["policy"]["contracts"].push_back(contract("rb2611", "rb", "2026-11"));
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, definition);
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    auto owner =
        session.register_strategy("partial-target", {{"SHFE", "rb2610"}, {"SHFE", "rb2611"}}).get();
    session.pause_strategy(owner, true);
    session.accept_target(owner, lots(1), {quote()});
    session.advance_target(owner).get();
    auto second = quote();
    second.instrument.symbol = "rb2611";
    auto next = lots(1, 2);
    next.mutable_targets(0)->mutable_instrument()->set_symbol("rb2611");
    session.accept_target(owner, next, {quote(), second});
    session.advance_target(owner).get();
    session.pause_strategy(owner, false);
    session.accept_target(owner, none(3), {quote(), second});
    exchange.call<void (*)(int)>("asterion_fake_trader_hold_trades", 1);
    session.advance_target(owner).get();
    {
        HeldCommit hold;
        auto sending = session.advance_target(owner);
        ASSERT_TRUE(hold.waiting());
        exchange.call<void (*)(int)>("asterion_fake_trader_hold_trades", 0);
        wait_for(session, filled(1));
        hold.release();
        EXPECT_NO_THROW(sending.get());
    }
    const auto state = progress(session, owner, filled(2));
    ASSERT_EQ(state.at("orders").size(), 2U);
    EXPECT_EQ(held(state), "2");
    EXPECT_EQ(state.at("positions").size(), 2U);
}

TEST_F(StrategyTarget, ANewFlatTargetWithAStaleQuoteCannotReviveTheOldOpeningTarget)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    auto owner = session.register_strategy().get();
    session.pause_strategy(owner, true);
    session.accept_target(owner, lots(1), {quote()});
    session.advance_target(owner).get();
    auto stale = quote();
    stale.source_ms -= 10001;
    session.accept_target(owner, lots(0, 2), {stale});
    session.pause_strategy(owner, false);
    session.accept_target(owner, none(3), {quote()});
    session.advance_target(owner).get();
    EXPECT_TRUE(session.snapshot().at("orders").empty());
    EXPECT_TRUE(session.strategy_failure(owner).empty());
}

TEST_F(StrategyTarget, AStrategyThatIsNotReadyDropsItsStandingTarget)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    auto owner = session.register_strategy().get();
    fill_one_of_three(session, owner);
    session.accept_target(owner, none(2, false), {});
    session.accept_target(owner, none(3), {quote()});
    for (int i = 0; i < 5; ++i)
        session.advance_target(owner).get();
    EXPECT_EQ(session.snapshot().at("orders").size(), 1U);
}

TEST_F(StrategyTarget, ManualCancellationDropsTheBatchWithoutReplacingItsOrder)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    auto owner = session.register_strategy().get();
    session.accept_target(owner, lots(3), {quote()});
    session.advance_target(owner).get();
    const auto state = wait_for(session, [](const Json& s) {
        return !s.at("orders").empty() && s.at("orders")[0].at("status") == "accepted";
    });
    ASSERT_EQ(state.at("orders").size(), 1U);
    const auto id = state.at("orders")[0].at("id");
    act(session, {{"request_id", "cancel.target"}, {"action", "cancel"}, {"order_id", id}});
    wait_for(session, [](const Json& s) {
        return s.at("orders")[0].at("status") == "cancelled";
    });
    session.advance_target(owner).get();
    session.advance_target(owner).get();
    EXPECT_EQ(session.snapshot().at("orders").size(), 1U);
    EXPECT_TRUE(session.strategy_failure(owner).empty());
}

TEST_F(StrategyTarget, ANewerTargetReplacesAnUnfinishedBatchAndRepeatsAreRefused)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    auto owner = session.register_strategy().get();
    session.accept_target(owner, lots(1), {quote()});
    session.accept_target(owner, lots(2, 2), {quote()});
    const auto state = progress(session, owner, [](const Json& s) {
        return s.at("orders").size() == 1;
    });
    ASSERT_EQ(state.at("orders").size(), 1U);
    EXPECT_EQ(decimal(state.at("orders")[0].at("quantity")), "2");
    EXPECT_THROW(session.accept_target(owner, lots(3, 2), {quote()}), Error);
}

TEST_F(StrategyTarget, CancellingASupersededBatchsOrderDoesNotDropTheNewBatch)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    auto owner = session.register_strategy().get();
    session.accept_target(owner, lots(3), {quote()});
    ASSERT_EQ(
        progress(
            session,
            owner,
            [](const Json& s) {
                return s.at("orders").size() == 1;
            })
            .at("orders")
            .size(),
        1U);
    // The newer target cancels the resting order once, then opens from the result.
    session.accept_target(owner, lots(3, 2), {quote()});
    const auto state = progress(session, owner, [](const Json& s) {
        return s.at("orders").size() == 2;
    });
    ASSERT_EQ(state.at("orders").size(), 2U);
    EXPECT_EQ(state.at("orders")[0].at("status"), "cancelled");
    EXPECT_EQ(decimal(state.at("orders")[1].at("quantity")), "3");
    EXPECT_TRUE(session.strategy_failure(owner).empty());
}

TEST_F(StrategyTarget, APausedInstanceOnlyReducesAndFlatteningClosesItsPosition)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    auto owner = session.register_strategy().get();
    session.accept_target(owner, lots(2), {quote()});
    ASSERT_EQ(progress(session, owner, filled(1)).at("orders").size(), 1U);
    session.advance_target(owner).get(); // Retire the settled batch.
    session.pause_strategy(owner, true);
    session.accept_target(owner, lots(4, 2), {quote()});
    for (int i = 0; i < 5; ++i)
        session.advance_target(owner).get();
    EXPECT_EQ(session.snapshot().at("orders").size(), 1U) << "paused: no opening order";
    session.flatten_strategy(owner, {quote()});
    const auto state = progress(session, owner, filled(2));
    ASSERT_EQ(state.at("orders").size(), 2U);
    EXPECT_EQ(state.at("orders")[1].at("side"), "sell");
    EXPECT_EQ(decimal(state.at("orders")[1].at("quantity")), "2");
    EXPECT_TRUE(session.strategy_failure(owner).empty());
    const auto events = session.strategy_events(owner);
    const auto count = [&](::trading::InstanceEventKind kind) {
        return std::ranges::count_if(events, [&](const auto& e) {
            return e.kind() == kind;
        });
    };
    EXPECT_EQ(count(::trading::INSTANCE_EVENT_KIND_FLATTENING), 1);
    EXPECT_EQ(count(::trading::INSTANCE_EVENT_KIND_ORDER_SENT), 2);
}

TEST_F(StrategyTarget, PausingDuringDurableSubmissionStopsTheOpeningButNotLaterReductions)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    auto owner = session.register_strategy().get();
    session.accept_target(owner, lots(2), {quote()});
    ASSERT_EQ(progress(session, owner, filled(1)).at("orders").size(), 1U);
    session.advance_target(owner).get();
    session.accept_target(owner, lots(4, 2), {quote()});
    HeldCommit hold;
    auto sending = session.advance_target(owner);
    ASSERT_TRUE(hold.waiting());
    session.pause_strategy(owner, true);
    hold.release();
    EXPECT_NO_THROW(sending.get());
    EXPECT_EQ(session.snapshot().at("orders").size(), 1U)
        << "paused: the recorded open is not sent";
    EXPECT_TRUE(session.snapshot().at("unconfirmed").empty());
    session.accept_target(owner, lots(0, 3), {quote()});
    const auto state = progress(session, owner, filled(2));
    ASSERT_EQ(state.at("orders").size(), 2U);
    EXPECT_EQ(state.at("orders")[1].at("side"), "sell");
    EXPECT_TRUE(session.strategy_failure(owner).empty());
}

TEST_F(StrategyTarget, AFailedLegInFlightDoesNotDropTheFlatteningOrTargetThatReplacedIt)
{
    for (const bool flatten : {true, false}) {
        exchange.reset();
        Directory journal, lock;
        LiveSession session(journal.path, ASTERION_TEST_CTP_TRADER, lock.path, manifest());
        ASSERT_EQ(ready(session).at("phase"), "ready");
        act(session, authorize());
        auto owner = session.register_strategy().get();
        session.accept_target(owner, lots(2), {quote()});
        ASSERT_EQ(progress(session, owner, filled(1)).at("orders").size(), 1U);
        session.advance_target(owner).get();
        session.accept_target(owner, lots(4, 2), {quote()});
        HeldCommit hold;
        auto adding = session.advance_target(owner);
        ASSERT_TRUE(hold.waiting());
        if (flatten)
            session.flatten_strategy(owner, {quote()});
        else
            session.accept_target(owner, lots(0, 3), {quote()});
        hold.release();
        EXPECT_NO_THROW(adding.get());
        const auto state = progress(session, owner, filled(2));
        ASSERT_EQ(state.at("orders").size(), 2U) << "flatten=" << flatten;
        EXPECT_EQ(state.at("orders")[1].at("side"), "sell");
        EXPECT_EQ(decimal(state.at("orders")[1].at("quantity")), "2");
        EXPECT_TRUE(session.strategy_failure(owner).empty());
    }
}

TEST_F(StrategyTarget, TargetsAboveTheOrderLimitAreSentAsSeveralLegs)
{
    // Lots are whole, so a 5.5 limit still sends 5-lot legs.
    for (const char* limit : {"5", "5.5"}) {
        exchange.reset();
        Directory journal, lock;
        auto definition = manifest();
        definition["policy"]["risk"]["max_order_quantity"] = limit;
        LiveSession session(journal.path, ASTERION_TEST_CTP_TRADER, lock.path, definition);
        ASSERT_EQ(ready(session).at("phase"), "ready");
        act(session, authorize());
        auto owner = session.register_strategy().get();
        session.accept_target(owner, lots(6), {quote()});
        ASSERT_EQ(progress(session, owner, resting(1)).at("orders").size(), 1U) << limit;
        exchange.call<void (*)(int, int)>("asterion_fake_trader_fill", 0, 5);
        auto state = progress(session, owner, filled(2));
        ASSERT_EQ(state.at("orders").size(), 2U) << limit;
        EXPECT_EQ(held(state), "6");
        session.advance_target(owner).get(); // Retire the settled batch.
        session.flatten_strategy(owner, {quote()});
        ASSERT_EQ(progress(session, owner, resting(3)).at("orders").size(), 3U) << limit;
        exchange.call<void (*)(int, int)>("asterion_fake_trader_fill", 2, 5);
        state = progress(session, owner, filled(4));
        ASSERT_EQ(state.at("orders").size(), 4U) << limit;
        for (int i : {0, 1, 2, 3}) {
            EXPECT_EQ(decimal(state.at("orders")[i].at("quantity")), i % 2 ? "1" : "5");
            EXPECT_EQ(state.at("orders")[i].at("side"), i < 2 ? "buy" : "sell");
        }
        EXPECT_EQ(held(state), "0");
        EXPECT_TRUE(session.strategy_failure(owner).empty());
    }
}

TEST_F(StrategyTarget, PausingRevokesAnOpeningQueuedToTheBrokerButNotAReduction)
{
    for (const bool opening : {true, false}) {
        exchange.reset();
        Directory journal, lock;
        LiveSession session(journal.path, ASTERION_TEST_CTP_TRADER, lock.path, manifest());
        ASSERT_EQ(ready(session).at("phase"), "ready");
        act(session, authorize());
        act(session, submit("seed", "2"));
        ASSERT_EQ(held(wait_for(session, filled(1))), "2");

        struct Release
        {
            ~Release()
            {
                exchange.call<void (*)(int)>("asterion_fake_trader_hold_quote_return", 0);
            }

            FakeExchange& exchange;
        } release{exchange};

        // A manual order's broker quote occupies the SDK thread.
        exchange.call<void (*)(int)>("asterion_fake_trader_hold_quote_return", 1);
        const auto before = session.snapshot();
        auto manual = session.execute(
            before.at("account_id").get<std::string>(),
            before.at("policy_revision").get<std::string>(),
            submit("manual", "1"));
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (!exchange.call<int (*)()>("asterion_fake_trader_quote_return_waiting")
            && std::chrono::steady_clock::now() < deadline) {
            clock->advance(250ms);
            std::this_thread::sleep_for(5ms);
        }
        ASSERT_TRUE(exchange.call<int (*)()>("asterion_fake_trader_quote_return_waiting"));
        auto owner = session.register_strategy().get();
        session.accept_target(owner, lots(opening ? 4 : 0), {quote()});
        auto sending = session.advance_target(owner);
        // Recorded, so its dispatch waits behind the held quote.
        ASSERT_EQ(
            wait_for(
                session,
                [](const Json& s) {
                    return !s.at("unconfirmed").empty();
                })
                .at("unconfirmed")
                .size(),
            1U);
        session.pause_strategy(owner, true);
        exchange.call<void (*)(int)>("asterion_fake_trader_hold_quote_return", 0);
        EXPECT_NO_THROW(sending.get());
        EXPECT_ANY_THROW(manual.get());
        const auto state = progress(session, owner, filled(opening ? 1 : 2));
        EXPECT_TRUE(state.at("unconfirmed").empty());
        ASSERT_EQ(state.at("orders").size(), opening ? 1U : 2U) << "opening=" << opening;
        EXPECT_EQ(held(state), opening ? "2" : "0");
        EXPECT_TRUE(session.strategy_failure(owner).empty());
    }
}

TEST_F(StrategyTarget, ConcurrentBrokerReportBeforeDurableSubmissionReevaluatesWithoutAbandoning)
{
    auto definition = manifest(4);
    definition["policy"]["max_margin_ratio"] = "0.8";
    definition["policy"]["contracts"].push_back(contract("cu2611", "cu", "2026-11"));
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, definition);
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    // A resting order whose fill advances the send gate while another leg submits.
    act(session, submit("seed.resting", "3"));
    ASSERT_EQ(wait_for(session, resting(1)).at("orders").size(), 1U);

    const InstrumentId cu{"SHFE", "cu2611"};
    auto owner = session.register_strategy("concurrent", {cu}).get();
    ::strategy::Decision decision;
    decision.set_sequence(1);
    decision.set_ready(true);
    auto* target = decision.add_targets();
    target->mutable_instrument()->set_exchange("SHFE");
    target->mutable_instrument()->set_symbol("cu2611");
    target->set_lots(1);
    auto cu_quote = quote();
    cu_quote.instrument = cu;
    session.accept_target(owner, decision, {cu_quote});

    auto first = session.advance_target(owner);
    wait_for(session, [](const Json& s) {
        return std::ranges::any_of(s.at("costs"), [](const Json& c) {
            return c.at("symbol") == "cu2611" && c.at("state") == "querying";
        });
    });
    exchange.call<void (*)(int, int)>("asterion_fake_trader_fill", 0, 1);
    EXPECT_NO_THROW(first.get());

    const auto state = progress(session, owner, filled(2));
    ASSERT_EQ(state.at("orders").size(), 2U);
    EXPECT_EQ(state.at("orders")[1].at("symbol"), "cu2611");
    EXPECT_EQ(state.at("orders")[1].at("status"), "filled");
    for (const auto& ev : session.strategy_events(owner))
        EXPECT_NE(ev.kind(), ::trading::INSTANCE_EVENT_KIND_BATCH_ABANDONED) << ev.text();
    EXPECT_TRUE(session.strategy_failure(owner).empty());
}

TEST_F(StrategyTarget, MultiLegOpeningPacesWithinWorkingOrderLimitWithoutAbandoning)
{
    auto definition = manifest(1);
    definition["policy"]["contracts"].push_back(contract("rb2611", "rb", "2026-11"));
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, definition);
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    const InstrumentId first{"SHFE", "rb2610"}, second{"SHFE", "rb2611"};
    auto owner = session.register_strategy("paced", {first, second}).get();
    // Three lots rest on the fake exchange until filled explicitly.
    auto decision = lots(3);
    auto* leg2 = decision.add_targets();
    leg2->mutable_instrument()->set_exchange("SHFE");
    leg2->mutable_instrument()->set_symbol("rb2611");
    leg2->set_lots(1);
    auto other = quote();
    other.instrument = second;
    session.accept_target(owner, decision, {quote(), other});
    session.advance_target(owner).get();
    const auto waiting = wait_for(session, resting(1));
    ASSERT_EQ(waiting.at("orders").size(), 1U);
    ASSERT_EQ(waiting.at("orders")[0].at("status"), "accepted");
    // Polling while the first leg occupies the only working-order slot must wait, not abandon.
    EXPECT_NO_THROW(session.advance_target(owner).get());
    EXPECT_EQ(session.snapshot().at("orders").size(), 1U);
    const auto first_events = session.strategy_events(owner);
    ASSERT_EQ(first_events.size(), 1U);
    EXPECT_EQ(first_events[0].kind(), ::trading::INSTANCE_EVENT_KIND_ORDER_SENT);
    exchange.call<void (*)(int, int)>("asterion_fake_trader_fill", 0, 3);
    const auto state = progress(session, owner, filled(2));
    ASSERT_EQ(state.at("orders").size(), 2U);
    EXPECT_EQ(state.at("orders")[1].at("symbol"), "rb2611");
    EXPECT_EQ(state.at("orders")[1].at("status"), "filled");
    for (const auto& ev : session.strategy_events(owner))
        EXPECT_NE(ev.kind(), ::trading::INSTANCE_EVENT_KIND_BATCH_ABANDONED) << ev.text();
    EXPECT_TRUE(session.strategy_failure(owner).empty());
}

TEST_F(StrategyTarget, ExternalRestingOrderStillRejectsStrategyOpeningOnWorkingOrderLimit)
{
    auto definition = manifest(1);
    definition["policy"]["contracts"].push_back(contract("cu2611", "cu", "2026-11"));
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, definition);
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    act(session, submit("external.resting", "3"));
    ASSERT_EQ(wait_for(session, resting(1)).at("orders").size(), 1U);
    const InstrumentId cu{"SHFE", "cu2611"};
    auto owner = session.register_strategy("blocked", {cu}).get();
    ::strategy::Decision decision;
    decision.set_sequence(1);
    decision.set_ready(true);
    auto* target = decision.add_targets();
    target->mutable_instrument()->set_exchange("SHFE");
    target->mutable_instrument()->set_symbol("cu2611");
    target->set_lots(1);
    auto cu_quote = quote();
    cu_quote.instrument = cu;
    session.accept_target(owner, decision, {cu_quote});
    EXPECT_NO_THROW(session.advance_target(owner).get());
    EXPECT_EQ(session.snapshot().at("orders").size(), 1U);
    const auto events = session.strategy_events(owner);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].kind(), ::trading::INSTANCE_EVENT_KIND_BATCH_ABANDONED);
    EXPECT_NE(events[0].text().find("working_orders"), std::string::npos);
    EXPECT_TRUE(session.strategy_failure(owner).empty());
}

TEST_F(StrategyTarget, FlatteningBeforeTheFirstDecisionClosesOnlyFromAFreshQuote)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    auto previous = session.register_strategy("previous").get();
    session.accept_target(previous, lots(2), {quote()});
    const auto opened = progress(session, previous, [](const Json& s) {
        return s.at("orders").size() == 1 && s.at("orders")[0].at("status") == "filled"
            && s.at("positions_reconciled") == true;
    });
    ASSERT_EQ(opened.at("orders")[0].at("status"), "filled");
    session.release_strategy(previous).get();
    // A restarted instance holds the position but has not decided anything yet.
    auto owner = session.register_strategy("restarted").get();
    auto stale = quote();
    stale.source_ms = stale.received_ms -= 10000;
    session.flatten_strategy(owner, {stale});
    session.advance_target(owner).get();
    EXPECT_EQ(session.snapshot().at("orders").size(), 1U) << "a stale quote sends nothing";
    session.flatten_strategy(owner, {quote()});
    const auto state = progress(session, owner, [](const Json& s) {
        return s.at("orders").size() == 2 && s.at("orders")[1].at("status") == "filled";
    });
    ASSERT_EQ(state.at("orders").size(), 2U);
    EXPECT_EQ(state.at("orders")[1].at("side"), "sell");
    EXPECT_EQ(decimal(state.at("orders")[1].at("quantity")), "2");
    EXPECT_TRUE(session.strategy_failure(owner).empty());
}

TEST_F(StrategyTarget, RevocationDuringDirectQuoteSubmissionStillFencesDurableDispatch)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    auto owner = session.register_strategy().get();
    session.accept_target(owner, lots(1), {quote()});
    HeldCommit hold;
    auto sending = session.advance_target(owner);
    ASSERT_TRUE(hold.waiting());
    const auto state = session.snapshot();
    auto revoking = session.execute(
        state.at("account_id").get<std::string>(),
        state.at("policy_revision").get<std::string>(),
        {{"request_id", "revoke.target"}, {"action", "live_revoke"}});
    hold.release();
    EXPECT_THROW(sending.get(), Error);
    revoking.get();
    EXPECT_TRUE(session.snapshot().at("orders").empty());
    EXPECT_TRUE(session.snapshot().at("unconfirmed").empty());
}

TEST_F(StrategyTarget, WeightsWaitForNewEquityAndPersistTheFrozenConversionWithTheOrder)
{
    {
        LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
        ASSERT_EQ(ready(session).at("phase"), "ready");
        act(session, authorize());
        auto owner = session
                         .register_strategy(
                             "weighted",
                             {{"SHFE", "rb2610"}},
                             asterion::trading::StrategyWeightBudget{
                                 Decimal::parse("1"),
                                 Decimal::parse("1")
                             })
                         .get();
        auto decision = lots(0);
        decision.mutable_targets(0)->mutable_weight()->set_text("0.07");
        // Old equity would buy two lots. Only the post-admission response buys one.
        exchange.call<void (*)(double)>("asterion_fake_trader_balance", 500000.01);
        session.accept_target(owner, decision, {quote()});
        session.advance_target(owner).get();
        EXPECT_TRUE(session.snapshot().at("orders").empty());
        const auto state = progress(session, owner, [](const Json& s) {
            return s.at("orders").size() == 1 && s.at("orders")[0].at("status") == "filled"
                && s.at("positions_reconciled") == true;
        });
        ASSERT_EQ(state.at("orders").size(), 1U);
        EXPECT_EQ(decimal(state.at("orders")[0].at("quantity")), "1");
        session.advance_target(owner).get();
        EXPECT_TRUE(session.strategy_failure(owner).empty());
    }
    const auto parsed =
        asterion::trading::parse_record(test::read_record(test::journal_record(directory.path, 2)));
    const auto& record = std::get<asterion::trading::CommandRecord>(parsed);
    const auto& source = record.request.as<asterion::trading::SubmitOrder>()->strategy;
    ASSERT_TRUE(source && source->weight_sizing);
    EXPECT_EQ(source->weight_sizing->equity, Decimal::parse("500000.01"));
    EXPECT_EQ(source->weight_sizing->weight, Decimal::parse("0.07"));
    EXPECT_EQ(source->weight_sizing->price, Decimal::parse("3500"));
    EXPECT_EQ(source->weight_sizing->quantity, Decimal::parse("1"));
    LiveSession recovered(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
    ASSERT_EQ(ready(recovered).at("phase"), "ready");
    EXPECT_FALSE(recovered.snapshot().contains("authorization"));
    EXPECT_EQ(recovered.snapshot().at("orders").size(), 1U);
}

TEST_F(StrategyTarget, WeightsRequireExplicitBudgetsAndCannotExceedThem)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    auto decision = lots(0);
    decision.mutable_targets(0)->mutable_weight()->set_text("0.07");
    auto owner = session.register_strategy("missing-budget").get();
    EXPECT_THROW(session.accept_target(owner, decision, {quote()}), std::invalid_argument);
    session.release_strategy(owner).get();
    owner = session
                .register_strategy(
                    "limited",
                    {{"SHFE", "rb2610"}},
                    asterion::trading::StrategyWeightBudget{
                        Decimal::parse("0.05"),
                        Decimal::parse("0.05")
                    })
                .get();
    session.accept_target(owner, decision, {quote()});
    EXPECT_THROW(
        progress(
            session,
            owner,
            [](const Json&) {
                return false;
            }),
        std::invalid_argument);
    EXPECT_TRUE(session.snapshot().at("orders").empty());
    EXPECT_FALSE(session.strategy_failure(owner).empty());
}

TEST_F(StrategyTarget, LaterEquityReportsCannotResizeTheRemainingPortfolioLeg)
{
    auto definition = manifest();
    definition["policy"]["contracts"].push_back(contract("rb2611", "rb", "2026-11"));
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, definition);
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    auto owner =
        session
            .register_strategy(
                "portfolio",
                {{"SHFE", "rb2610"}, {"SHFE", "rb2611"}},
                asterion::trading::StrategyWeightBudget{Decimal::parse("1"), Decimal::parse("1")})
            .get();
    auto decision = lots(0);
    decision.mutable_targets(0)->mutable_weight()->set_text("0.07");
    auto* second = decision.add_targets();
    second->mutable_instrument()->set_exchange("SHFE");
    second->mutable_instrument()->set_symbol("rb2611");
    second->mutable_weight()->set_text("0.07");
    auto other = quote();
    other.instrument.symbol = "rb2611";
    exchange.call<void (*)(double)>("asterion_fake_trader_balance", 500000.01);
    session.accept_target(owner, decision, {quote(), other});
    auto state = progress(session, owner, [](const Json& s) {
        return s.at("orders").size() == 1;
    });
    ASSERT_EQ(state.at("orders").size(), 1U);
    exchange.call<void (*)(double)>("asterion_fake_trader_balance", 1000000.01);
    wait_for(session, [&](const Json&) {
        const auto snapshot = session.wire_snapshot();
        return snapshot.has_funds() && snapshot.funds().balance().text() == "1000000.01";
    });
    state = progress(session, owner, [](const Json& s) {
        return s.at("orders").size() == 2 && s.at("orders")[1].at("status") == "filled"
            && s.at("positions_reconciled") == true;
    });
    ASSERT_EQ(state.at("orders").size(), 2U);
    EXPECT_EQ(state.at("orders")[1].at("symbol"), "rb2611");
    EXPECT_EQ(decimal(state.at("orders")[1].at("quantity")), "1");
    EXPECT_TRUE(session.strategy_failure(owner).empty());
}

TEST_F(
    StrategyTarget,
    PriceDeviationRejectKeepsInstanceAliveAndTenContractPortfolioCompletesWithinTenSeconds)
{
    // Measure the real batch duration without accelerating its deadline while polling.
    test_clock.reset();
    exchange.set_clock(nullptr);
    auto definition = manifest(2);
    definition["policy"]["risk"]["max_gross_quantity"] = "15";
    definition["policy"]["risk"]["max_contract_quantity"] = "2";
    definition["policy"]["contracts"].clear();
    std::vector<InstrumentId> selected;
    for (int i = 1; i <= 10; ++i) {
        const auto month = (i < 10 ? std::string("0") : std::string{}) + std::to_string(i);
        const auto symbol = "rb27" + month;
        const auto delivery = "2027-" + month;
        definition["policy"]["contracts"].push_back(
            contract(symbol.c_str(), "rb", delivery.c_str()));
        selected.push_back({"SHFE", symbol});
    }
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, definition);
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    auto owner = session.register_strategy("portfolio-10", selected).get();

    // First bar: opposite-side ask deviates beyond max_price_deviation (2%).
    auto deviated = quote();
    deviated.instrument = selected.front();
    deviated.ask = Decimal::parse("3600");
    auto bad_decision = lots(1, 1);
    bad_decision.mutable_targets(0)->mutable_instrument()->set_symbol("rb2701");
    session.accept_target(owner, bad_decision, {deviated});
    EXPECT_NO_THROW(session.advance_target(owner).get());
    EXPECT_TRUE(session.strategy_failure(owner).empty());
    EXPECT_TRUE(session.snapshot().at("orders").empty());
    const auto abandoned_events = session.strategy_events(owner);
    ASSERT_EQ(abandoned_events.size(), 1U);
    EXPECT_EQ(abandoned_events[0].kind(), ::trading::INSTANCE_EVENT_KIND_BATCH_ABANDONED);
#ifdef ASTERION_SANITIZED
    // Ten legs fit the product's 10 s batch only at full speed.
    return;
#endif

    // Next bar: quote returns to normal and all 10 contracts rebalance within 10s.
    ::strategy::Decision ten;
    ten.set_sequence(2);
    ten.set_ready(true);
    std::vector<MarketQuote> quotes;
    for (const auto& id : selected) {
        auto* item = ten.add_targets();
        item->mutable_instrument()->set_exchange(id.venue);
        item->mutable_instrument()->set_symbol(id.symbol);
        item->set_lots(1);
        auto current = quote();
        current.instrument = id;
        quotes.push_back(current);
    }
    // Earlier callbacks can invalidate the next leg while its intent is being written.
    exchange.call<void (*)(int)>("asterion_fake_trader_hold_positions", 1);
    const auto started = std::chrono::steady_clock::now();
    session.accept_target(owner, ten, quotes);
    for (std::size_t count = 1; count <= selected.size(); ++count) {
        session.advance_target(owner).get();
        const auto filled = wait_for(session, [count](const Json& s) {
            return s.at("trades").size() == count;
        });
        ASSERT_EQ(filled.at("trades").size(), count);
    }
    exchange.call<void (*)(int)>("asterion_fake_trader_hold_positions", 0);
    const auto state = progress(session, owner, [](const Json& s) {
        return s.at("orders").size() == 10 && s.at("positions_reconciled") == true
            && s.at("positions").size() == 10;
    });
    EXPECT_LT(std::chrono::steady_clock::now() - started, 10s);
    ASSERT_EQ(state.at("orders").size(), 10U);
    ASSERT_EQ(state.at("positions").size(), 10U);
    EXPECT_TRUE(session.strategy_failure(owner).empty());
}

TEST_F(StrategyTarget, BrokerRejectionAbandonsTheBatchAndTheInstanceRunsTheNextTarget)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    auto owner = session.register_strategy().get();
    exchange.call<void (*)(int)>("asterion_fake_trader_reject_inserts", 1);
    session.accept_target(owner, lots(1), {quote()});
    session.advance_target(owner).get();
    const auto rejected = progress(session, owner, [](const Json& state) {
        return state.at("orders").size() == 1 && state.at("orders")[0].at("status") == "rejected";
    });
    ASSERT_EQ(rejected.at("orders")[0].at("status"), "rejected");
    EXPECT_TRUE(session.strategy_failure(owner).empty());
    const auto events = session.strategy_events(owner);
    EXPECT_EQ(
        std::ranges::count_if(
            events,
            [](const auto& event) {
                return event.kind() == ::trading::INSTANCE_EVENT_KIND_BATCH_ABANDONED;
            }),
        1);
    session.advance_target(owner).get();
    EXPECT_EQ(session.snapshot().at("orders").size(), 1U);
    exchange.call<void (*)(int)>("asterion_fake_trader_reject_inserts", 0);
    session.accept_target(owner, lots(1, 2), {quote()});
    const auto state = progress(session, owner, filled(2));
    ASSERT_EQ(state.at("orders").size(), 2U);
    EXPECT_EQ(held(state), "1");
}

TEST_F(StrategyTarget, CrossSessionWeightsAndPipelinedContractsAdvanceIndependently)
{
    auto definition = manifest(4);
    definition["policy"]["risk"]["max_gross_quantity"] = "30";
    definition["policy"]["risk"]["max_contract_quantity"] = "15";
    definition["policy"]["risk"]["max_order_quantity"] = "10";
    auto lh = contract("lh2611", "lh", "2026-11");
    lh["venue"] = "DCE";
    auto idx = contract("IF2612", "IF", "2026-12");
    idx["venue"] = "CFFEX";
    auto flat = contract("cu2611", "cu", "2026-11");
    definition["policy"]["contracts"].push_back(lh);
    definition["policy"]["contracts"].push_back(idx);
    definition["policy"]["contracts"].push_back(flat);

    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, definition);
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());

    // Seed an existing 2-lot position on day-only DCE/lh2611 that will reduce at 09:00.
    auto seed = submit("seed.lh", "2", "3501", "lh2611");
    seed["venue"] = "DCE";
    act(session, seed);
    ASSERT_EQ(held(wait_for(session, filled(1))), "2");

    const std::vector<InstrumentId>
        selected{{"SHFE", "rb2610"}, {"DCE", "lh2611"}, {"CFFEX", "IF2612"}, {"SHFE", "cu2611"}};
    auto owner = session
                     .register_strategy(
                         "cross-session",
                         selected,
                         asterion::trading::StrategyWeightBudget{
                             Decimal::parse("2"),
                             Decimal::parse("2"),
                             Decimal::parse("0.096"),
                             Decimal::parse("0.02")
                         })
                     .get();

    // Implied margin across rb (+1.0 * 0.12), lh (+0.2 * 0.12), IF (-0.4 * 0.12) = 0.192;
    // max_margin_ratio 0.096 scales weights by 0.5 -> rb=+0.5 (5 lots), lh=+0.1 (1 lot), IF=-0.2 (-2 lots), cu=0.
    exchange.call<void (*)(double)>("asterion_fake_trader_balance", 350000.0);

    ::strategy::Decision decision;
    decision.set_sequence(1);
    decision.set_ready(true);
    const std::pair<InstrumentId, const char*> raw[] = {
        {{"SHFE", "rb2610"}, "1.0"},
        {{"DCE", "lh2611"}, "0.2"},
        {{"CFFEX", "IF2612"}, "-0.4"},
        {{"SHFE", "cu2611"}, "0"}
    };
    for (const auto& [id, w] : raw) {
        auto* item = decision.add_targets();
        item->mutable_instrument()->set_exchange(id.venue);
        item->mutable_instrument()->set_symbol(id.symbol);
        item->mutable_weight()->set_text(w);
    }

    // 21:00 night session: only SHFE/rb2610 has a fresh quote; 5-lot order rests (> 2 lots in fake counter).
    session.accept_target(owner, decision, {quote()});
    const auto night_deadline = std::chrono::steady_clock::now() + 12s;
    Json night_state;
    for (;;) {
        clock->advance(250ms);
        session.advance_target(owner, {quote()}).get();
        night_state = session.snapshot();
        if ((night_state.at("orders").size() == 2
                && night_state.at("orders")[1].at("status") == "accepted")
            || std::chrono::steady_clock::now() >= night_deadline)
            break;
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_EQ(night_state.at("orders")[1].at("symbol"), "rb2610");
    EXPECT_EQ(decimal(night_state.at("orders")[1].at("quantity")), "5");

    // 09:00 morning open: DCE/lh2611 reduces from 2 lots to 1 lot while rb2610 is STILL resting.
    auto lh_quote = quote();
    lh_quote.instrument = {"DCE", "lh2611"};
    session.advance_target(owner, {lh_quote}).get();
    const auto morning_state = wait_for(session, [](const Json& s) {
        return s.at("orders").size() == 3 && s.at("orders")[2].at("status") == "filled"
            && s.at("positions_reconciled") == true;
    });
    EXPECT_EQ(morning_state.at("orders")[1].at("status"), "accepted");
    EXPECT_EQ(morning_state.at("orders")[2].at("symbol"), "lh2611");
    EXPECT_EQ(morning_state.at("orders")[2].at("side"), "sell");
    EXPECT_EQ(decimal(morning_state.at("orders")[2].at("quantity")), "1");

    // 09:30 CFFEX open: IF2612 opens -2 short lots concurrently while rb2610 is STILL resting; flat cu2611 needed no quote.
    auto if_quote = quote();
    if_quote.instrument = {"CFFEX", "IF2612"};
    session.advance_target(owner, {if_quote}).get();
    const auto cffex_state = wait_for(session, [](const Json& s) {
        return s.at("orders").size() == 4 && s.at("orders")[3].at("status") == "filled"
            && s.at("positions_reconciled") == true;
    });
    EXPECT_EQ(cffex_state.at("orders")[1].at("status"), "accepted");
    EXPECT_EQ(cffex_state.at("orders")[3].at("symbol"), "IF2612");
    EXPECT_EQ(cffex_state.at("orders")[3].at("side"), "sell");
    EXPECT_EQ(decimal(cffex_state.at("orders")[3].at("quantity")), "2");

    // Finally fill rb2610's resting 5-lot order and retire the settled batch.
    exchange.call<void (*)(int, int)>("asterion_fake_trader_fill", 1, 5);
    wait_for(session, [](const Json& s) {
        return s.at("orders")[1].at("status") == "filled" && s.at("positions_reconciled") == true;
    });
    session.advance_target(owner).get();
    EXPECT_TRUE(session.strategy_failure(owner).empty());
}

TEST_F(StrategyTarget, LaterQuotesCannotReopenAnExhaustedWeightMarginBudget)
{
    auto definition = manifest(4);
    definition["policy"]["contracts"].push_back(contract("rb2611", "rb", "2026-11"));
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, definition);
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    act(session, submit("seed", "2", "3501"));
    ASSERT_EQ(held(wait_for(session, filled(1))), "2");
    auto short_seed = submit("seed.other", "1", "3499", "rb2611");
    short_seed["side"] = "sell";
    act(session, short_seed);
    ASSERT_EQ(held(wait_for(session, filled(2))), "3");
    const auto owner = session
                           .register_strategy(
                               "margin",
                               {{"SHFE", "rb2610"}, {"SHFE", "rb2611"}},
                               asterion::trading::StrategyWeightBudget{
                                   Decimal::parse("1"),
                                   Decimal::parse("1"),
                                   Decimal::parse("0.024"),
                                   Decimal::parse("0.2")
                               })
                           .get();
    exchange.call<void (*)(double)>("asterion_fake_trader_balance", 350000.0);
    auto decision = lots(0);
    decision.mutable_targets(0)->mutable_weight()->set_text("0.02");
    auto* second = decision.add_targets();
    second->mutable_instrument()->set_exchange("SHFE");
    second->mutable_instrument()->set_symbol("rb2611");
    second->mutable_weight()->set_text("0.4");
    session.accept_target(owner, decision, {quote()});
    const auto basis = progress(session, owner, [](const Json& state) {
        return state.at("costs").size() == 2
            && std::ranges::all_of(
                state.at("costs"),
                [](const auto& cost) {
                    return cost.at("state") == "ready";
                })
            && decimal(state.at("funds").at("balance")) == "350000";
    });
    ASSERT_EQ(basis.at("costs").size(), 2U);
    // The drift floor retains two rb2610 lots, consuming the entire 8,400 margin cap.
    session.advance_target(owner, {quote()}).get();
    auto other = quote();
    other.instrument.symbol = "rb2611";
    for (int i = 0; i < 30; ++i) {
        clock->advance(250ms);
        other.source_ms = other.received_ms = utc_now_ms(*clock);
        session.advance_target(owner, {other}).get();
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_EQ(session.snapshot().at("orders").size(), 3U);
    EXPECT_EQ(held(session.snapshot()), "2");
    EXPECT_TRUE(session.strategy_failure(owner).empty());
}

TEST_F(StrategyTarget, MarginCapCountsFillsUntilTheNextReconciledFunds)
{
    auto definition = manifest(4);
    definition["policy"]["max_margin_ratio"] = "0.02";
    definition["policy"]["contracts"].push_back(contract("cu2611", "cu", "2026-11"));
    exchange.call<void (*)(double, double, double, double, double)>(
        "asterion_fake_trader_funds",
        1000000.0,
        1000000.0,
        0.0,
        0.0,
        0.0);
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, definition);
    ready(session);
    act(session, authorize());
    session.query_costs();
    wait_for(session, [](const Json& s) {
        return s.at("costs").size() == 2 && s.at("costs")[1].at("state") == "ready";
    });
    exchange.call<void (*)(int)>("asterion_fake_trader_hold_positions", 1);
    const auto owner =
        session.register_strategy("margin", {{"SHFE", "rb2610"}, {"SHFE", "cu2611"}}).get();
    session.accept_target(owner, lots(2), {quote()});
    session.advance_target(owner, {quote()}).get();
    wait_for(session, [](const Json& s) {
        return s.at("trades").size() == 1;
    });

    auto cu = quote();
    cu.instrument.symbol = "cu2611";
    auto target = lots(3, 2);
    target.mutable_targets(0)->mutable_instrument()->set_symbol("cu2611");
    session.accept_target(owner, target, {cu});
    session.advance_target(owner, {cu}).get();
    const auto events = session.strategy_events(owner);
    EXPECT_TRUE(std::ranges::any_of(events, [](const auto& e) {
        return e.text().find("margin_ratio") != std::string::npos;
    }));

    // Two lots still fit: the rb order and its trade consume the same margin.
    target.set_sequence(3);
    target.mutable_targets(0)->set_lots(2);
    session.accept_target(owner, target, {cu});
    session.advance_target(owner, {cu}).get();
    EXPECT_EQ(
        wait_for(
            session,
            [](const Json& s) {
                return s.at("trades").size() == 2;
            })
            .at("trades")
            .size(),
        2U);
    exchange.call<void (*)(int)>("asterion_fake_trader_hold_positions", 0);
}

TEST_F(StrategyTarget, CapitalAndCancelHardRiskHandlesCashflowsMarkToMarketRolloverAndRestart)
{
    auto m = manifest();
    m["policy"]["max_daily_loss"] = "15000";
    m["policy"]["max_drawdown"] = "25000";
    m["policy"]["max_margin_ratio"] = "0.7";
    m["policy"]["max_daily_cancels_per_contract"] = 2;

    const auto close_order =
        [](std::string id, const char* offset, const char* quantity, const char* price) {
            return Json{
                {"request_id", "submit." + id},
                {"action", "submit"},
                {"order_id", id},
                {"venue", "SHFE"},
                {"symbol", "rb2610"},
                {"side", "sell"},
                {"offset", offset},
                {"quantity", quantity},
                {"price", price}
            };
        };

    {
        exchange.call<void (*)(double)>("asterion_fake_trader_balance", 1000000.0);
        LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, m);
        ASSERT_EQ(ready(session).at("phase"), "ready");
        act(session, authorize("auth-1"));

        // Open 2 lots SHFE/rb2610 at 3500 (notional = 70,000, cost = 70,000).
        act(session, submit("ord-1", "2", "3500"));
        wait_for(session, [](const Json& s) {
            return !s.at("positions").empty() && s.at("positions_reconciled") == true;
        });

        // 1) Intraday withdrawal of 200,000: balance=800,000, withdraw=200,000 -> daily_loss=0, drawdown=0.
        exchange.call<void (*)(double, double, double, double, double)>(
            "asterion_fake_trader_funds",
            1000000.0,
            800000.0,
            0.0,
            200000.0,
            7000.0);
        wait_for(session, [](const Json& s) {
            return s.contains("funds") && decimal(s.at("funds").at("withdraw")) == "200000";
        });
        EXPECT_TRUE(session.snapshot().at("risk_breaches").empty());

        // 2) Intraday deposit of 100,000 + live mark-to-market drop on 2 lots rb2610 from 3500 -> 2700 (-16,000):
        // Deposit does not mask the 16,000 floating loss (>= 15,000 max_daily_loss).
        exchange.call<void (*)(double, double, double, double, double)>(
            "asterion_fake_trader_funds",
            1000000.0,
            900000.0,
            100000.0,
            200000.0,
            7000.0);
        wait_for(session, [](const Json& s) {
            return s.contains("funds") && decimal(s.at("funds").at("deposit")) == "100000";
        });
        exchange.call<void (*)(double)>("asterion_fake_trader_quote_price", 2700.0);
        auto mark = quote();
        mark.last = Decimal::parse("2700");
        session.advance_quotes({mark});
        const auto m2m_state = session.snapshot();
        EXPECT_EQ(decimal(m2m_state.at("funds").at("dynamic_equity")), "884000");
        ASSERT_TRUE(m2m_state.contains("risk_breaches"));
        EXPECT_EQ(m2m_state.at("risk_breaches"), Json::array({"daily_loss"}));
        EXPECT_THROW(act(session, submit("ord-2", "1", "2700")), std::invalid_argument);
        // Closing remains permitted while hard risk is breached!
        act(session, close_order("ord-close-1", "close_today", "1", "2700"));
        wait_for(session, [](const Json& s) {
            return order_has_status(s, "ord-close-1", "filled")
                && s.at("positions_reconciled") == true;
        });

        // 3) Price recovers to 3500; cancel 2 resting orders (>=3 lots rest) to hit max_daily_cancels_per_contract=2.
        exchange.call<void (*)(double)>("asterion_fake_trader_quote_price", 3500.0);
        auto recovered = quote();
        recovered.last = Decimal::parse("3500");
        session.advance_quotes({recovered});
        for (int i = 1; i <= 2; ++i) {
            const auto oid = "ord-rest-" + std::to_string(i);
            act(session, submit(oid, "3", "3500"));
            wait_for(session, [&](const Json& s) {
                return order_has_status(s, oid, "accepted");
            });
            act(session,
                {{"request_id", "req-can-" + std::to_string(i)},
                    {"action", "cancel"},
                    {"order_id", oid}});
            wait_for(session, [&](const Json& s) {
                return order_has_status(s, oid, "cancelled");
            });
        }
        EXPECT_EQ(
            session.snapshot().at("risk_breaches"),
            Json::array({"daily_cancels:SHFE.rb2610"}));
        EXPECT_THROW(act(session, submit("ord-blocked", "1", "3500")), std::invalid_argument);

        // 4) Settle day 1 down 26,000 from cashflow-adjusted peak (900,000 -> 874,000) and roll to 20260929:
        // Daily loss and cancel cap clear on the new day, while multi-day drawdown (26,000 >= 25,000) persists.
        exchange.call<void (*)(double, double, double, double, double)>(
            "asterion_fake_trader_funds",
            1000000.0,
            874000.0,
            100000.0,
            200000.0,
            3500.0);
        wait_for(session, [](const Json& s) {
            return s.contains("funds") && decimal(s.at("funds").at("balance")) == "874000";
        });
        exchange.call<void (*)(const char*)>("asterion_fake_trader_next_day", "20260929");
        const auto day2 = wait_for(session, [](const Json& s) {
            return s.at("phase") == "ready" && s.at("trading_day") == "2026-09-29"
                && s.at("positions_reconciled") == true;
        });
        EXPECT_EQ(day2.at("risk_breaches"), Json::array({"drawdown"}));
        EXPECT_EQ(decimal(day2.at("funds").at("peak_equity")), "900000");
    }

    // 5) Restart process/session on 20260929: authorization is cleared; after re-authorizing,
    // persisted peak_equity (900,000) still blocks new opens via drawdown while allowing closes.
    LiveSession reopened(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
    EXPECT_FALSE(reopened.snapshot().contains("authorization"));
    const auto after_restart = ready(reopened);
    wait_for(reopened, [](const Json& s) {
        return s.at("trading_day") == "2026-09-29" && s.at("positions_reconciled") == true;
    });
    EXPECT_FALSE(after_restart.contains("authorization"));
    const auto synced_restart = reopened.snapshot();
    EXPECT_EQ(synced_restart.at("risk_breaches"), Json::array({"drawdown"}));
    EXPECT_EQ(decimal(synced_restart.at("funds").at("peak_equity")), "900000");
    act(reopened, authorize("auth-2"));
    EXPECT_THROW(act(reopened, submit("ord-day2-open", "1", "3500")), std::invalid_argument);
    act(reopened, close_order("ord-day2-close", "close_yesterday", "1", "3500"));
    wait_for(reopened, [](const Json& s) {
        return order_has_status(s, "ord-day2-close", "filled");
    });
}

TEST_F(
    StrategyTarget,
    RiskStaysActiveDuringFundsRefreshZeroLastUsesPreSettlementAndSelfTradesRecordBothLegs)
{
    auto m = manifest(4);
    m["policy"]["max_daily_loss"] = "15000";
    m["policy"]["contracts"].push_back(contract("cu2611", "cu", "2026-11"));

    {
        exchange.call<void (*)(double, double, double, double, double)>(
            "asterion_fake_trader_funds",
            1000000.0,
            980000.0,
            0.0,
            0.0,
            10000.0);
        LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, m);
        ASSERT_EQ(ready(session).at("phase"), "ready");
        act(session, authorize("auth-refresh"));

        // Hold funds refresh while daily_loss (20,000 >= 15,000) is breached; opening cu2611 must
        // stay blocked during the refresh instead of seeing missing funds.
        exchange.call<void (*)(int)>("asterion_fake_trader_hold_funds", 1);
        exchange.call<void (*)(double, double, double, double, double)>(
            "asterion_fake_trader_funds",
            1000000.0,
            979000.0,
            0.0,
            0.0,
            10000.0);
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!exchange.call<int (*)()>("asterion_fake_trader_pending_funds")
            && std::chrono::steady_clock::now() < deadline) {
            clock->advance(250ms);
            std::this_thread::sleep_for(5ms);
        }
        ASSERT_GT(exchange.call<int (*)()>("asterion_fake_trader_pending_funds"), 0);
        auto cu_open = submit("cu-during-refresh", "1", "3500");
        cu_open["symbol"] = "cu2611";
        EXPECT_THROW(act(session, cu_open), std::invalid_argument);
        exchange.call<void (*)(int)>("asterion_fake_trader_hold_funds", 0);

        // Recover funds above the loss limit; verify LastPrice == 0.0 falls back to pre_settlement
        // (3490) and self-trade legs sharing one TradeID on opposite sides both reconcile and record.
        exchange.call<void (*)(double, double, double, double, double)>(
            "asterion_fake_trader_funds",
            1000000.0,
            1000000.0,
            0.0,
            0.0,
            10000.0);
        wait_for(session, [](const Json& s) {
            return s.contains("funds") && decimal(s.at("funds").at("balance")) == "1000000";
        });

        exchange.call<void (*)(int)>("asterion_fake_trader_quote_mode", 10);
        act(session, submit("preopen-buy", "3", "3489"));
        wait_for(session, [](const Json& s) {
            return order_has_status(s, "preopen-buy", "accepted");
        });
        auto sell_open = submit("preopen-sell", "3", "3490");
        sell_open["side"] = "sell";
        act(session, sell_open);
        wait_for(session, [](const Json& s) {
            return order_has_status(s, "preopen-sell", "accepted");
        });
        exchange.call<void (*)(int)>("asterion_fake_trader_quote_mode", 0);

        exchange.call<void (*)(int, int, int)>("asterion_fake_trader_self_trade", 0, 1, 1);
        const auto reconciled = wait_for(session, [](const Json& s) {
            return s.at("trades").size() == 2 && s.at("positions_reconciled") == true;
        });
        EXPECT_EQ(reconciled.at("trades").size(), 2U);
        const auto recorded_day = wait_for(session, [&](const Json&) {
            try {
                return session.day("2026-09-28", 0, 200, true).trades_size() == 2;
            } catch (const Error&) {
                return false;
            }
        });
        (void)recorded_day;
        const auto exported = session.day("2026-09-28", 0, 200, true);
        ASSERT_EQ(exported.trades_size(), 2);
        EXPECT_EQ(exported.trades(0).id(), exported.trades(1).id());
        EXPECT_NE(exported.trades(0).side(), exported.trades(1).side());
    }

    // Fail-closed when capital limits are configured and the broker returned no funds snapshot.
    exchange.reset();
    exchange.call<void (*)(int)>("asterion_fake_trader_empty_funds", 1);
    Directory empty_dir, empty_owners;
    LiveSession empty_session(empty_dir.path, ASTERION_TEST_CTP_TRADER, empty_owners.path, m);
    ASSERT_EQ(ready(empty_session).at("phase"), "ready");
    act(empty_session, authorize("auth-empty"));
    EXPECT_THROW(act(empty_session, submit("blocked-no-funds", "1", "3500")), Error);
}

TEST_F(StrategyTarget, FlatteningFallsBackOnEmptyOppositeBookAndRetriesAfterPartialFillCancel)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest(4));
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize("auth-flat"));
    auto owner = session.register_strategy().get();

    // Open 3 lots (rests on fake trader, then manually filled).
    session.accept_target(owner, lots(3), {quote()});
    ASSERT_EQ(progress(session, owner, resting(1)).at("orders").size(), 1U);
    exchange.call<void (*)(int, int)>("asterion_fake_trader_fill", 0, 3);
    ASSERT_EQ(held(progress(session, owner, filled(1))), "3");
    session.advance_target(owner).get();

    // Locked limit-down quote: bid absent & bid_quantity=0; flatten falls back to permissible bound.
    auto locked = quote();
    locked.bid.reset();
    locked.bid_quantity = 0;
    session.flatten_strategy(owner, {locked});
    ASSERT_EQ(progress(session, owner, resting(2)).at("orders").size(), 2U);
    const auto resting_flat = session.snapshot();
    EXPECT_EQ(resting_flat.at("orders")[1].at("side"), "sell");
    EXPECT_EQ(decimal(resting_flat.at("orders")[1].at("limit_price")), "3430");

    // Partial fill of 1 lot followed by cancel of remaining 2 lots -> flatten retries remaining 2.
    exchange.call<void (*)(int, int)>("asterion_fake_trader_fill", 1, 1);
    wait_for(session, [](const Json& s) {
        return s.at("orders").size() == 2 && decimal(s.at("orders")[1].at("filled")) == "1";
    });
    const auto partial_id = session.snapshot().at("orders")[1].at("id").get<std::string>();
    act(session,
        {{"request_id", "cancel-partial-flat"}, {"action", "cancel"}, {"order_id", partial_id}});
    wait_for(session, [](const Json& s) {
        return s.at("orders")[1].at("status") == "cancelled";
    });
    const auto done = progress(session, owner, [](const Json& s) {
        return s.at("orders").size() == 3 && s.at("orders")[2].at("status") == "filled"
            && held(s) == "0";
    });
    EXPECT_EQ(decimal(done.at("orders")[2].at("quantity")), "2");
    EXPECT_EQ(held(done), "0");
    EXPECT_TRUE(session.strategy_failure(owner).empty());
}

TEST_F(StrategyTarget, AggressiveTicksCrossSpreadClampToExchangeLimitAndRespectDeviation)
{
    using asterion::trading::StrategyExecutionOptions;
    {
        LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest(4));
        ASSERT_EQ(ready(session).at("phase"), "ready");
        act(session, authorize("auth-agg"));
        StrategyExecutionOptions exec;
        exec.aggressive_ticks = 2;
        auto owner =
            session.register_strategy("agg-run", {{"SHFE", "rb2610"}}, std::nullopt, exec).get();

        // Buy 1 lot with ask=3501, tick=1, aggressive_ticks=2 -> limit price 3503.
        session.accept_target(owner, lots(1), {quote()});
        const auto after_buy = progress(session, owner, filled(1));
        EXPECT_EQ(decimal(after_buy.at("orders")[0].at("limit_price")), "3503");
        session.advance_target(owner).get();

        // Quote near upper limit (ask=3699, upper_limit=3700) clamps to 3700 without exceeding limit.
        auto near_limit = quote();
        near_limit.last = Decimal::parse("3699");
        near_limit.ask = Decimal::parse("3699");
        near_limit.bid = Decimal::parse("3698");
        near_limit.upper_limit = Decimal::parse("3700");
        session.accept_target(owner, lots(2, 2), {near_limit});
        const auto clamped = progress(session, owner, filled(2));
        EXPECT_EQ(decimal(clamped.at("orders")[1].at("limit_price")), "3700");
        EXPECT_TRUE(session.strategy_failure(owner).empty());
    }
    // Tight max_price_deviation (0.001 => +/-3.5 on 3500): aggressive_ticks=5 yields 3506 -> rejected.
    Directory tight_dir, tight_owners;
    auto tight_manifest = manifest(4);
    tight_manifest["policy"]["max_price_deviation"] = "0.001";
    LiveSession tight(tight_dir.path, ASTERION_TEST_CTP_TRADER, tight_owners.path, tight_manifest);
    ASSERT_EQ(ready(tight).at("phase"), "ready");
    act(tight, authorize("auth-tight"));
    StrategyExecutionOptions agg5;
    agg5.aggressive_ticks = 5;
    auto owner = tight.register_strategy("agg-dev", {{"SHFE", "rb2610"}}, std::nullopt, agg5).get();
    const auto before_orders = tight.snapshot().at("orders").size();
    tight.accept_target(owner, lots(1), {quote()});
    tight.advance_target(owner, {quote()}).get();
    EXPECT_EQ(tight.snapshot().at("orders").size(), before_orders);
    EXPECT_TRUE(tight.strategy_failure(owner).empty());
}

TEST_F(StrategyTarget, BoundedRepriceCancelsRestingOrderOnAdverseMoveAndRespectsCancelLimit)
{
    using asterion::trading::StrategyExecutionOptions;
    using asterion::trading::strategy_execution_window;
    auto m = manifest(4);
    m["policy"]["max_daily_cancels_per_contract"] = 2;
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, m);
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize("auth-reprice"));

    StrategyExecutionOptions exec;
    exec.max_reprices = 2;
    exec.reprice_interval_ms = 2000;
    auto owner =
        session.register_strategy("reprice-run", {{"SHFE", "rb2610"}}, std::nullopt, exec).get();

    // Fake trader leaves 3-lot buy resting at 3501.
    const auto window = strategy_execution_window(exec);
    session.accept_target(owner, lots(3), {quote()}, steady_now(*test_clock) + window);
    ASSERT_EQ(progress(session, owner, resting(1)).at("orders").size(), 1U);
    EXPECT_EQ(decimal(session.snapshot().at("orders")[0].at("limit_price")), "3501");

    // Advance past reprice_interval_ms and move opposite ask adversely to 3504.
    test_clock->advance(2500ms);
    auto moved = quote();
    moved.last = Decimal::parse("3504");
    moved.ask = Decimal::parse("3504");
    moved.bid = Decimal::parse("3503");
    session.advance_target(owner, {moved}).get();

    // First reprice cancels order 0 and submits fresh order 1 at 3504.
    const auto repriced = progress(session, owner, [](const Json& s) {
        return s.at("orders").size() == 2 && s.at("orders")[0].at("status") == "cancelled"
            && s.at("orders")[1].at("status") == "accepted";
    });
    EXPECT_EQ(decimal(repriced.at("orders")[1].at("limit_price")), "3504");

    // Daily cancel limit is 2 (1 used; 1 headroom reserved for replacement open):
    // a further adverse move must NOT cancel order 1.
    test_clock->advance(2500ms);
    auto moved2 = quote();
    moved2.last = Decimal::parse("3506");
    moved2.ask = Decimal::parse("3506");
    moved2.bid = Decimal::parse("3505");
    session.advance_target(owner, {moved2}).get();
    EXPECT_EQ(session.snapshot().at("orders").size(), 2U);
    EXPECT_EQ(session.snapshot().at("orders")[1].at("status"), "accepted");

    // Fill the resting repriced order to finish cleanly.
    exchange.call<void (*)(int, int)>("asterion_fake_trader_fill", 1, 3);
    ASSERT_EQ(held(progress(session, owner, filled(2))), "3");
    EXPECT_TRUE(session.strategy_failure(owner).empty());
}

TEST_F(StrategyTarget, MiniTwapSlicesTargetSequentiallyOverInterval)
{
    using asterion::trading::StrategyExecutionOptions;
    using asterion::trading::strategy_execution_window;
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest(10));
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize("auth-twap"));

    StrategyExecutionOptions exec;
    exec.twap_slices = 3;
    exec.twap_interval_ms = 10000;
    auto owner =
        session.register_strategy("twap-run", {{"SHFE", "rb2610"}}, std::nullopt, exec).get();

    // Target 5 lots over 3 slices -> ceil(5/3)=2, ceil(3/2)=2, ceil(1/1)=1.
    const auto window = strategy_execution_window(exec);
    session.accept_target(owner, lots(5), {quote()}, steady_now(*test_clock) + window);

    // Slice 1 (2 lots) executes immediately and fills on fake trader (<= 2 lots auto-fill).
    const auto s1 = progress(session, owner, filled(1));
    EXPECT_EQ(decimal(s1.at("orders")[0].at("quantity")), "2");
    EXPECT_EQ(held(s1), "2");

    // Before twap_interval_ms (10s) elapses, advancing sends no second slice.
    test_clock->advance(500ms);
    session.advance_target(owner, {quote()}).get();
    EXPECT_EQ(session.snapshot().at("orders").size(), 1U);

    // After interval elapses, slice 2 (2 lots) sends and fills.
    test_clock->advance(10000ms);
    session.advance_target(owner, {quote()}).get();
    const auto s2 = progress(session, owner, filled(2));
    EXPECT_EQ(decimal(s2.at("orders")[1].at("quantity")), "2");
    EXPECT_EQ(held(s2), "4");

    // After second interval elapses, slice 3 (1 lot) sends and completes target.
    test_clock->advance(10000ms);
    session.advance_target(owner, {quote()}).get();
    const auto s3 = progress(session, owner, filled(3));
    EXPECT_EQ(decimal(s3.at("orders")[2].at("quantity")), "1");
    EXPECT_EQ(held(s3), "5");
    EXPECT_TRUE(session.strategy_failure(owner).empty());
}

TEST_F(StrategyTarget, DeliveryMonthAbandonsContractWithoutStoppingInstance)
{
    auto m = manifest(4);
    m["policy"]["contracts"].push_back(contract("rb2701", "rb", "2027-01"));
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, m);
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());

    // Enter rb2610's delivery month (2026-10); rb2701 remains openable.
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

    auto owner =
        session.register_strategy("guard-run", {{"SHFE", "rb2610"}, {"SHFE", "rb2701"}}).get();
    ::strategy::Decision decision;
    decision.set_sequence(1);
    decision.set_ready(true);
    for (const char* sym : {"rb2610", "rb2701"}) {
        auto* target = decision.add_targets();
        target->mutable_instrument()->set_exchange("SHFE");
        target->mutable_instrument()->set_symbol(sym);
        target->set_lots(1);
    }
    auto q_delivery = quote();
    q_delivery.trading_day = "20261009";
    auto q_active = q_delivery;
    q_active.instrument = {"SHFE", "rb2701"};
    session.accept_target(owner, decision, {q_delivery, q_active});

    // rb2610 is abandoned by the delivery-month gate while rb2701 fills cleanly.
    const auto state = progress(session, owner, filled(1));
    EXPECT_TRUE(session.strategy_failure(owner).empty());
    ASSERT_EQ(state.at("orders").size(), 1U);
    EXPECT_EQ(state.at("orders")[0].at("symbol"), "rb2701");
    EXPECT_EQ(state.at("orders")[0].at("status"), "filled");
    const auto events = session.strategy_events(owner);
    EXPECT_TRUE(std::ranges::any_of(events, [](const auto& ev) {
        return ev.text().find("delivery month") != std::string::npos;
    }));
}
