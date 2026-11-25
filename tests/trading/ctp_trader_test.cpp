#include "ctp_support.hpp"
#include "ctp_trader.hpp"
#include "ctp_reconciliation.hpp"
#include <cstring>
#include <asterion/base/error.hpp>
#include <asterion/system/process/child.hpp>
#include <asterion/system/environment.hpp>
#include <asterion/system/rpc_host.hpp>
#include <gtest/gtest.h>
#include <thread>
#include <future>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using namespace asterion;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

Decimal d(const char* text)
{
    return Decimal::parse(text);
}

// Test control entry points exported by the SDK double.
struct FakeControl
{
    void reset()
    {
        reset_library.symbol<void (*)()>()();
    }

    void reconnect()
    {
        reconnect_library.symbol<void (*)()>()();
    }

    int query_rejections()
    {
        return rejections_library.symbol<int (*)()>()();
    }

    void set_clock(const Clock* clock)
    {
        clock_library.symbol<void (*)(
            std::int64_t (*)(void*),
            std::int64_t (*)(void*),
            void*)>()(
            clock ? +[](void* ctx) { return static_cast<const Clock*>(ctx)->monotonic_now(); }
                  : nullptr,
            clock ? +[](void* ctx) { return static_cast<const Clock*>(ctx)->utc_now(); }
                  : nullptr,
            const_cast<Clock*>(clock));
    }

    template <class F, class... Args>
    auto call(const char* name, Args... args)
    {
        ctp::SharedLibrary control(ASTERION_TEST_CTP_TRADER, name);
        return control.symbol<F>()(args...);
    }

    ctp::SharedLibrary reset_library{ASTERION_TEST_CTP_TRADER, "asterion_fake_trader_reset"};
    ctp::SharedLibrary reconnect_library{
        ASTERION_TEST_CTP_TRADER,
        "asterion_fake_trader_reconnect"
    };
    ctp::SharedLibrary rejections_library{
        ASTERION_TEST_CTP_TRADER,
        "asterion_fake_trader_query_rejections"
    };
    ctp::SharedLibrary clock_library{ASTERION_TEST_CTP_TRADER, "asterion_fake_trader_set_clock"};
};

ctp::TraderConfiguration
configuration(std::string password = "secret", std::string auth = "auth-code")
{
    return {"tcp://127.0.0.1:41205", "9999", "000001", std::move(password), "client_app", auth};
}

LimitOrder order(
    std::string id,
    std::string venue,
    std::string symbol,
    Side side,
    const char* quantity,
    const char* price)
{
    return {std::move(id), {std::move(venue), std::move(symbol)}, side, d(quantity), d(price)};
}

bool ready(const BrokerSnapshot& s)
{
    return s.phase == BrokerPhase::ready;
}

const BrokerOrder* find(const BrokerSnapshot& s, const std::string& id)
{
    for (const auto& o : s.orders)
        if (o.order_id == id)
            return &o;
    return nullptr;
}

std::future<BrokerDispatchResult> dispatch_with_permission(
    ctp::Trader& trader,
    BrokerSendGate& gate,
    std::unique_ptr<PreparedBrokerOrder> prepared)
{
    static std::uint64_t sequence = 1;
    auto permit = gate.issue(gate.revision(), prepared->order().order_id);
    return trader.dispatch(std::move(prepared), std::move(permit), ++sequence);
}

class CtpTrader : public ::testing::Test
{
protected:
    void SetUp() override
    {
        fake.reset();
        fake.set_clock(clock.get());
    }

    void TearDown() override
    {
        fake.set_clock(nullptr);
        std::error_code e;
        fs::remove_all(flow, e);
    }

    auto dispatch_deadline() const
    {
        return steady_now(*clock) + 10s;
    }

    template <class F>
    BrokerSnapshot wait_for(ctp::Trader& trader, F done, auto timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        trader.poll();
        auto state = trader.snapshot();
        while (!done(state) && std::chrono::steady_clock::now() < deadline) {
            clock->advance(250ms);
            trader.poll();
            state = trader.snapshot();
            if (done(state))
                break;
            std::this_thread::sleep_for(5ms);
        }
        return state;
    }

    template <class T>
    T await(ctp::Trader& trader, std::future<T> result)
    {
        const auto deadline = std::chrono::steady_clock::now() + 15s;
        while (result.wait_for(0s) != std::future_status::ready) {
            clock->advance(250ms);
            trader.poll();
            if (result.wait_for(0s) == std::future_status::ready)
                break;
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("test SDK result did not complete");
            std::this_thread::sleep_for(5ms);
        }
        trader.poll();
        return result.get();
    }

    void cancel(ctp::Trader& trader, const std::string& order_id)
    {
        const auto result = await(trader, trader.cancel(order_id));
        EXPECT_TRUE(result.invoked);
        EXPECT_EQ(result.code, 0);
    }

    BrokerOrder send(
        ctp::Trader& trader,
        BrokerSendGate& send_gate,
        const LimitOrder& item,
        Offset offset,
        std::uint64_t generation,
        std::uint64_t exposure,
        std::chrono::steady_clock::time_point deadline)
    {
        auto prepared = trader.prepare(item, offset, generation, exposure, deadline);
        const auto pending = prepared->order();
        auto result = dispatch_with_permission(trader, send_gate, std::move(prepared));
        EXPECT_TRUE(await(trader, std::move(result)).invoked);
        trader.poll();
        const auto state = trader.snapshot();
        if (const auto* reported = find(state, pending.order_id))
            return *reported;
        return pending;
    }

    FakeControl fake;
    BrokerSendGate gate;
    std::shared_ptr<ManualClock> clock = std::make_shared<ManualClock>(1'790'557'200'000'000'000LL);
    fs::path flow = fs::temp_directory_path() / ("asterion-ctp-trader-" + unique_process_id());
};

struct Socket
{
    explicit Socket(int value)
        : fd(value)
    {}

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    ~Socket()
    {
        if (fd >= 0)
            ::close(fd);
    }

    int fd;
};

struct VendorFlow
{
    ~VendorFlow()
    {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }

    fs::path path = fs::temp_directory_path() / ("asterion-vendor-trader-" + unique_process_id());
};

} // namespace

TEST(CtpVendor, TraderLoopbackLifecycleUsesThePackagedAbi)
{
    // Match trading/market service initialization, including signal policy.
    service::install_stop_signals();
#ifdef __APPLE__
    const char* filename = "ctp-trader.dylib";
#else
    const char* filename = "ctp-trader.so";
#endif
    const auto library = environment_path("ASTERION_VENDOR_CTP_TRADER_LIBRARY")
                             .value_or(current_executable().parent_path() / filename);
    ASSERT_TRUE(fs::is_regular_file(library));
    ctp::SharedLibrary version(library, "_ZN19CThostFtdcTraderApi13GetApiVersionEv");
    const auto api_version = version.symbol<const char* (*)()>();
    ASSERT_NE(api_version, nullptr);
    ASSERT_NE(api_version(), nullptr);
    ASSERT_NE(std::string(api_version()).find("6.7.7"), std::string::npos);

    // Only this owned loopback listener is reachable. No CTP protocol replies,
    // authentication or orders are supplied by the fixture.
    Socket listener(::socket(AF_INET, SOCK_STREAM, 0));
    ASSERT_GE(listener.fd, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(::bind(listener.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    socklen_t length = sizeof(address);
    ASSERT_EQ(::getsockname(listener.fd, reinterpret_cast<sockaddr*>(&address), &length), 0);
    ASSERT_EQ(::listen(listener.fd, 2), 0);
    const auto front = "tcp://127.0.0.1:" + std::to_string(ntohs(address.sin_port));
    VendorFlow flow;
    {
        BrokerSendGate gate;
        ctp::Trader trader(library, flow.path, gate, [] {});
        trader.start();
        for (int attempt = 0; attempt < 2; ++attempt) {
            SCOPED_TRACE(attempt);
            trader.connect({front, "test", "fixture", "loopback-only", "test_app", "test_auth"});
            pollfd incoming{listener.fd, POLLIN, 0};
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            while (::poll(&incoming, 1, 0) == 0 && std::chrono::steady_clock::now() < deadline) {
                trader.poll();
                std::this_thread::sleep_for(10ms);
            }
            ASSERT_EQ(::poll(&incoming, 1, 0), 1);
            ASSERT_NE(incoming.revents & POLLIN, 0);
            Socket peer(::accept(listener.fd, nullptr, nullptr));
            ASSERT_GE(peer.fd, 0);
            EXPECT_NE(trader.snapshot().phase, BrokerPhase::ready);
            EXPECT_TRUE(trader.snapshot().orders.empty());
            trader.disconnect();
            EXPECT_EQ(trader.snapshot().phase, BrokerPhase::disconnected);
        }
    } // SDK Release and its worker complete before unloading the real library.
}

TEST(BrokerSendGate, PermissionIsBoundToOneOrderAndOneInvalidationRevision)
{
    BrokerSendGate gate;
    auto old = gate.issue(gate.revision(), "old");
    EXPECT_THROW(gate.issue(gate.revision(), "parallel"), Error);
    gate.invalidate();
    auto current = gate.issue(gate.revision(), "current");
    old = BrokerSendPermit{};
    EXPECT_TRUE(current.consume("current")) << "retiring an old grant cannot close a new one";
    EXPECT_FALSE(current.consume("current"));
    auto wrong = gate.issue(gate.revision(), "expected");
    EXPECT_FALSE(wrong.consume("different"));
    const auto previous = gate.revision();
    gate.invalidate();
    EXPECT_THROW(gate.issue(previous, "stale"), Error);
    gate.pending();
    const auto pending = gate.revision();
    EXPECT_THROW(gate.issue(pending, "unapplied"), Error);
    gate.invalidate();
    gate.acknowledge(pending);
    EXPECT_THROW(gate.issue(gate.revision(), "old-acknowledgement"), Error);
    gate.acknowledge(gate.revision());
    EXPECT_TRUE(gate.issue(gate.revision(), "applied").consume("applied"));
}

TEST_F(CtpTrader, QueuedReportsInvalidatePermissionBeforeTheOwnerAppliesThem)
{
    ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {}, clock);
    trader.connect(configuration());
    const auto initial = wait_for(trader, ready, 15s);
    ASSERT_EQ(initial.phase, BrokerPhase::ready);
    send(
        trader,
        gate,
        order("working", "SHFE", "rb2610", Side::buy, "3", "3500"),
        Offset::open,
        initial.connection_generation,
        initial.exposure_revision,
        dispatch_deadline());
    const auto before = wait_for(
        trader,
        [](const BrokerSnapshot& state) {
            const auto* entry = find(state, "working");
            return entry && entry->status == BrokerOrderStatus::accepted;
        },
        2s);
    ASSERT_NE(find(before, "working"), nullptr);
    const auto revision = gate.revision();
    auto permit = gate.issue(revision, "next");
    fake.call<void (*)(int, int)>("asterion_fake_trader_replay_order", 0, 0);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (gate.revision() == revision && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    ASSERT_GT(gate.revision(), revision);
    EXPECT_EQ(trader.snapshot().sequence, before.sequence);
    EXPECT_FALSE(permit.consume("next"));
    EXPECT_THROW(gate.issue(gate.revision(), "unapplied"), Error);
    EXPECT_THROW(
        trader.disconnect_checked(before.connection_generation, before.exposure_revision),
        Error);
    EXPECT_EQ(trader.snapshot().phase, BrokerPhase::ready);
    trader.poll();
    EXPECT_TRUE(gate.issue(gate.revision(), "applied").consume("applied"));
    const auto applied = trader.snapshot();
    EXPECT_NO_THROW(
        trader.disconnect_checked(applied.connection_generation, applied.exposure_revision));
}

TEST_F(CtpTrader, CallbackOverflowRequiresReconciliationAndStillAllowsCancellation)
{
    ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {}, clock);
    trader.connect(configuration());
    const auto initial = wait_for(trader, ready, 15s);
    ASSERT_EQ(initial.phase, BrokerPhase::ready);
    send(
        trader,
        gate,
        order("working", "SHFE", "rb2610", Side::buy, "3", "3500"),
        Offset::open,
        initial.connection_generation,
        initial.exposure_revision,
        dispatch_deadline());
    ASSERT_NE(
        find(
            wait_for(
                trader,
                [](const BrokerSnapshot& state) {
                    const auto* entry = find(state, "working");
                    return entry && entry->status == BrokerOrderStatus::accepted;
                },
                2s),
            "working"),
        nullptr);
    const auto revision = gate.revision();
    fake.call<void (*)(int)>("asterion_fake_trader_report_burst", 1100);
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (gate.revision() < revision + 1100 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    ASSERT_GE(gate.revision(), revision + 1100);
    EXPECT_THROW(gate.issue(gate.revision(), "unapplied"), Error);
    trader.poll();
    EXPECT_EQ(trader.snapshot().phase, BrokerPhase::error);
    EXPECT_EQ(trader.snapshot().error_code, broker_code::callback_overflow);
    EXPECT_FALSE(trader.snapshot().positions_reconciled);
    EXPECT_THROW(gate.issue(gate.revision(), "lost-report"), Error);
    cancel(trader, "working");
    const auto cancelled = wait_for(
        trader,
        [](const BrokerSnapshot& state) {
            const auto* entry = find(state, "working");
            return entry && entry->status == BrokerOrderStatus::cancelled;
        },
        2s);
    ASSERT_NE(find(cancelled, "working"), nullptr);
    EXPECT_EQ(find(cancelled, "working")->status, BrokerOrderStatus::cancelled);
    EXPECT_THROW(gate.issue(gate.revision(), "still-unreconciled"), Error);
    trader.connect(configuration());
    ASSERT_EQ(wait_for(trader, ready, 15s).phase, BrokerPhase::ready);
    EXPECT_TRUE(gate.issue(gate.revision(), "reconciled").consume("reconciled"));
}

TEST_F(CtpTrader, DisconnectRetainsAlreadyReceivedOrderAndTradeFacts)
{
    ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {}, clock);
    trader.connect(configuration());
    const auto initial = wait_for(trader, ready, 15s);
    ASSERT_EQ(initial.phase, BrokerPhase::ready);
    send(
        trader,
        gate,
        order("working", "SHFE", "rb2610", Side::buy, "3", "3500"),
        Offset::open,
        initial.connection_generation,
        initial.exposure_revision,
        dispatch_deadline());
    const auto accepted = wait_for(
        trader,
        [](const BrokerSnapshot& state) {
            const auto* entry = find(state, "working");
            return entry && entry->status == BrokerOrderStatus::accepted;
        },
        2s);
    ASSERT_NE(find(accepted, "working"), nullptr);
    const auto revision = gate.revision();
    fake.call<void (*)(int, int)>("asterion_fake_trader_fill", 0, 1);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (gate.revision() < revision + 2 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    ASSERT_GE(gate.revision(), revision + 2);
    EXPECT_TRUE(trader.snapshot().trades.empty()) << "only the owner may apply the queued facts";
    trader.disconnect();
    trader.poll();
    const auto closed = trader.snapshot();
    EXPECT_EQ(closed.phase, BrokerPhase::disconnected);
    ASSERT_NE(find(closed, "working"), nullptr);
    EXPECT_EQ(find(closed, "working")->filled, d("1"));
    ASSERT_EQ(closed.trades.size(), 1U);
    EXPECT_EQ(closed.trades.front().quantity, d("1"));
}

TEST_F(CtpTrader, RevokedPermissionNeverInvokesTheSdkOrInventsABrokerOrder)
{
    ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {}, clock);
    trader.connect(configuration());
    const auto state = wait_for(trader, ready, 15s);
    ASSERT_EQ(state.phase, BrokerPhase::ready);
    auto prepared = trader.prepare(
        order("revoked", "SHFE", "rb2610", Side::buy, "1", "3500"),
        Offset::open,
        state.connection_generation,
        state.exposure_revision,
        dispatch_deadline());
    auto permit = gate.issue(gate.revision(), "revoked");
    const auto effects = fake.call<int (*)()>("asterion_fake_catalog_side_effects");
    gate.invalidate();
    const auto result = await(trader, trader.dispatch(std::move(prepared), std::move(permit), 1));
    EXPECT_FALSE(result.invoked);
    EXPECT_EQ(result.code, broker_code::permit_refused);
    EXPECT_TRUE(trader.snapshot().orders.empty());
    EXPECT_EQ(fake.call<int (*)()>("asterion_fake_catalog_side_effects"), effects);
}

TEST_F(CtpTrader, RefusedOrderDoesNotConsumeOrderSpacing)
{
    ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {}, clock);
    trader.connect(configuration());
    const auto state = wait_for(trader, ready, 15s);
    ASSERT_EQ(state.phase, BrokerPhase::ready);
    auto refused = trader.prepare(
        order("revoked", "SHFE", "rb2610", Side::buy, "1", "3500"),
        Offset::open,
        state.connection_generation,
        state.exposure_revision,
        dispatch_deadline());
    auto permit = gate.issue(gate.revision(), "revoked");
    gate.invalidate();
    const auto first = await(trader, trader.dispatch(std::move(refused), std::move(permit), 1));
    ASSERT_FALSE(first.invoked);
    ASSERT_EQ(first.code, broker_code::permit_refused);
    auto next = trader.prepare(
        order("sent", "SHFE", "rb2610", Side::buy, "1", "3500"),
        Offset::open,
        state.connection_generation,
        state.exposure_revision,
        dispatch_deadline());
    auto sending = dispatch_with_permission(trader, gate, std::move(next));
    // No clock advance: an uninvoked refusal must not hold back the next order.
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (sending.wait_for(0s) != std::future_status::ready
        && std::chrono::steady_clock::now() < deadline) {
        trader.poll();
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_EQ(sending.wait_for(0s), std::future_status::ready);
    const auto second = sending.get();
    EXPECT_TRUE(second.invoked);
    EXPECT_EQ(second.code, 0);
}

TEST_F(CtpTrader, SdkLifecycleHasOneOwnerAndBlockedReleaseDoesNotBlockControl)
{
    {
        ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {}, clock);

        // Unblock Release before Trader destruction even if an assertion fails.
        struct ReleaseGuard
        {
            ~ReleaseGuard()
            {
                fake.call<void (*)(int)>("asterion_fake_trader_hold_release", 0);
            }

            FakeControl& fake;
        } release{fake};

        trader.connect(configuration());
        const auto initial = wait_for(trader, ready, 15s);
        ASSERT_EQ(initial.phase, BrokerPhase::ready);
        auto pending = send(
            trader,
            gate,
            order("resting", "SHFE", "rb2610", Side::buy, "3", "3500"),
            Offset::open,
            initial.connection_generation,
            initial.exposure_revision,
            dispatch_deadline());
        const auto accepted = wait_for(
            trader,
            [](const BrokerSnapshot& state) {
                const auto* entry = find(state, "resting");
                return entry && entry->status == BrokerOrderStatus::accepted;
            },
            5s);
        ASSERT_NE(find(accepted, "resting"), nullptr);
        ASSERT_EQ(find(accepted, "resting")->status, BrokerOrderStatus::accepted);
        cancel(trader, pending.order_id);
        ASSERT_TRUE(await(trader, trader.quote({"SHFE", "rb2610"})));
        fake.call<void (*)(int)>("asterion_fake_trader_hold_release", 1);
        trader.disconnect();
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!fake.call<int (*)()>("asterion_fake_trader_sdk_releases")
            && std::chrono::steady_clock::now() < deadline) {
            trader.poll();
            std::this_thread::sleep_for(10ms);
        }
        ASSERT_EQ(fake.call<int (*)()>("asterion_fake_trader_sdk_releases"), 1);
        EXPECT_EQ(trader.snapshot().phase, BrokerPhase::disconnected);

        auto replacement = std::async(std::launch::async, [&] {
            trader.connect(configuration());
        });
        const auto responsive = replacement.wait_for(1s);
        if (responsive != std::future_status::ready)
            fake.call<void (*)(int)>("asterion_fake_trader_hold_release", 0);
        replacement.get();
        ASSERT_EQ(responsive, std::future_status::ready);
        EXPECT_EQ(trader.snapshot().phase, BrokerPhase::connecting);
        EXPECT_EQ(fake.call<int (*)()>("asterion_fake_trader_sdk_creations"), 1);
        // A disconnect cancels the waiting replacement before it creates an SDK.
        trader.disconnect();
        EXPECT_EQ(trader.snapshot().phase, BrokerPhase::disconnected);
        fake.call<void (*)(int)>("asterion_fake_trader_hold_release", 0);
        trader.connect(configuration());
        const auto restored = wait_for(trader, ready, 15s);
        ASSERT_EQ(restored.phase, BrokerPhase::ready);
        EXPECT_NE(restored.connection_generation, initial.connection_generation);
        EXPECT_EQ(fake.call<int (*)()>("asterion_fake_trader_sdk_creations"), 2);
    }
    EXPECT_EQ(fake.call<int (*)()>("asterion_fake_trader_sdk_releases"), 2);
    EXPECT_EQ(fake.call<int (*)()>("asterion_fake_trader_sdk_thread_mismatches"), 0);
}

TEST_F(CtpTrader, PartialQuoteResponsesRequireSuccessfulCompletionAndCurrentIdentity)
{
    ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {}, clock);
    trader.start();
    trader.connect(configuration());
    ASSERT_EQ(wait_for(trader, ready, 15s).phase, BrokerPhase::ready);
    // A refused query or another trading day's quote is refused with its reason.
    for (const auto& [mode, reason] :
        {std::pair{1, "refused the quote query with code 7"},
            std::pair{3, "is for trading day 20260925"}}) {
        fake.call<void (*)(int)>("asterion_fake_trader_quote_mode", mode);
        try {
            (void)await(trader, trader.quote({"SHFE", "rb2610"}));
            ADD_FAILURE() << "mode " << mode << " was accepted";
        } catch (const std::invalid_argument& error) {
            EXPECT_NE(std::string(error.what()).find(reason), std::string::npos) << error.what();
        }
    }
    // A CZCE night quote under its civil date is of this session; an earlier night's is not.
    fake.call<void (*)(int)>("asterion_fake_trader_quote_mode", 8);
    const auto night = await(trader, trader.quote({"CZCE", "SR701"}));
    ASSERT_TRUE(night);
    EXPECT_EQ(night->trading_day, trader.snapshot().trading_day);
    fake.call<void (*)(int)>("asterion_fake_trader_quote_mode", 9);
    EXPECT_THROW((void)await(trader, trader.quote({"CZCE", "SR701"})), std::invalid_argument);
    // A counter may leave the exchange blank, as market data does.
    fake.call<void (*)(int)>("asterion_fake_trader_quote_mode", 6);
    const auto blank = await(trader, trader.quote({"SHFE", "rb2610"}));
    ASSERT_TRUE(blank);
    EXPECT_EQ(blank->instrument, (InstrumentId{"SHFE", "rb2610"}));
    // A prefix query also returns the contract's options; their rows are skipped.
    fake.call<void (*)(int)>("asterion_fake_trader_quote_mode", 7);
    const auto skipped = await(trader, trader.quote({"SHFE", "rb2610"}));
    ASSERT_TRUE(skipped);
    EXPECT_EQ(skipped->last, d("3500"));
    // Only another contract's row is no quote of this one.
    fake.call<void (*)(int)>("asterion_fake_trader_quote_mode", 4);
    EXPECT_FALSE(await(trader, trader.quote({"SHFE", "rb2610"})));
    fake.call<void (*)(int)>("asterion_fake_trader_quote_mode", 5);
    const auto started = steady_now(*clock);
    const auto quote = await(trader, trader.quote({"SHFE", "rb2610"}));
    ASSERT_TRUE(quote);
    EXPECT_EQ(quote->last, d("3500"));
    EXPECT_EQ(quote->trading_day, trader.snapshot().trading_day);
    EXPECT_EQ(quote->connection_generation, trader.snapshot().connection_generation);
    EXPECT_GE(quote->completed_at, started);
    EXPECT_LE(quote->completed_at, steady_now(*clock));
    // A zero LastPrice before the first trade leaves last absent for pre-settlement fallback.
    fake.call<void (*)(int)>("asterion_fake_trader_quote_mode", 10);
    const auto opening = await(trader, trader.quote({"SHFE", "rb2610"}));
    ASSERT_TRUE(opening);
    EXPECT_FALSE(opening->last);
    EXPECT_EQ(opening->pre_settlement, d("3490"));
    // An OnRspError targeting a quote fails only that quote without ending the session.
    fake.call<void (*)(int)>("asterion_fake_trader_quote_mode", 11);
    EXPECT_THROW((void)await(trader, trader.quote({"SHFE", "rb2610"})), std::invalid_argument);
    EXPECT_EQ(trader.snapshot().phase, BrokerPhase::ready);
    // An incomplete quote times out on its own and leaves the session ready.
    fake.call<void (*)(int)>("asterion_fake_trader_quote_mode", 2);
    EXPECT_FALSE(await(trader, trader.quote({"SHFE", "rb2610"})));
    EXPECT_EQ(trader.snapshot().phase, BrokerPhase::ready);
    fake.call<void (*)(int)>("asterion_fake_trader_quote_mode", 0);
    EXPECT_TRUE(await(trader, trader.quote({"SHFE", "rb2610"})));
}

TEST_F(CtpTrader, DispatchReturnsWithoutWaitingAndLateSdkResultCannotOverwriteBrokerFacts)
{
    ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {}, clock);

    struct ReturnGuard
    {
        ~ReturnGuard()
        {
            fake.call<void (*)(int, int)>("asterion_fake_trader_hold_insert_return", 0, 0);
        }

        FakeControl& fake;
    } release{fake};

    trader.connect(configuration());
    const auto state = wait_for(trader, ready, 15s);
    ASSERT_EQ(state.phase, BrokerPhase::ready);
    auto prepared = trader.prepare(
        order("early.fill", "SHFE", "rb2610", Side::buy, "1", "3500"),
        Offset::open,
        state.connection_generation,
        state.exposure_revision,
        dispatch_deadline());
    fake.call<void (*)(int, int)>("asterion_fake_trader_hold_insert_return", 1, -7);
    auto completion = dispatch_with_permission(trader, gate, std::move(prepared));
    const auto filled = wait_for(
        trader,
        [](const BrokerSnapshot& current) {
            const auto* order = find(current, "early.fill");
            return order && order->status == BrokerOrderStatus::filled;
        },
        2s);
    ASSERT_NE(find(filled, "early.fill"), nullptr);
    ASSERT_EQ(find(filled, "early.fill")->status, BrokerOrderStatus::filled);
    EXPECT_EQ(completion.wait_for(0s), std::future_status::timeout);
    EXPECT_THROW(
        trader.prepare(
            order("queued", "SHFE", "rb2610", Side::buy, "1", "3500"),
            Offset::open,
            filled.connection_generation,
            filled.exposure_revision,
            dispatch_deadline()),
        Error);
    fake.call<void (*)(int, int)>("asterion_fake_trader_hold_insert_return", 0, -7);
    const auto result = await(trader, std::move(completion));
    EXPECT_TRUE(result.invoked);
    EXPECT_EQ(result.code, -7);
    const auto current = trader.snapshot();
    ASSERT_NE(find(current, "early.fill"), nullptr);
    EXPECT_EQ(find(current, "early.fill")->status, BrokerOrderStatus::filled);
    EXPECT_EQ(find(current, "early.fill")->filled, d("1"));
}

TEST_F(CtpTrader, ExpiredSubmissionNeverDispatchesEvenIfJournalingCompletesLater)
{
    ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {}, clock);
    trader.start();
    trader.connect(configuration());
    const auto state = wait_for(trader, ready, 15s);
    ASSERT_TRUE(state.positions_reconciled);
    EXPECT_THROW(
        trader.prepare(
            order("expired", "SHFE", "rb2610", Side::buy, "4", "3500"),
            Offset::open,
            state.connection_generation,
            state.exposure_revision,
            steady_now(*clock)),
        Error);
    const auto effects = fake.call<int (*)()>("asterion_fake_catalog_side_effects");
    const auto deadline = steady_now(*clock) + 100ms;
    auto prepared = trader.prepare(
        order("slow.disk", "SHFE", "rb2610", Side::buy, "4", "3500"),
        Offset::open,
        state.connection_generation,
        state.exposure_revision,
        deadline);
    clock->advance(120ms);
    const auto result = await(trader, dispatch_with_permission(trader, gate, std::move(prepared)));
    EXPECT_FALSE(result.invoked);
    EXPECT_EQ(result.code, broker_code::expired);
    EXPECT_EQ(fake.call<int (*)()>("asterion_fake_catalog_side_effects"), effects);
    EXPECT_TRUE(trader.snapshot().orders.empty());
}

TEST_F(CtpTrader, BackToBackOrdersAndCancelsStayWithinTheCountersRequestRate)
{
    // A counter that refuses order and cancel requests less than 100 ms apart.
    fake.call<void (*)(int)>("asterion_fake_trader_order_gap", 100);
    ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {}, clock);
    trader.start();
    trader.connect(configuration());
    auto state = wait_for(trader, ready, 15s);
    ASSERT_TRUE(state.positions_reconciled);
    const std::vector<std::string> ids{"first", "second", "third"};
    for (const auto& id : ids) {
        auto prepared = trader.prepare(
            order(id, "SHFE", "rb2610", Side::buy, "4", "3500"),
            Offset::open,
            state.connection_generation,
            state.exposure_revision,
            dispatch_deadline());
        const auto result =
            await(trader, dispatch_with_permission(trader, gate, std::move(prepared)));
        EXPECT_TRUE(result.invoked);
        EXPECT_EQ(result.code, 0);
        state = wait_for(
            trader,
            [&](const BrokerSnapshot& current) {
                return find(current, id) != nullptr;
            },
            2s);
    }
    for (const auto& id : ids)
        cancel(trader, id);
}

TEST_F(CtpTrader, FilledOrderWaitsForTradeReportsAndLateOrdersCannotReduceExposure)
{
    ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {}, clock);
    trader.start();
    trader.connect(configuration());
    ASSERT_TRUE(wait_for(trader, ready, 15s).positions_reconciled);
    fake.call<void (*)(int)>("asterion_fake_trader_hold_trades", 1);
    send(
        trader,
        gate,
        order("filled", "SHFE", "rb2610", Side::buy, "2", "3500"),
        Offset::open,
        trader.snapshot().connection_generation,
        trader.snapshot().exposure_revision,
        dispatch_deadline());
    auto state = wait_for(
        trader,
        [](const BrokerSnapshot& s) {
            return find(s, "filled") && find(s, "filled")->status == BrokerOrderStatus::filled
                && !s.positions.empty();
        },
        10s);
    ASSERT_FALSE(state.positions.empty());
    EXPECT_FALSE(state.positions_reconciled) << "position rows do not replace missing fill reports";
    EXPECT_THROW(
        trader.prepare(
            order("blocked", "SHFE", "rb2610", Side::buy, "2", "3500"),
            Offset::open,
            state.connection_generation,
            state.exposure_revision,
            dispatch_deadline()),
        Error);
    const auto revision = state.exposure_revision;
    fake.call<void (*)(int, int)>("asterion_fake_trader_replay_order", 0, 0);
    state = wait_for(
        trader,
        [&](const BrokerSnapshot& s) {
            return s.exposure_revision > revision;
        },
        2s);
    ASSERT_TRUE(find(state, "filled"));
    EXPECT_EQ(find(state, "filled")->filled, d("2"));
    EXPECT_EQ(find(state, "filled")->status, BrokerOrderStatus::filled);
    EXPECT_FALSE(state.positions_reconciled);
    fake.call<void (*)(int)>("asterion_fake_trader_hold_trades", 0);
    state = wait_for(
        trader,
        [](const BrokerSnapshot& s) {
            return s.positions_reconciled;
        },
        10s);
    ASSERT_TRUE(state.positions_reconciled);
    EXPECT_EQ(state.trades.size(), 1U);
    EXPECT_EQ(state.positions[0].today, d("2"));
    // The broker's own amounts come with the lots.
    EXPECT_EQ(state.positions[0].cost, d("70000"));
    EXPECT_EQ(state.positions[0].margin, d("7000"));
}

TEST_F(CtpTrader, AdditionalFillDuringPositionQueryForcesAnotherRefreshAndAllowsCancellation)
{
    ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {}, clock);
    trader.start();
    trader.connect(configuration());
    ASSERT_TRUE(wait_for(trader, ready, 15s).positions_reconciled);
    send(
        trader,
        gate,
        order("partial", "SHFE", "rb2610", Side::buy, "5", "3500"),
        Offset::open,
        trader.snapshot().connection_generation,
        trader.snapshot().exposure_revision,
        dispatch_deadline());
    ASSERT_TRUE(find(
        wait_for(
            trader,
            [](const BrokerSnapshot& s) {
                return find(s, "partial")
                    && find(s, "partial")->status == BrokerOrderStatus::accepted;
            },
            2s),
        "partial"));
    fake.call<void (*)(int)>("asterion_fake_trader_hold_positions", 1);
    fake.call<void (*)(int, int)>("asterion_fake_trader_fill", 0, 1);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!fake.call<int (*)()>("asterion_fake_trader_pending_positions")
        && std::chrono::steady_clock::now() < deadline) {
        clock->advance(250ms);
        trader.poll();
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_GT(fake.call<int (*)()>("asterion_fake_trader_pending_positions"), 0);
    fake.call<void (*)(int, int)>("asterion_fake_trader_fill", 0, 1);
    auto state = wait_for(
        trader,
        [](const BrokerSnapshot& s) {
            return s.trades.size() == 2;
        },
        2s);
    ASSERT_EQ(state.trades.size(), 2U);
    EXPECT_FALSE(state.positions_reconciled);
    EXPECT_NO_THROW(cancel(trader, "partial"));
    fake.call<void (*)(int)>("asterion_fake_trader_hold_positions", 0);
    state = wait_for(
        trader,
        [](const BrokerSnapshot& s) {
            return s.positions_reconciled;
        },
        10s);
    ASSERT_TRUE(state.positions_reconciled);
    ASSERT_EQ(state.positions.size(), 1U);
    EXPECT_EQ(state.positions[0].today, d("2")) << "the held first query only contained one lot";
    EXPECT_EQ(find(state, "partial")->status, BrokerOrderStatus::cancelled);
}

TEST_F(CtpTrader, ExposureChangesBeforeOrDuringJournalingRejectTheEvaluatedOrder)
{
    ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {}, clock);
    trader.start();
    trader.connect(configuration());
    ASSERT_TRUE(wait_for(trader, ready, 15s).positions_reconciled);
    send(
        trader,
        gate,
        order("working", "SHFE", "rb2610", Side::buy, "5", "3500"),
        Offset::open,
        trader.snapshot().connection_generation,
        trader.snapshot().exposure_revision,
        dispatch_deadline());
    auto before = wait_for(
        trader,
        [](const BrokerSnapshot& s) {
            return find(s, "working") && find(s, "working")->status == BrokerOrderStatus::accepted;
        },
        2s);
    cancel(trader, "working");
    auto state = wait_for(
        trader,
        [](const BrokerSnapshot& s) {
            return find(s, "working") && find(s, "working")->status == BrokerOrderStatus::cancelled;
        },
        2s);
    EXPECT_THROW(
        trader.prepare(
            order("stale.risk", "SHFE", "rb2610", Side::buy, "5", "3500"),
            Offset::open,
            before.connection_generation,
            before.exposure_revision,
            dispatch_deadline()),
        Error);
    auto prepared = trader.prepare(
        order("during.risk", "SHFE", "rb2610", Side::buy, "5", "3500"),
        Offset::open,
        state.connection_generation,
        state.exposure_revision,
        dispatch_deadline());
    fake.call<void (*)(int, int)>("asterion_fake_trader_replay_order", 0, 0);
    ASSERT_GT(
        wait_for(
            trader,
            [&](const BrokerSnapshot& current) {
                return current.exposure_revision > state.exposure_revision;
            },
            2s)
            .exposure_revision,
        state.exposure_revision);
    const auto result = await(trader, dispatch_with_permission(trader, gate, std::move(prepared)));
    EXPECT_FALSE(result.invoked);
    EXPECT_EQ(result.code, broker_code::exposure_changed);
    EXPECT_FALSE(find(trader.snapshot(), "during.risk"));
}

TEST_F(CtpTrader, PreparationHasNoBrokerEffectsAndDispatchTracksFills)
{
    ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {}, clock);
    trader.start();
    EXPECT_THROW(
        send(
            trader,
            gate,
            order("o0", "SHFE", "rb2610", Side::buy, "1", "3500"),
            Offset::open,
            trader.snapshot().connection_generation,
            trader.snapshot().exposure_revision,
            dispatch_deadline()),
        Error)
        << "not connected";
    trader.connect(configuration());
    auto state = wait_for(trader, ready, 15s);
    ASSERT_EQ(state.phase, BrokerPhase::ready);
    EXPECT_EQ(state.trading_day, "20260928");
    ASSERT_TRUE(state.funds);
    EXPECT_EQ(state.funds->available, d("950000"));
    EXPECT_EQ(state.funds->commission, d("12.34"));
    EXPECT_TRUE(state.positions.empty());

    auto prepared = trader.prepare(
        order("o1", "SHFE", "rb2610", Side::buy, "2", "3500.125"),
        Offset::open,
        state.connection_generation,
        state.exposure_revision,
        dispatch_deadline());
    EXPECT_TRUE(trader.snapshot().orders.empty()) << "preparation cannot dispatch an order";
    const auto recorded = prepared->order();
    EXPECT_EQ(recorded.status, BrokerOrderStatus::submitted);
    EXPECT_TRUE(await(trader, dispatch_with_permission(trader, gate, std::move(prepared))).invoked);
    state = wait_for(
        trader,
        [](const BrokerSnapshot& s) {
            return !s.positions.empty() && find(s, "o1")
                && find(s, "o1")->status == BrokerOrderStatus::filled;
        },
        10s);
    const auto* filled = find(state, "o1");
    ASSERT_TRUE(filled);
    EXPECT_EQ(filled->status, BrokerOrderStatus::filled);
    EXPECT_EQ(filled->broker_key, recorded.broker_key);
    EXPECT_EQ(filled->filled, d("2"));
    EXPECT_FALSE(filled->exchange_order_id.empty());
    ASSERT_EQ(state.trades.size(), 1U);
    EXPECT_EQ(state.trades[0].order_id, "o1");
    EXPECT_EQ(state.trades[0].price, d("3500.125"));
    ASSERT_EQ(state.positions.size(), 1U);
    EXPECT_EQ(state.positions[0].side, Side::buy);
    EXPECT_EQ(state.positions[0].today, d("2"));

    // SHFE requires a close bucket; an abandoned preparation sends nothing.
    EXPECT_THROW(
        send(
            trader,
            gate,
            order("o2", "SHFE", "rb2610", Side::sell, "1", "3500"),
            Offset::close,
            trader.snapshot().connection_generation,
            trader.snapshot().exposure_revision,
            dispatch_deadline()),
        std::invalid_argument);
    EXPECT_THROW(
        send(
            trader,
            gate,
            order("o3", "SHFE", "rb2610", Side::sell, "1.5", "3500"),
            Offset::close_today,
            trader.snapshot().connection_generation,
            trader.snapshot().exposure_revision,
            dispatch_deadline()),
        std::invalid_argument);
    auto abandoned = trader.prepare(
        order("o4", "SHFE", "rb2610", Side::sell, "1", "3500"),
        Offset::close_today,
        trader.snapshot().connection_generation,
        trader.snapshot().exposure_revision,
        dispatch_deadline());
    abandoned.reset();
    EXPECT_FALSE(find(trader.snapshot(), "o4"));
    EXPECT_THROW(
        send(
            trader,
            gate,
            order("o1", "SHFE", "rb2610", Side::buy, "1", "3500"),
            Offset::open,
            trader.snapshot().connection_generation,
            trader.snapshot().exposure_revision,
            dispatch_deadline()),
        Error)
        << "duplicate order ID";
    EXPECT_EQ(fake.query_rejections(), 0) << "queries respect the CTP flow limit";
    trader.disconnect();
    EXPECT_EQ(trader.snapshot().phase, BrokerPhase::disconnected);
}

TEST_F(CtpTrader, RejectionsAndCancellation)
{
    ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {}, clock);
    trader.start();
    trader.connect(configuration());
    ASSERT_EQ(wait_for(trader, ready, 15s).phase, BrokerPhase::ready);
    send(
        trader,
        gate,
        order("bad", "DCE", "zz2609", Side::buy, "1", "100"),
        Offset::open,
        trader.snapshot().connection_generation,
        trader.snapshot().exposure_revision,
        dispatch_deadline());
    auto state = wait_for(
        trader,
        [](const BrokerSnapshot& s) {
            return find(s, "bad") && find(s, "bad")->status == BrokerOrderStatus::rejected;
        },
        5s);
    EXPECT_EQ(find(state, "bad")->error_code, 16);

    send(
        trader,
        gate,
        order("rest", "DCE", "m2609", Side::buy, "5", "2800"),
        Offset::open,
        trader.snapshot().connection_generation,
        trader.snapshot().exposure_revision,
        dispatch_deadline());
    state = wait_for(
        trader,
        [](const BrokerSnapshot& s) {
            return find(s, "rest") && find(s, "rest")->status == BrokerOrderStatus::accepted;
        },
        5s);
    ASSERT_EQ(find(state, "rest")->status, BrokerOrderStatus::accepted);
    cancel(trader, "rest");
    state = wait_for(
        trader,
        [](const BrokerSnapshot& s) {
            return find(s, "rest") && find(s, "rest")->status == BrokerOrderStatus::cancelled;
        },
        5s);
    EXPECT_EQ(find(state, "rest")->status, BrokerOrderStatus::cancelled);
    EXPECT_THROW(cancel(trader, "rest"), Error);
    EXPECT_THROW(cancel(trader, "missing"), Error);
    // DCE closes yesterday's positions first, so the generic close is allowed.
    EXPECT_NO_THROW(send(
        trader,
        gate,
        order("close", "DCE", "m2609", Side::sell, "1", "2800"),
        Offset::close,
        trader.snapshot().connection_generation,
        trader.snapshot().exposure_revision,
        dispatch_deadline()));
}

TEST_F(CtpTrader, ReconnectAndRestartKeepOrdersAttributed)
{
    std::string key, next_key;
    {
        ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {}, clock);
        trader.start();
        trader.connect(configuration());
        ASSERT_EQ(wait_for(trader, ready, 15s).phase, BrokerPhase::ready);
        key = send(
            trader,
            gate,
            order("resting", "SHFE", "rb2610", Side::buy, "4", "3400"),
            Offset::open,
            trader.snapshot().connection_generation,
            trader.snapshot().exposure_revision,
            dispatch_deadline())
                  .broker_key;
        ASSERT_EQ(
            wait_for(
                trader,
                [](const BrokerSnapshot& s) {
                    return find(s, "resting")
                        && find(s, "resting")->status == BrokerOrderStatus::accepted;
                },
                5s)
                .orders.size(),
            1U);
        const auto before = trader.snapshot().sequence;
        fake.reconnect();
        auto state = wait_for(
            trader,
            [&](const BrokerSnapshot& s) {
                return s.sequence > before + 2 && ready(s);
            },
            15s);
        ASSERT_EQ(state.phase, BrokerPhase::ready);
        ASSERT_EQ(state.orders.size(), 1U) << "the queried order merges with the tracked one";
        EXPECT_EQ(find(state, "resting")->broker_key, key);
        // A new session allocates keys that cannot collide with the old one.
        const auto next = send(
            trader,
            gate,
            order("after", "SHFE", "rb2610", Side::buy, "4", "3401"),
            Offset::open,
            trader.snapshot().connection_generation,
            trader.snapshot().exposure_revision,
            dispatch_deadline());
        next_key = next.broker_key;
        EXPECT_NE(
            next.broker_key.substr(0, next.broker_key.rfind(':')),
            key.substr(0, key.rfind(':')));
        cancel(trader, "resting");
        EXPECT_EQ(
            wait_for(
                trader,
                [](const BrokerSnapshot& s) {
                    return find(s, "resting")
                        && find(s, "resting")->status == BrokerOrderStatus::cancelled;
                },
                5s)
                .phase,
            BrokerPhase::ready);
    }
    // A restarted process attributes orders from its journal; others stay
    // unattributed but visible.
    ctp::Trader restarted(ASTERION_TEST_CTP_TRADER, flow, gate, [] {}, clock);
    restarted.start();
    restarted.connect(
        configuration(),
        {{{"20260928", key}, {"resting", 1}}, {{"20260927", next_key}, {"yesterday", 2}}});
    const auto state = wait_for(restarted, ready, 15s);
    ASSERT_EQ(state.phase, BrokerPhase::ready);
    ASSERT_EQ(state.orders.size(), 2U);
    EXPECT_EQ(find(state, "resting")->status, BrokerOrderStatus::cancelled);
    EXPECT_TRUE(find(state, ""));
    EXPECT_EQ(find(state, "yesterday"), nullptr);
    const auto effects = fake.call<int (*)()>("asterion_fake_catalog_side_effects");
    EXPECT_THROW(cancel(restarted, "yesterday"), Error);
    EXPECT_EQ(fake.call<int (*)()>("asterion_fake_catalog_side_effects"), effects);
    EXPECT_THROW(cancel(restarted, ""), Error);
}

TEST(ReportReconciliation, APositionQueryCrossingAFillCannotDoubleCountOpenLots)
{
    BrokerSnapshot state;
    state.trading_day = "20260928";
    ctp::ReportReconciler reports(state);
    ASSERT_FALSE(reports.finish_positions(state.exposure_revision));

    CThostFtdcTradeField trade{};
    std::strcpy(trade.TradingDay, "20260928");
    std::strcpy(trade.ExchangeID, "SHFE");
    std::strcpy(trade.InstrumentID, "rb2610");
    std::strcpy(trade.OrderSysID, "1");
    std::strcpy(trade.TradeID, "1");
    trade.Direction = THOST_FTDC_D_Buy;
    trade.OffsetFlag = THOST_FTDC_OF_Open;
    trade.Price = 3500;
    trade.Volume = 1;
    reports.apply(trade);
    const auto revision = state.exposure_revision;

    CThostFtdcInvestorPositionField position{};
    std::strcpy(position.ExchangeID, "SHFE");
    std::strcpy(position.InstrumentID, "rb2610");
    position.PosiDirection = THOST_FTDC_PD_Long;
    position.HedgeFlag = THOST_FTDC_HF_Speculation;
    position.Position = position.TodayPosition = 1;
    reports.apply(position);
    std::strcpy(trade.TradeID, "2");
    reports.apply(trade);

    EXPECT_TRUE(reports.finish_positions(revision));
    EXPECT_FALSE(state.positions_reconciled);
    EXPECT_TRUE(state.positions.empty());
    EXPECT_EQ(state.unreconciled_open_lots, d("2"));

    position.Position = position.TodayPosition = 2;
    reports.apply(position);
    EXPECT_FALSE(reports.finish_positions(state.exposure_revision));
    EXPECT_TRUE(state.positions_reconciled);
    ASSERT_EQ(state.positions.size(), 1U);
    EXPECT_EQ(state.positions.front().today, d("2"));
    EXPECT_EQ(state.unreconciled_open_lots, d("0"));
}

TEST(ReportReconciliation, ClosingFillsReserveTheirBucketsWithoutCountingLateReportsTwice)
{
    BrokerSnapshot state;
    state.trading_day = "20260928";
    ctp::ReportReconciler reports(state);
    CThostFtdcInvestorPositionField position{};
    std::strcpy(position.ExchangeID, "SHFE");
    std::strcpy(position.InstrumentID, "rb2610");
    position.PosiDirection = THOST_FTDC_PD_Long;
    position.HedgeFlag = THOST_FTDC_HF_Speculation;
    position.Position = 5;
    position.TodayPosition = 3;
    reports.apply(position);
    ASSERT_FALSE(reports.finish_positions(state.exposure_revision));
    const auto today = std::tuple{InstrumentId{"SHFE", "rb2610"}, Side::sell, Offset::close_today};
    const auto yesterday =
        std::tuple{InstrumentId{"SHFE", "rb2610"}, Side::sell, Offset::close_yesterday};
    CThostFtdcOrderField order{};
    order.FrontID = order.SessionID = 1;
    std::strcpy(order.OrderRef, "1");
    std::strcpy(order.OrderSysID, "1");
    std::strcpy(order.TradingDay, "20260928");
    std::strcpy(order.ExchangeID, "SHFE");
    std::strcpy(order.InstrumentID, "rb2610");
    order.Direction = THOST_FTDC_D_Sell;
    order.CombOffsetFlag[0] = THOST_FTDC_OF_CloseToday;
    order.VolumeTotalOriginal = order.VolumeTraded = 1;
    order.OrderStatus = THOST_FTDC_OST_AllTraded;
    reports.apply(order);
    EXPECT_EQ(state.unreconciled_close_lots.at(today), d("1"));
    position.Position = 4;
    position.TodayPosition = 2;
    reports.apply(position);
    ASSERT_FALSE(reports.finish_positions(state.exposure_revision));
    CThostFtdcTradeField trade{};
    std::strcpy(trade.TradingDay, "20260928");
    std::strcpy(trade.ExchangeID, "SHFE");
    std::strcpy(trade.InstrumentID, "rb2610");
    std::strcpy(trade.OrderSysID, "1");
    std::strcpy(trade.TradeID, "1");
    trade.Direction = THOST_FTDC_D_Sell;
    trade.OffsetFlag = THOST_FTDC_OF_CloseToday;
    trade.Price = 3500;
    trade.Volume = 1;
    reports.apply(trade);
    EXPECT_TRUE(state.unreconciled_close_lots.empty());

    std::strcpy(trade.OrderSysID, "2");
    std::strcpy(trade.TradeID, "2");
    trade.OffsetFlag = THOST_FTDC_OF_CloseYesterday;
    reports.apply(trade);
    std::strcpy(order.OrderRef, "2");
    std::strcpy(order.OrderSysID, "2");
    order.CombOffsetFlag[0] = THOST_FTDC_OF_CloseYesterday;
    reports.apply(order);
    EXPECT_EQ(state.unreconciled_close_lots.at(yesterday), d("1"));
    const auto revision = state.exposure_revision;
    position.Position = 3;
    reports.apply(position);
    std::strcpy(trade.OrderSysID, "3");
    std::strcpy(trade.TradeID, "3");
    trade.OffsetFlag = THOST_FTDC_OF_CloseToday;
    reports.apply(trade);
    EXPECT_TRUE(reports.finish_positions(revision));
    EXPECT_EQ(state.unreconciled_close_lots.at(today), d("1"));
    EXPECT_EQ(state.unreconciled_close_lots.at(yesterday), d("1"));
    EXPECT_EQ(state.positions.front().today, d("2"));
    EXPECT_EQ(state.positions.front().yesterday, d("2"));
    position.Position = 2;
    position.TodayPosition = 1;
    reports.apply(position);
    EXPECT_FALSE(reports.finish_positions(state.exposure_revision));
    EXPECT_TRUE(state.unreconciled_close_lots.empty());
    EXPECT_EQ(state.positions.front().today, d("1"));
    EXPECT_EQ(state.positions.front().yesterday, d("1"));
    std::strcpy(trade.OrderSysID, "4");
    std::strcpy(trade.TradeID, "4");
    trade.OffsetFlag = THOST_FTDC_OF_Close;
    reports.apply(trade);
    EXPECT_EQ(
        state.unreconciled_close_lots.at(
            {InstrumentId{"SHFE", "rb2610"}, Side::sell, Offset::close}),
        d("1"));
    position.Position = position.TodayPosition = 1;
    reports.apply(position);
    EXPECT_FALSE(reports.finish_positions(state.exposure_revision));
    reports.apply(trade);
    EXPECT_TRUE(state.unreconciled_close_lots.empty());
    EXPECT_EQ(state.positions.front().today, d("1"));
    EXPECT_EQ(state.positions.front().yesterday, d("0"));
}

TEST(ReportReconciliation, RestoredIdentityUsesCurrentDayAndLatestJournalSequence)
{
    BrokerSnapshot state;
    state.trading_day = "20260928";
    ctp::ReportReconciler reports(state);
    reports.reset({{{"20260927", "1:1:1"}, {"old", 1}}, {{"20260928", "1:1:1"}, {"current", 2}}});
    CThostFtdcOrderField order{};
    order.FrontID = 1;
    order.SessionID = 1;
    order.Direction = THOST_FTDC_D_Buy;
    order.CombOffsetFlag[0] = THOST_FTDC_OF_Open;
    order.LimitPrice = 3500;
    order.VolumeTotalOriginal = order.VolumeTotal = 3;
    order.OrderSubmitStatus = THOST_FTDC_OSS_Accepted;
    order.OrderStatus = THOST_FTDC_OST_NoTradeQueueing;
    std::strcpy(order.ExchangeID, "SHFE");
    std::strcpy(order.InstrumentID, "rb2610");
    std::strcpy(order.OrderRef, "1");
    std::strcpy(order.TradingDay, "20260927");
    EXPECT_FALSE(reports.apply(order));
    EXPECT_TRUE(state.orders.empty());
    EXPECT_EQ(state.exposure_revision, 0U);
    std::strcpy(order.TradingDay, "20260928");
    reports.apply(order);
    ASSERT_EQ(state.orders.size(), 1U);
    EXPECT_EQ(state.orders.front().order_id, "current");
    EXPECT_EQ(reports.find_order("old"), nullptr);
    const auto revision = state.exposure_revision;
    CThostFtdcTradeField trade{};
    std::strcpy(trade.TradingDay, "20260927");
    EXPECT_FALSE(reports.apply(trade));
    EXPECT_TRUE(state.trades.empty());
    EXPECT_EQ(state.exposure_revision, revision);
    order.TradingDay[0] = '\0';
    EXPECT_THROW(reports.apply(order), std::runtime_error);
    reports.remember({"20260928", "1:1:1"}, {"latest", 5});
    reports.remember({"20260928", "1:1:1"}, {"stale", 3});
    EXPECT_EQ(state.orders.front().order_id, "latest");
    EXPECT_EQ(reports.find_order("current"), nullptr);
    EXPECT_EQ(reports.find_order("stale"), nullptr);
    EXPECT_NE(reports.find_order("latest"), nullptr);
}

TEST(ReportReconciliation, ALateOrderReportAttributesTheTradeThatPrecededIt)
{
    BrokerSnapshot state;
    state.trading_day = "20260928";
    ctp::ReportReconciler reports(state);
    reports.reset({{{"20260928", "1:1:1"}, {"local", 1}}});
    CThostFtdcTradeField trade{};
    std::strcpy(trade.TradingDay, "20260928");
    std::strcpy(trade.ExchangeID, "SHFE");
    std::strcpy(trade.InstrumentID, "rb2610");
    std::strcpy(trade.TradeID, "7");
    std::strcpy(trade.OrderSysID, "42");
    trade.Direction = THOST_FTDC_D_Buy;
    trade.OffsetFlag = THOST_FTDC_OF_Open;
    trade.Volume = 1;
    trade.Price = 3500;
    EXPECT_TRUE(reports.apply(trade));
    ASSERT_EQ(state.trades.size(), 1U);
    EXPECT_TRUE(state.trades.front().order_id.empty());
    CThostFtdcOrderField order{};
    order.FrontID = order.SessionID = 1;
    std::strcpy(order.OrderRef, "1");
    std::strcpy(order.TradingDay, "20260928");
    std::strcpy(order.ExchangeID, "SHFE");
    std::strcpy(order.InstrumentID, "rb2610");
    std::strcpy(order.OrderSysID, "42");
    order.Direction = THOST_FTDC_D_Buy;
    order.CombOffsetFlag[0] = THOST_FTDC_OF_Open;
    order.LimitPrice = 3500;
    order.VolumeTotalOriginal = order.VolumeTraded = 1;
    order.OrderSubmitStatus = THOST_FTDC_OSS_Accepted;
    order.OrderStatus = THOST_FTDC_OST_AllTraded;
    reports.apply(order);
    EXPECT_EQ(state.trades.front().order_id, "local");
    reports.action_failed("0:0:", 26, "SHFE:42");
    EXPECT_EQ(state.orders.front().error_code, 26);

    CThostFtdcOrderField unaccepted{};
    unaccepted.FrontID = unaccepted.SessionID = 1;
    std::strcpy(unaccepted.OrderRef, "2");
    std::strcpy(unaccepted.TradingDay, "20260928");
    std::strcpy(unaccepted.ExchangeID, "SHFE");
    std::strcpy(unaccepted.InstrumentID, "rb2610");
    unaccepted.Direction = THOST_FTDC_D_Buy;
    unaccepted.CombOffsetFlag[0] = THOST_FTDC_OF_Open;
    unaccepted.LimitPrice = 3500;
    unaccepted.VolumeTotalOriginal = 1;
    unaccepted.OrderSubmitStatus = THOST_FTDC_OSS_InsertSubmitted;
    unaccepted.OrderStatus = THOST_FTDC_OST_Canceled;
    reports.apply(unaccepted);
    EXPECT_EQ(state.orders.back().status, BrokerOrderStatus::rejected);

    // A cancel that first failed and then succeeded leaves the order cancelled.
    auto working = unaccepted;
    std::strcpy(working.OrderRef, "3");
    std::strcpy(working.OrderSysID, "43");
    working.OrderSubmitStatus = THOST_FTDC_OSS_Accepted;
    working.OrderStatus = THOST_FTDC_OST_NoTradeQueueing;
    reports.apply(working);
    reports.action_failed("1:1:3", 26);
    working.OrderStatus = THOST_FTDC_OST_Canceled;
    reports.apply(working);
    EXPECT_EQ(state.orders.back().status, BrokerOrderStatus::cancelled);

    const auto revision = state.exposure_revision;
    CThostFtdcInvestorPositionField pos{};
    std::strcpy(pos.ExchangeID, "DCE");
    std::strcpy(pos.InstrumentID, "m2609");
    pos.PosiDirection = THOST_FTDC_PD_Long;
    pos.HedgeFlag = THOST_FTDC_HF_Speculation;
    pos.Position = 2;
    pos.TodayPosition = 5;
    reports.apply(pos);
    pos.HedgeFlag = THOST_FTDC_HF_Hedge;
    pos.Position = 9;
    reports.apply(pos);
    EXPECT_FALSE(reports.finish_positions(revision));
    ASSERT_EQ(state.positions.size(), 1U);
    EXPECT_EQ(state.positions.front().today, d("2"));
    EXPECT_EQ(state.positions.front().yesterday, d("0"));
}

TEST_F(CtpTrader, ReusedReferenceRetiresThePreviousCallerBinding)
{
    ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {}, clock);
    const auto key =
        "1:" + std::to_string(fake.call<int (*)()>("asterion_fake_trader_next_session")) + ":1";
    // The journal can contain a request which never reached the broker. Its
    // reference is then absent from the broker's MaxOrderRef on recovery.
    trader.connect(configuration(), {{{"20260928", key}, {"unconfirmed", 1}}});
    ASSERT_EQ(wait_for(trader, ready, 15s).phase, BrokerPhase::ready);
    const auto sent = send(
        trader,
        gate,
        order("replacement", "SHFE", "rb2610", Side::buy, "3", "3500"),
        Offset::open,
        trader.snapshot().connection_generation,
        trader.snapshot().exposure_revision,
        dispatch_deadline());
    ASSERT_EQ(sent.broker_key, key);
    const auto state = wait_for(
        trader,
        [](const BrokerSnapshot& value) {
            return find(value, "replacement") != nullptr;
        },
        5s);
    ASSERT_NE(find(state, "replacement"), nullptr);
    EXPECT_EQ(find(state, "unconfirmed"), nullptr);
    const auto effects = fake.call<int (*)()>("asterion_fake_catalog_side_effects");
    EXPECT_THROW(cancel(trader, "unconfirmed"), Error);
    EXPECT_EQ(fake.call<int (*)()>("asterion_fake_catalog_side_effects"), effects);
    EXPECT_NO_THROW(cancel(trader, "replacement"));
}

TEST_F(CtpTrader, DelayedInputRejectionCannotRejectAReconnectedOrder)
{
    ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {}, clock);
    trader.connect(configuration());
    ASSERT_EQ(wait_for(trader, ready, 15s).phase, BrokerPhase::ready);
    fake.call<void (*)(int)>("asterion_fake_trader_hold_insert_errors", 1);
    const auto old = send(
        trader,
        gate,
        order("old", "DCE", "zz2609", Side::buy, "3", "100"),
        Offset::open,
        trader.snapshot().connection_generation,
        trader.snapshot().exposure_revision,
        dispatch_deadline());
    const auto generation = trader.snapshot().connection_generation;
    fake.reconnect();
    ASSERT_EQ(
        wait_for(
            trader,
            [&](const BrokerSnapshot& state) {
                return ready(state) && state.connection_generation != generation;
            },
            15s)
            .phase,
        BrokerPhase::ready);
    const auto current = send(
        trader,
        gate,
        order("current", "DCE", "m2609", Side::buy, "3", "2800"),
        Offset::open,
        trader.snapshot().connection_generation,
        trader.snapshot().exposure_revision,
        dispatch_deadline());
    ASSERT_NE(old.broker_key, current.broker_key);
    ASSERT_EQ(
        old.broker_key.substr(old.broker_key.rfind(':')),
        current.broker_key.substr(current.broker_key.rfind(':')));
    ASSERT_NE(
        find(
            wait_for(
                trader,
                [](const BrokerSnapshot& state) {
                    const auto* order = find(state, "current");
                    return order && order->status == BrokerOrderStatus::accepted;
                },
                5s),
            "current"),
        nullptr);
    fake.call<void (*)(int)>("asterion_fake_trader_hold_insert_errors", 0);
    // The quote response follows the released failures on the fake callback queue.
    ASSERT_TRUE(await(trader, trader.quote({"DCE", "m2609"})));
    const auto state = trader.snapshot();
    ASSERT_NE(find(state, "current"), nullptr);
    EXPECT_EQ(find(state, "current")->status, BrokerOrderStatus::accepted);
    EXPECT_EQ(find(state, "current")->instrument.symbol, "m2609");
    EXPECT_EQ(find(state, "old"), nullptr);
    EXPECT_NO_THROW(cancel(trader, "current"));
}

TEST_F(CtpTrader, CredentialFailuresStopBeforeTrading)
{
    ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {}, clock);
    trader.start();
    EXPECT_THROW(trader.connect({"http://x", "9999", "1", "p", "a", "c"}), std::invalid_argument);
    EXPECT_THROW(trader.connect(configuration("secret", "")), std::invalid_argument);
    trader.connect(configuration("secret", "bad-auth"));
    auto state = wait_for(
        trader,
        [](const BrokerSnapshot& s) {
            return s.phase == BrokerPhase::error;
        },
        5s);
    EXPECT_EQ(state.error_code, 63);
    auto wait_releases = [&](int expected) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (fake.call<int (*)()>("asterion_fake_trader_sdk_releases") < expected
            && std::chrono::steady_clock::now() < deadline) {
            trader.poll();
            std::this_thread::sleep_for(5ms);
        }
        return fake.call<int (*)()>("asterion_fake_trader_sdk_releases");
    };
    EXPECT_EQ(wait_releases(1), 1);
    const int auths_after_auth_reject = fake.call<int (*)()>("asterion_fake_trader_auth_requests");
    fake.reconnect();
    std::this_thread::sleep_for(30ms);
    trader.poll();
    EXPECT_EQ(trader.snapshot().phase, BrokerPhase::error);
    EXPECT_EQ(trader.snapshot().error_code, 63);
    EXPECT_EQ(fake.call<int (*)()>("asterion_fake_trader_auth_requests"), auths_after_auth_reject);

    trader.connect(configuration("bad"));
    state = wait_for(
        trader,
        [](const BrokerSnapshot& s) {
            return s.phase == BrokerPhase::error;
        },
        5s);
    EXPECT_EQ(state.error_code, 3);
    EXPECT_EQ(wait_releases(2), 2);
    const int auths_after_login_reject = fake.call<int (*)()>("asterion_fake_trader_auth_requests");
    const int logins_after_login_reject =
        fake.call<int (*)()>("asterion_fake_trader_login_requests");
    fake.reconnect();
    std::this_thread::sleep_for(30ms);
    trader.poll();
    EXPECT_EQ(trader.snapshot().phase, BrokerPhase::error);
    EXPECT_EQ(trader.snapshot().error_code, 3);
    EXPECT_EQ(fake.call<int (*)()>("asterion_fake_trader_auth_requests"), auths_after_login_reject);
    EXPECT_EQ(
        fake.call<int (*)()>("asterion_fake_trader_login_requests"),
        logins_after_login_reject);
    EXPECT_THROW(
        send(
            trader,
            gate,
            order("x", "SHFE", "rb2610", Side::buy, "1", "3500"),
            Offset::open,
            trader.snapshot().connection_generation,
            trader.snapshot().exposure_revision,
            dispatch_deadline()),
        Error);
    trader.disconnect();
    EXPECT_EQ(trader.snapshot().phase, BrokerPhase::disconnected);
    EXPECT_EQ(trader.snapshot().error_code, 0);
    EXPECT_THROW((ctp::Trader(fs::path("missing-library"), flow, gate, [] {})), Error);
}

TEST_F(CtpTrader, AccountRatesFillCostsWithoutAffectingTheSession)
{
    ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {}, clock);
    trader.start();
    EXPECT_THROW(trader.query_costs({{{"SHFE", "rb2610"}, "rb"}}), Error) << "not connected";
    trader.connect(configuration());
    ASSERT_EQ(wait_for(trader, ready, 15s).phase, BrokerPhase::ready);
    trader.query_costs(
        {{{"SHFE", "rb2610"}, "rb"}, {{"SHFE", "zz2610"}, "zz"}, {{"SHFE", "rel2610"}, "rel"}});
    const auto state = wait_for(
        trader,
        [](const BrokerSnapshot& s) {
            return s.costs.size() == 3 && std::ranges::none_of(s.costs, [](const BrokerCosts& c) {
                return c.state == BrokerCostsState::querying;
            });
        },
        15s);
    ASSERT_EQ(state.costs.size(), 3U);
    EXPECT_EQ(state.phase, BrokerPhase::ready) << "a missing rate does not end the session";
    const auto& rb = state.costs[0];
    ASSERT_EQ(rb.state, BrokerCostsState::ready);
    ASSERT_TRUE(rb.costs);
    // Margin takes the higher short rate; commission comes from the product row.
    EXPECT_EQ(rb.costs->margin_rate, d("0.12"));
    EXPECT_EQ(rb.costs->margin_per_lot, d("0"));
    EXPECT_EQ(rb.costs->open_fee_rate, d("0.0001"));
    EXPECT_EQ(rb.costs->close_yesterday_fee_rate, d("0.0001"));
    EXPECT_EQ(rb.costs->close_today_fee_rate, d("0.0003"));
    EXPECT_EQ(rb.costs->close_today_fee, d("1.5"));
    EXPECT_GT(rb.queried_ms, 0);
    EXPECT_EQ(state.costs[1].state, BrokerCostsState::unavailable);
    EXPECT_FALSE(state.costs[1].costs);
    EXPECT_EQ(state.costs[2].state, BrokerCostsState::unavailable)
        << "a relative markup needs the exchange rate";
    EXPECT_EQ(fake.query_rejections(), 0) << "rate queries respect the CTP flow limit";
}

TEST_F(CtpTrader, QuoteFlowControlRetriesWithoutEndingTheConnection)
{
    ctp::SharedLibrary reject(ASTERION_TEST_CTP_TRADER, "asterion_fake_trader_reject_quotes");
    ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {}, clock);
    trader.start();
    trader.connect(configuration());
    ASSERT_EQ(wait_for(trader, ready, 15s).phase, BrokerPhase::ready);
    for (const int code : {-2, -3}) {
        reject.symbol<void (*)(int, int)>()(code, 1);
        auto quote = await(trader, trader.quote({"SHFE", "rb2610"}));
        ASSERT_TRUE(quote);
        EXPECT_EQ(quote->last, d("3500"));
        EXPECT_EQ(trader.snapshot().phase, BrokerPhase::ready);
    }
    EXPECT_EQ(fake.query_rejections(), 2);
    auto pending = send(
        trader,
        gate,
        order("after.throttle", "SHFE", "rb2610", Side::buy, "4", "3400"),
        Offset::open,
        trader.snapshot().connection_generation,
        trader.snapshot().exposure_revision,
        dispatch_deadline());
    EXPECT_NE(pending.status, BrokerOrderStatus::rejected);
    EXPECT_NO_THROW(cancel(trader, "after.throttle"));
}

TEST_F(CtpTrader, ReconnectFencesAuthorizationBeforeAndDuringJournaling)
{
    ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {}, clock);
    trader.start();
    trader.connect(configuration());
    auto state = wait_for(trader, ready, 15s);
    ASSERT_EQ(state.phase, BrokerPhase::ready);
    const auto reconnect = [&] {
        const auto generation = trader.snapshot().connection_generation;
        fake.reconnect();
        return wait_for(
            trader,
            [&](const BrokerSnapshot& s) {
                return ready(s) && s.connection_generation != generation;
            },
            15s);
    };
    ASSERT_NE(reconnect().connection_generation, state.connection_generation);
    EXPECT_THROW(
        trader.prepare(
            order("stale", "SHFE", "rb2610", Side::buy, "4", "3400"),
            Offset::open,
            state.connection_generation,
            state.exposure_revision,
            dispatch_deadline()),
        Error);
    state = trader.snapshot();
    auto prepared = trader.prepare(
        order("during.journal", "SHFE", "rb2610", Side::buy, "4", "3400"),
        Offset::open,
        state.connection_generation,
        state.exposure_revision,
        dispatch_deadline());
    ASSERT_EQ(reconnect().phase, BrokerPhase::ready);
    const auto result = await(trader, dispatch_with_permission(trader, gate, std::move(prepared)));
    EXPECT_FALSE(result.invoked);
    EXPECT_EQ(result.code, broker_code::session_changed);
    trader.disconnect();
    trader.connect(configuration());
    const auto synced = wait_for(trader, ready, 15s);
    EXPECT_TRUE(synced.trades.empty());
    EXPECT_TRUE(synced.orders.empty()) << "nothing was sent in the newer connection";
}

TEST_F(CtpTrader, DisconnectCompletesAThrottledQuoteWithoutRetryingItAfterReconnect)
{
    ctp::SharedLibrary reject(ASTERION_TEST_CTP_TRADER, "asterion_fake_trader_reject_quotes");
    ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {}, clock);
    trader.start();
    trader.connect(configuration());
    ASSERT_EQ(wait_for(trader, ready, 15s).phase, BrokerPhase::ready);
    reject.symbol<void (*)(int, int)>()(-3, 100);
    auto query = trader.quote({"SHFE", "rb2610"});
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!fake.query_rejections() && std::chrono::steady_clock::now() < deadline) {
        clock->advance(250ms);
        trader.poll();
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_GT(fake.query_rejections(), 0);
    trader.disconnect();
    ASSERT_EQ(query.wait_for(2s), std::future_status::ready);
    EXPECT_FALSE(query.get());
    reject.symbol<void (*)(int, int)>()(0, 0);
    trader.connect(configuration());
    ASSERT_EQ(wait_for(trader, ready, 15s).phase, BrokerPhase::ready);
    EXPECT_TRUE(await(trader, trader.quote({"SHFE", "rb2610"})));
}

TEST_F(CtpTrader, TimedOutQuoteRetiresRetriesAndLeavesTheWorkerAvailable)
{
    ctp::SharedLibrary reject(ASTERION_TEST_CTP_TRADER, "asterion_fake_trader_reject_quotes");
    ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {}, clock);
    trader.start();
    trader.connect(configuration());
    ASSERT_EQ(wait_for(trader, ready, 15s).phase, BrokerPhase::ready);
    reject.symbol<void (*)(int, int)>()(-3, 100);
    EXPECT_FALSE(await(trader, trader.quote({"SHFE", "rb2610"})));
    EXPECT_EQ(trader.snapshot().phase, BrokerPhase::ready);
    reject.symbol<void (*)(int, int)>()(0, 0);
    EXPECT_TRUE(await(trader, trader.quote({"SHFE", "rb2610"})));
    EXPECT_EQ(trader.snapshot().phase, BrokerPhase::ready);
}

TEST_F(CtpTrader, LateQueryRepliesCannotReplaceReconnectedAccountState)
{
    ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {}, clock);
    trader.start();
    trader.connect(configuration());
    const auto initial = wait_for(trader, ready, 15s);
    ASSERT_EQ(initial.phase, BrokerPhase::ready);
    fake.reconnect();
    const auto synced = wait_for(
        trader,
        [&](const BrokerSnapshot& s) {
            return ready(s) && s.connection_generation != initial.connection_generation;
        },
        15s);
    ASSERT_EQ(synced.phase, BrokerPhase::ready);
    ctp::SharedLibrary inject(ASTERION_TEST_CTP_TRADER, "asterion_fake_trader_stale_queries");
    ctp::SharedLibrary batches(ASTERION_TEST_CTP_TRADER, "asterion_fake_trader_stale_batches");
    const auto before = batches.symbol<int (*)()>()();
    inject.symbol<void (*)()>()();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (batches.symbol<int (*)()>()() == before && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
    ASSERT_GT(batches.symbol<int (*)()>()(), before);
    trader.poll();
    const auto after = trader.snapshot();
    EXPECT_EQ(after.sequence, synced.sequence);
    EXPECT_EQ(after.synchronized_ms, synced.synchronized_ms);
    ASSERT_TRUE(after.funds);
    EXPECT_EQ(after.funds->balance, synced.funds->balance);
    EXPECT_TRUE(after.positions.empty());
    EXPECT_TRUE(after.costs.empty());
}

TEST_F(CtpTrader, QuotesGoAheadOfQueuedRateQueries)
{
    ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {}, clock);
    trader.start();
    trader.connect(configuration());
    ASSERT_EQ(wait_for(trader, ready, 15s).phase, BrokerPhase::ready);
    // Ten rate queries need over ten seconds at the CTP query limit.
    std::vector<std::pair<InstrumentId, std::string>> contracts;
    for (const auto* symbol : {"rb2610", "rb2611", "rb2612", "rb2701", "rb2702"})
        contracts.push_back({{"SHFE", symbol}, "rb"});
    trader.query_costs(contracts);
    const auto started = steady_now(*clock);
    const auto quote = await(trader, trader.quote({"SHFE", "rb2610"}));
    ASSERT_TRUE(quote);
    EXPECT_EQ(quote->last, d("3500"));
    EXPECT_LT(steady_now(*clock) - started, 4s)
        << "an order's quote must not wait behind rate queries";
}

TEST_F(CtpTrader, RecordContinuationRejectsAChangedConnectionOrExposureBeforeDisconnecting)
{
    ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {}, clock);
    trader.start();
    trader.connect(configuration());
    const auto before = wait_for(trader, ready, 15s);
    ASSERT_TRUE(before.positions_reconciled);
    EXPECT_THROW(
        trader.disconnect_checked(before.connection_generation + 1, before.exposure_revision),
        Error);
    EXPECT_EQ(trader.snapshot().phase, BrokerPhase::ready);
    send(
        trader,
        gate,
        order("working", "SHFE", "rb2610", Side::buy, "3", "3500"),
        Offset::open,
        before.connection_generation,
        before.exposure_revision,
        dispatch_deadline());
    ASSERT_TRUE(
        wait_for(
            trader,
            [](const BrokerSnapshot& state) {
                return find(state, "working")
                    && find(state, "working")->status == BrokerOrderStatus::accepted;
            },
            2s)
            .phase
        == BrokerPhase::ready);
    EXPECT_THROW(
        trader.disconnect_checked(before.connection_generation, before.exposure_revision),
        Error);
    EXPECT_EQ(trader.snapshot().phase, BrokerPhase::ready);
    cancel(trader, "working");
    const auto after = wait_for(
        trader,
        [](const BrokerSnapshot& state) {
            return find(state, "working")
                && find(state, "working")->status == BrokerOrderStatus::cancelled;
        },
        2s);
    ASSERT_EQ(find(after, "working")->status, BrokerOrderStatus::cancelled);
    EXPECT_NO_THROW(
        trader.disconnect_checked(after.connection_generation, after.exposure_revision));
    EXPECT_EQ(trader.snapshot().phase, BrokerPhase::disconnected);
}

TEST_F(CtpTrader, CashTransferCallbackRefreshesPreBalanceDepositAndWithdraw)
{
    ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {}, clock);
    trader.start();
    trader.connect(configuration());
    auto state = wait_for(trader, ready, 15s);
    ASSERT_TRUE(state.funds);
    EXPECT_EQ(state.funds->pre_balance, d("1000000.01"));
    EXPECT_EQ(state.funds->deposit, d("0"));
    EXPECT_EQ(state.funds->withdraw, d("0"));

    fake.call<void (*)(double, double, double, double, double)>(
        "asterion_fake_trader_funds",
        1000000.0,
        950000.0,
        100000.0,
        150000.0,
        40000.0);
    state = wait_for(
        trader,
        [](const BrokerSnapshot& s) {
            return s.funds && s.funds->deposit == d("100000") && s.funds->withdraw == d("150000");
        },
        5s);
    EXPECT_EQ(state.funds->pre_balance, d("1000000"));
    EXPECT_EQ(state.funds->balance, d("950000"));
    EXPECT_EQ(state.funds->deposit, d("100000"));
    EXPECT_EQ(state.funds->withdraw, d("150000"));
    EXPECT_EQ(state.funds->margin, d("40000"));
}

TEST(ReportReconciliation, SelfTradeWithSameTradeIdAndOppositeSidesReconcilesBothLegs)
{
    BrokerSnapshot state;
    state.trading_day = "20260928";
    ctp::ReportReconciler reports(state);
    ASSERT_FALSE(reports.finish_positions(state.exposure_revision));

    CThostFtdcOrderField buy{};
    buy.FrontID = buy.SessionID = 1;
    std::strcpy(buy.OrderRef, "1");
    std::strcpy(buy.OrderSysID, "10");
    std::strcpy(buy.TradingDay, "20260928");
    std::strcpy(buy.ExchangeID, "SHFE");
    std::strcpy(buy.InstrumentID, "rb2610");
    buy.Direction = THOST_FTDC_D_Buy;
    buy.CombOffsetFlag[0] = THOST_FTDC_OF_Open;
    buy.VolumeTotalOriginal = buy.VolumeTraded = 1;
    buy.OrderStatus = THOST_FTDC_OST_AllTraded;
    reports.apply(buy);

    auto sell = buy;
    std::strcpy(sell.OrderRef, "2");
    std::strcpy(sell.OrderSysID, "11");
    sell.Direction = THOST_FTDC_D_Sell;
    reports.apply(sell);

    CThostFtdcTradeField trade{};
    std::strcpy(trade.TradingDay, "20260928");
    std::strcpy(trade.ExchangeID, "SHFE");
    std::strcpy(trade.InstrumentID, "rb2610");
    std::strcpy(trade.TradeID, "99");
    std::strcpy(trade.OrderSysID, "10");
    trade.Direction = THOST_FTDC_D_Buy;
    trade.OffsetFlag = THOST_FTDC_OF_Open;
    trade.Price = 3500;
    trade.Volume = 1;
    EXPECT_TRUE(reports.apply(trade));
    EXPECT_FALSE(reports.apply(trade));

    std::strcpy(trade.OrderSysID, "11");
    trade.Direction = THOST_FTDC_D_Sell;
    EXPECT_TRUE(reports.apply(trade));
    EXPECT_FALSE(reports.apply(trade));
    ASSERT_EQ(state.trades.size(), 2U);

    EXPECT_FALSE(reports.finish_positions(state.exposure_revision));
    EXPECT_TRUE(state.positions_reconciled);
}

TEST_F(CtpTrader, RefreshRetainsFundsAndReadySessionSurvivesRspErrorAndCancelsAfterSdkError)
{
    ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {}, clock);
    trader.start();
    trader.connect(configuration());
    const auto initial = wait_for(trader, ready, 15s);
    ASSERT_EQ(initial.phase, BrokerPhase::ready);
    ASSERT_TRUE(initial.funds);

    // Holding a live funds refresh keeps the previous valid funds snapshot visible.
    fake.call<void (*)(int)>("asterion_fake_trader_hold_funds", 1);
    fake.call<void (*)(double, double, double, double, double)>(
        "asterion_fake_trader_funds",
        1000000.0,
        920000.0,
        0.0,
        0.0,
        50000.0);
    const auto pending_deadline = std::chrono::steady_clock::now() + 5s;
    while (!fake.call<int (*)()>("asterion_fake_trader_pending_funds")
        && std::chrono::steady_clock::now() < pending_deadline) {
        clock->advance(250ms);
        trader.poll();
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_GT(fake.call<int (*)()>("asterion_fake_trader_pending_funds"), 0);
    const auto refreshing = trader.snapshot();
    EXPECT_TRUE(refreshing.refreshing_funds);
    ASSERT_TRUE(refreshing.funds);
    EXPECT_EQ(refreshing.funds->balance, initial.funds->balance);
    fake.call<void (*)(int)>("asterion_fake_trader_hold_funds", 0);
    const auto updated = wait_for(
        trader,
        [](const BrokerSnapshot& s) {
            return !s.refreshing_funds && s.funds && s.funds->balance == d("920000");
        },
        5s);
    ASSERT_TRUE(updated.funds);
    EXPECT_EQ(updated.funds->balance, d("920000"));

    // Unsolicited OnRspError on a ready session does not end the session.
    fake.call<void (*)(int)>("asterion_fake_trader_rsp_error", 90);
    ASSERT_TRUE(await(trader, trader.quote({"SHFE", "rb2610"})));
    EXPECT_EQ(trader.snapshot().phase, BrokerPhase::ready);

    // Even after a post-login SDK error puts the session into error, cancel stays available.
    send(
        trader,
        gate,
        order("working", "SHFE", "rb2610", Side::buy, "3", "3500"),
        Offset::open,
        trader.snapshot().connection_generation,
        trader.snapshot().exposure_revision,
        dispatch_deadline());
    ASSERT_NE(
        find(
            wait_for(
                trader,
                [](const BrokerSnapshot& s) {
                    const auto* o = find(s, "working");
                    return o && o->status == BrokerOrderStatus::accepted;
                },
                5s),
            "working"),
        nullptr);
    fake.call<void (*)()>("asterion_fake_trader_malformed_order");
    const auto failed = wait_for(
        trader,
        [](const BrokerSnapshot& s) {
            return s.phase == BrokerPhase::error;
        },
        5s);
    EXPECT_EQ(failed.error_code, broker_code::sdk_failure);
    cancel(trader, "working");
    const auto cancelled = wait_for(
        trader,
        [](const BrokerSnapshot& s) {
            const auto* o = find(s, "working");
            return o && o->status == BrokerOrderStatus::cancelled;
        },
        5s);
    ASSERT_NE(find(cancelled, "working"), nullptr);
    EXPECT_EQ(find(cancelled, "working")->status, BrokerOrderStatus::cancelled);
}
