#include "ctp_support.hpp"
#include "trading/journal_fixture.hpp"
#include "live_session.hpp"
#include "strategy_orders.hpp"
#include <asterion/futures/order_limits.hpp>
#include <barrier>
#include <future>
#include <asterion/base/error.hpp>
#include <asterion/system/process/child.hpp>
#include <asterion/system/process/artifact.hpp>
#include <asterion/system/durable_file.hpp>
#include <asterion/protocol/trading.hpp>
#include <google/protobuf/util/json_util.h>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <thread>
#include "account_journal.hpp"
#include <algorithm>
#include <regex>
#include <sys/resource.h>
using namespace asterion;

using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

// Blocking driver for account behavior tests. Production callers await these
// futures without occupying their I/O thread.
class LiveSession
{
    // The account the journal records: the manifest's, or a recovered journal's.
    std::string account_;
    asterion::trading::LiveSession session_;

    template <class... Args>
    static std::string account_of(const fs::path& journal, const Args&... args)
    {
        std::string account;
        (
            (void)[&] {
                if constexpr (std::is_same_v<std::decay_t<Args>, Json>)
                    account = args.at("account_id").template get<std::string>();
            }(),
            ...);
        if (account.empty())
            account =
                asterion::read_journal_header(journal, asterion::trading::AccountJournal::sidecars)
                    .at("account_id");
        return account;
    }

public:
    template <class... Args>
    explicit LiveSession(const fs::path& journal, Args&&... args)
        : account_(account_of(journal, args...)),
          session_(journal, std::forward<Args>(args)...)
    {
        session_.initialized().get();
    }

    void
    connect(std::string password, std::string auth, std::string front = "tcp://127.0.0.1:41205")
    {
        session_.connect({std::move(front), "client_app", std::move(password), std::move(auth)})
            .get();
    }

    void disconnect()
    {
        session_.disconnect().get();
    }

    ::trading::Trades trades(std::string day) const
    {
        return session_.trades(std::move(day)).get();
    }

    void query_costs()
    {
        session_.query_costs().get();
    }

    ::trading::Session wire_snapshot() const
    {
        return session_.snapshot().get();
    }

    // The session as these tests read it: its ProtoJSON with decimals as text,
    // enum values by their short lower-case names, instruments as venue and
    // symbol, and the account the journal records.
    Json snapshot() const
    {
        google::protobuf::util::JsonPrintOptions options;
        options.preserve_proto_field_names = true;
        options.always_print_fields_with_no_presence = true;
        std::string text;
        if (!google::protobuf::util::MessageToJsonString(wire_snapshot(), &text, options).ok())
            throw std::runtime_error("cannot print the snapshot");
        auto state = plain(Json::parse(text));
        const auto rename = [](Json& item, const char* from, const char* to) {
            if (item.contains(from)) {
                item[to] = item.at(from);
                item.erase(from);
            }
        };
        rename(state, "broker_state", "phase");
        rename(state, "ctp_error", "error_code");
        if (state.at("phase") == "failed")
            state["phase"] = "error";
        for (auto& order : state.at("orders")) {
            rename(order, "state", "status");
            rename(order, "lots", "quantity");
            rename(order, "filled_lots", "filled");
            rename(order, "price", "limit_price");
            rename(order, "ctp_error", "error_code");
        }
        for (auto& trade : state.at("trades"))
            rename(trade, "lots", "quantity");
        for (auto& position : state.at("positions")) {
            rename(position, "today_lots", "today");
            rename(position, "yesterday_lots", "yesterday");
        }
        const auto& policy = state.at("policy");
        state["risk"] = {
            {"max_order_quantity", policy.at("limits").at("max_order_lots")},
            {"max_gross_quantity", policy.at("limits").at("max_gross_lots")},
            {"max_working_orders", policy.at("limits").at("max_working_orders")}
        };
        state["max_price_deviation"] = policy.at("max_price_deviation");
        state["contracts"] = policy.at("contracts");
        const auto& journal = state.at("journal");
        state["capacity"] = {
            {"records_used", journal.at("records")},
            {"records_limit", journal.at("records_limit")},
            {"bytes_used", journal.at("bytes")},
            {"bytes_limit", journal.at("bytes_limit")}
        };
        state["segment_count"] = journal.at("segments");
        if (state.contains("authorized_ms"))
            state["authorization"] = {{"authorized_at_ms", state.at("authorized_ms")}};
        state["account_id"] = account_;
        return state;
    }

    static Json plain(const Json& value)
    {
        if (value.is_array()) {
            Json result = Json::array();
            for (const auto& item : value)
                result.push_back(plain(item));
            return result;
        }
        if (value.is_object()) {
            if (value.size() == 1 && value.contains("text"))
                return value.at("text");
            Json result = Json::object();
            for (const auto& [key, item] : value.items()) {
                if (key == "instrument") {
                    result["venue"] = item.at("exchange");
                    result["symbol"] = item.at("symbol");
                } else
                    result[key] = plain(item);
            }
            return result;
        }
        if (value.is_string()) {
            static const std::regex prefix(
                "(BROKER_STATE|ORDER_STATE|COSTS_STATE|SIDE|OFFSET|ENVIRONMENT)_([A-Z_]+)");
            std::smatch match;
            const auto text = value.get<std::string>();
            if (std::regex_match(text, match, prefix)) {
                auto name = match[2].str();
                std::ranges::transform(name, name.begin(), [](unsigned char c) {
                    return std::tolower(c);
                });
                return name;
            }
        }
        return value;
    }

    auto execute(std::string_view record, std::string_view policy, const Json& command)
    {
        return session_.execute(
            record,
            policy,
            asterion::trading::AccountRequest::from_wire(protocol::encode_command(command)));
    }

    bool recovery_required() const
    {
        return session_.recovery_required();
    }

    auto health() const
    {
        return session_.health();
    }

    void close_admission()
    {
        session_.close_admission();
    }

    void stop()
    {
        session_.stop();
    }

    auto stopped() const
    {
        return session_.stopped();
    }

    auto register_strategy(
        std::string instance = "strategy-run",
        std::vector<InstrumentId> contracts = {{"SHFE", "rb2610"}},
        std::optional<asterion::trading::StrategyWeightBudget> budget = std::nullopt)
    {
        return session_.register_strategy(std::move(instance), std::move(contracts), budget);
    }

    void accept_target(
        asterion::trading::StrategyAuthorityPtr authority,
        ::strategy::Decision decision,
        std::vector<MarketQuote> quotes,
        std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + 10s)
    {
        session_
            .accept_strategy_target(
                std::move(authority),
                std::move(decision),
                std::move(quotes),
                deadline)
            .get();
    }

    auto advance_target(asterion::trading::StrategyAuthorityPtr authority)
    {
        return session_.advance_strategy_target(std::move(authority));
    }

    auto release_strategy(asterion::trading::StrategyAuthorityPtr authority)
    {
        return session_.release_strategy(std::move(authority));
    }

    auto strategy_failure(asterion::trading::StrategyAuthorityPtr authority) const
    {
        return session_.strategy_failure(std::move(authority)).get();
    }

    void pause_strategy(asterion::trading::StrategyAuthorityPtr authority, bool paused)
    {
        session_.pause_strategy(std::move(authority), paused).get();
    }

    void flatten_strategy(
        asterion::trading::StrategyAuthorityPtr authority,
        std::vector<MarketQuote> quotes)
    {
        session_.flatten_strategy(std::move(authority), std::move(quotes)).get();
    }

    auto strategy_events(asterion::trading::StrategyAuthorityPtr authority)
    {
        return session_.take_strategy_events(std::move(authority)).get();
    }

    auto execute_strategy(asterion::trading::StrategyOrderBasis basis, const Json& command)
    {
        auto request =
            asterion::trading::AccountRequest::from_wire(protocol::encode_command(command));
        return session_.execute_strategy(
            std::move(basis),
            std::get<asterion::trading::SubmitOrder>(request.operation));
    }
};

// Test control entry point exported by the CTP SDK double. The handle keeps
// the library, and so its exchange state, loaded across sessions.
struct FakeExchange
{
    ctp::SharedLibrary library{ASTERION_TEST_CTP_TRADER, "asterion_fake_trader_reset"};

    void reset()
    {
        library.symbol<void (*)()>()();
    }

    template <class F, class... Args>
    auto call(const char* name, Args... args)
    {
        ctp::SharedLibrary control(ASTERION_TEST_CTP_TRADER, name);
        return control.symbol<F>()(args...);
    }
};

struct Directory
{
    fs::path path = fs::temp_directory_path() / ("asterion-live-" + unique_process_id());

    Directory()
    {
        fs::create_directory(path);
    }

    ~Directory()
    {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

Json contract(const char* symbol, const char* product = "rb", const char* month = "2026-10")
{
    return {
        {"venue", "SHFE"},
        {"symbol", symbol},
        {"currency", "CNY"},
        {"price_increment", "1"},
        {"multiplier", "10"},
        {"quantity_increment", "1"},
        {"product", product},
        {"delivery_month", month}
    };
}

// The fake fills orders of at most 2 lots at once; larger orders rest. Its
// market for rb2610 is 3500 within 3300..3700.
Json manifest(std::uint64_t working_orders = 1, const char* deviation = "0.02")
{
    return {
        {"account_id", "fixture-account"},
        {"type", "live_ctp"},
        {"environment", "simulation"},
        {"broker",
            {{"front", "tcp://127.0.0.1:41205"},
                {"broker_id", "9999"},
                {"user_id", "000001"},
                {"app_id", "client_app"}}},
        {"policy",
            {{"risk",
                 {{"max_order_quantity", "5"},
                     {"max_gross_quantity", "10"},
                     {"max_working_orders", working_orders}}},
                {"max_price_deviation", deviation},
                {"contracts", Json::array({contract("rb2610")})}}}
    };
}

Json submit(
    std::string id,
    const char* quantity,
    const char* price = "3500",
    const char* symbol = "rb2610")
{
    return {
        {"request_id", "submit." + id},
        {"action", "submit"},
        {"order_id", id},
        {"venue", "SHFE"},
        {"symbol", symbol},
        {"side", "buy"},
        {"offset", "open"},
        {"quantity", quantity},
        {"price", price}
    };
}

Json authorize(std::string id = "authorize", std::string user = "000001")
{
    return {{"request_id", std::move(id)}, {"action", "live_authorize"}, {"user_id", user}};
}

// ProtoJSON prints 64-bit integers as strings.
std::int64_t number(const Json& value)
{
    return std::stoll(value.get<std::string>());
}

std::string decimal(const Json& value)
{
    return Decimal::parse(value.get<std::string>()).str();
}

Json find_order(const Json& snapshot, const std::string& id)
{
    for (const auto& order : snapshot.at("orders"))
        if (order.at("id") == id)
            return order;
    return nullptr;
}

bool order_has_status(const Json& state, const std::string& id, std::string_view status)
{
    const auto order = find_order(state, id);
    return order.is_object() && order.at("status") == status;
}

template <class F>
Json wait_for(const LiveSession& session, F done)
{
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    auto state = session.snapshot();
    while (!done(state) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(20ms);
        state = session.snapshot();
    }
    return state;
}

void act(LiveSession& session, const Json& command)
{
    const auto state = session.snapshot();
    session
        .execute(
            state.at("account_id").get<std::string>(),
            state.at("policy_revision").get<std::string>(),
            command)
        .get();
}

Json policy_command(const LiveSession& session, Json definition)
{
    return {
        {"request_id", "policy.change"},
        {"action", "live_policy"},
        {"policy", std::move(definition)}
    };
}

Json ready(LiveSession& session)
{
    session.connect("secret", "auth-code");
    return wait_for(session, [](const Json& s) {
        return s.at("phase") == "ready";
    });
}

struct HeldCommit
{
    HeldCommit()
    {
        sqlite::hold_commits_for_testing(true);
    }

    ~HeldCommit()
    {
        release();
    }

    void release()
    {
        sqlite::hold_commits_for_testing(false);
    }

    bool waiting() const
    {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!sqlite::commit_waiting_for_testing() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(10ms);
        return sqlite::commit_waiting_for_testing();
    }
};

class Live : public ::testing::Test
{
protected:
    FakeExchange exchange;
    Directory directory;
    Directory owners;

    void SetUp() override
    {
        exchange.reset();
    }
};

} // namespace

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
        next["risk"]["max_gross_quantity"] = "1";
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
        next["risk"]["max_gross_quantity"] = "8";
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

TEST_F(Live, FilledOrdersDoNotCreateRiskHeadroomBeforePositionsAreReconciled)
{
    auto input = manifest(3);
    input["policy"]["risk"]["max_gross_quantity"] = "2";
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
        FakeExchange& exchange;

        ~Release()
        {
            exchange.call<void (*)(int, int)>("asterion_fake_trader_hold_insert_return", 0, 0);
            exchange.call<void (*)(int)>("asterion_fake_trader_hold_cancel_return", 0);
        }
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
        FakeExchange& exchange;

        ~Release()
        {
            exchange.call<void (*)(int, int)>("asterion_fake_trader_reject_quotes", 0, 0);
        }
    } release{exchange};

    exchange.call<void (*)(int, int)>("asterion_fake_trader_reject_quotes", -2, 100000);
    placed = session.execute(record, policy, submit("waiting.quote", "1"));
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!exchange.call<int (*)()>("asterion_fake_trader_query_rejections")
        && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
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
        FakeExchange& exchange;

        ~Release()
        {
            exchange.call<void (*)(int, int)>("asterion_fake_trader_reject_quotes", 0, 0);
        }
    } release{exchange};

    exchange.call<void (*)(int, int)>("asterion_fake_trader_reject_quotes", -2, 100000);
    placed = session.execute(record, policy, submit("waiting", "3"));
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!exchange.call<int (*)()>("asterion_fake_trader_query_rejections")
        && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
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
        FakeExchange& exchange;

        ~Release()
        {
            exchange.call<void (*)(int)>("asterion_fake_trader_hold_quote_return", 0);
        }
    } release{exchange};

    exchange.call<void (*)(int)>("asterion_fake_trader_hold_quote_return", 1);
    auto placed = session.execute(
        before.at("account_id").get<std::string>(),
        before.at("policy_revision").get<std::string>(),
        submit("expired.quote", "1"));
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!exchange.call<int (*)()>("asterion_fake_trader_quote_return_waiting")
        && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
    ASSERT_TRUE(exchange.call<int (*)()>("asterion_fake_trader_quote_return_waiting"));
    // No snapshot, control request or external polling drives the account here.
    ASSERT_EQ(placed.wait_for(12s), std::future_status::ready);
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
        const auto state = ready(again);
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
    const auto state = ready(closed);
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

TEST_F(Live, SdkFailureWithoutBrokerReportsKeepsAnUnknownIntentAndNoInventedOrder)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    exchange.call<void (*)(int)>("asterion_fake_trader_cancel_code", -2);
    EXPECT_THROW(act(session, submit("unknown", "3")), Error);
    exchange.call<void (*)(int)>("asterion_fake_trader_cancel_code", 0);
    const auto state = session.snapshot();
    EXPECT_TRUE(state.at("orders").empty()) << "an SDK return is not a broker order report";
    ASSERT_EQ(state.at("unconfirmed").size(), 1U);
    EXPECT_EQ(state.at("unconfirmed")[0].at("id"), "unknown");
    EXPECT_THROW(act(session, submit("next", "1")), std::invalid_argument)
        << "the unknown intent still occupies the account risk budget";
    const auto effects = exchange.call<int (*)()>("asterion_fake_catalog_side_effects");
    EXPECT_NO_THROW(act(session, submit("unknown", "3")));
    EXPECT_EQ(exchange.call<int (*)()>("asterion_fake_catalog_side_effects"), effects);
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
    ASSERT_EQ(snapshot.orders_size(), 1);
    EXPECT_EQ(snapshot.orders(0).side(), ::common::SIDE_BUY);
    EXPECT_EQ(snapshot.orders(0).offset(), ::common::OFFSET_OPEN);
    EXPECT_EQ(snapshot.orders(0).price().text(), "3500");
    EXPECT_EQ(snapshot.orders(0).filled_lots().text(), "2");
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
    EXPECT_THROW(act(session, submit("revoked", "1")), std::invalid_argument);
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
    for (const auto* day : {"20260929", "20260930"}) {
        SCOPED_TRACE(day);
        rollover.symbol<void (*)(const char*)>()(day);
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
            EXPECT_EQ(state.at("unconfirmed")[0].at("trading_day"), "20260928");
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
            auto trades = session.trades(day);
            if (trades.trades_size())
                return trades;
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        return session.trades(day);
    };
    {
        LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
        ASSERT_EQ(ready(session).at("phase"), "ready");
        act(session, authorize());
        act(session, submit("filled", "1"));
        const auto trades = recorded(session, "20260928");
        ASSERT_EQ(trades.trades_size(), 1);
        EXPECT_EQ(trades.trades(0).order_id(), "filled");
        EXPECT_EQ(trades.trades(0).lots().text(), "1");
        EXPECT_FALSE(trades.trades(0).price().text().empty());
        EXPECT_EQ(trades.trades(0).trade_time(), "20260928 09:30:00");
        // The counter reports the same fill again after a reconnect.
        exchange.call<void (*)()>("asterion_fake_trader_reconnect");
        ready(session);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        EXPECT_EQ(session.trades("20260928").trades_size(), 1);
        // The broker rebuilt its fill list; a fill after it is still recorded.
        act(session, authorize("authorize.after.reconnect"));
        act(session, submit("after", "1"));
        for (int attempt = 0; attempt < 200 && session.trades("20260928").trades_size() < 2;
            ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        EXPECT_EQ(session.trades("20260928").trades_size(), 2);
    }
    exchange.call<void (*)(const char*)>("asterion_fake_trader_next_day", "20260929");
    LiveSession next_day(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
    EXPECT_TRUE(ready(next_day).at("trades").empty());
    const auto trades = next_day.trades("20260928");
    ASSERT_EQ(trades.trades_size(), 2);
    EXPECT_EQ(trades.trades(0).order_id(), "filled");
    EXPECT_EQ(trades.trading_days_size(), 1);
    EXPECT_EQ(trades.trading_days(0), "20260928");
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
    EXPECT_EQ(state.at("unconfirmed")[0].at("trading_day"), "20260928");
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
        FakeExchange& exchange;

        ~Release()
        {
            exchange.call<void (*)(int)>("asterion_fake_trader_hold_release", 0);
        }
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
            ASSERT_EQ(snapshot.orders_size(), rows);
            ASSERT_EQ(snapshot.trades_size(), rows);
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

namespace {

class StrategyExecution : public Live
{
protected:
    static asterion::trading::StrategyOrderBasis
    basis(asterion::trading::StrategyAuthorityPtr authority, std::uint64_t sequence = 1)
    {
        return {std::move(authority), sequence, 0, true, std::chrono::steady_clock::now() + 10s};
    }
};

} // namespace

namespace {

class StrategyTarget : public Live
{
protected:
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
        quote.source_ms = quote.received_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
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
            session.advance_target(owner).get();
            auto state = session.snapshot();
            if (done(state) || std::chrono::steady_clock::now() >= deadline)
                return state;
            std::this_thread::sleep_for(20ms);
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
    session.accept_target(owner, lots(1, 3), {quote()});
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
    session.accept_target(owner, lots(3), {quote()}, std::chrono::steady_clock::now() + 1500ms);
    session.advance_target(owner).get();
    exchange.call<void (*)(int, int)>("asterion_fake_trader_fill", 0, 1);
    auto state = wait_for(session, [](const Json& s) {
        return !s.at("orders").empty() && s.at("orders")[0].at("status") == "partially_filled";
    });
    ASSERT_EQ(state.at("orders").size(), 1U);
    session.advance_target(owner).get();
    state = wait_for(session, [](const Json& s) {
        return s.at("orders")[0].at("status") == "cancelled";
    });
    EXPECT_EQ(state.at("orders").size(), 1U);
    EXPECT_EQ(decimal(state.at("orders")[0].at("filled")), "1");
    // Expiry drops the batch, not the instance; the next target is admitted.
    EXPECT_TRUE(session.strategy_failure(owner).empty());
    EXPECT_NO_THROW(session.accept_target(owner, lots(3, 2), {quote()}));
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
    EXPECT_NE(std::ranges::find(events, "flattening"), events.end());
    EXPECT_EQ(
        std::ranges::count_if(
            events,
            [](const auto& e) {
                return e.starts_with("sent ");
            }),
        2);
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
            FakeExchange& exchange;

            ~Release()
            {
                exchange.call<void (*)(int)>("asterion_fake_trader_hold_quote_return", 0);
            }
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
            && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(10ms);
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
        return s.at("orders").size() == 1 && s.at("orders")[0].at("status") == "filled";
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
    expired.expires_at = std::chrono::steady_clock::now() - 1s;
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
    target.expires_at = std::chrono::steady_clock::now() + 3s;
    HeldCommit held;
    auto sending = session.execute_strategy(target, submit("expired.target", "1"));
    ASSERT_TRUE(held.waiting());
    std::this_thread::sleep_until(target.expires_at + 20ms);
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
        FakeExchange& exchange;

        ~Release()
        {
            exchange.call<void (*)(int)>("asterion_fake_trader_hold_quote_return", 0);
        }
    } release{exchange};

    exchange.call<void (*)(int)>("asterion_fake_trader_hold_quote_return", 1);
    auto target = basis(owner);
    target.expires_at = std::chrono::steady_clock::now() + 2s;
    const auto before = session.snapshot().at("capacity").at("records_used");
    auto sending = session.execute_strategy(target, submit("quote.expired", "1"));
    std::this_thread::sleep_until(target.expires_at + 20ms);
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
        FakeExchange& exchange;

        ~Release()
        {
            exchange.call<void (*)(int, int)>("asterion_fake_trader_hold_insert_return", 0, 0);
        }
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
    act(session, submit("manual.resting", "3"));
    auto owner = session.register_strategy().get();
    session.execute_strategy(basis(owner), submit("strategy.first", "3")).get();
    session.execute_strategy(basis(owner, 2), submit("strategy.second", "3")).get();
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

TEST_F(StrategyExecution, UnknownSubmissionAndFailedCancellationKeepRiskAcrossRestart)
{
    {
        LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
        ASSERT_EQ(ready(session).at("phase"), "ready");
        act(session, authorize());
        auto owner = session.register_strategy().get();
        const auto records = number(session.snapshot().at("capacity").at("records_used"));
        exchange.call<void (*)(int)>("asterion_fake_trader_cancel_code", -2);
        EXPECT_THROW(session.execute_strategy(basis(owner), submit("unknown", "3")).get(), Error);
        // Stopping attempts the one cancel, which fails and is never retried.
        session.release_strategy(owner).get();
        const auto after = wait_for(session, [records](const Json& state) {
            return number(state.at("capacity").at("records_used")) >= records + 3;
        });
        EXPECT_EQ(number(after.at("capacity").at("records_used")), records + 3);
        ASSERT_EQ(after.at("unconfirmed").size(), 1U);
        EXPECT_EQ(after.at("unconfirmed")[0].at("id"), "unknown");
        EXPECT_TRUE(after.at("orders").empty());
    }
    exchange.call<void (*)(int)>("asterion_fake_trader_cancel_code", 0);
    LiveSession recovered(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
    ASSERT_EQ(ready(recovered).at("phase"), "ready");
    EXPECT_EQ(recovered.snapshot().at("unconfirmed").size(), 1U);
    act(recovered, authorize("recovered"));
    EXPECT_THROW(act(recovered, submit("blocked.by.unknown", "1")), std::invalid_argument);
}

TEST_F(StrategyExecution, BrokerRejectionEndsTheInstanceAndCancelsItsOtherOrder)
{
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest(2));
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    auto owner = session.register_strategy().get();
    session.execute_strategy(basis(owner), submit("resting", "3")).get();
    exchange.call<void (*)(int)>("asterion_fake_trader_reject_inserts", 1);
    session.execute_strategy(basis(owner, 2), submit("rejected", "1")).get();
    const auto after = wait_for(session, [](const Json& state) {
        return order_has_status(state, "resting", "cancelled")
            && order_has_status(state, "rejected", "rejected");
    });
    EXPECT_TRUE(order_has_status(after, "resting", "cancelled"));
    EXPECT_TRUE(order_has_status(after, "rejected", "rejected"));
    EXPECT_EQ(session.strategy_failure(owner), "broker rejected a strategy order");
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
        && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
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
