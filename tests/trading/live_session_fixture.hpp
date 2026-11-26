#pragma once
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
#include <set>
#include <sys/resource.h>

namespace asterion::test::live {

using namespace std::chrono_literals;
namespace fs = std::filesystem;

inline std::shared_ptr<ManualClock> test_clock;

// Blocking driver for account behavior tests. Production callers await these
// futures without occupying their I/O thread.
class LiveSession
{
public:
    explicit LiveSession(
        const fs::path& journal,
        const fs::path& ctp_library,
        const fs::path& ownership_directory,
        const Json& create_manifest = nullptr,
        std::shared_ptr<asterion::trading::AlertWebhook> alerts = nullptr,
        std::shared_ptr<const Clock> clock = test_clock)
        : account_(account_of(journal, create_manifest)),
          clock_(clock ? std::move(clock) : default_clock()),
          session_(
              journal,
              ctp_library,
              ownership_directory,
              create_manifest,
              std::move(alerts),
              clock_)
    {
        session_.initialized().get();
    }

    ~LiveSession()
    {
        if (test_clock) {
            session_.stop();
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            while (session_.stopped().wait_for(5ms) != std::future_status::ready
                && std::chrono::steady_clock::now() < deadline)
                test_clock->advance(50ms);
        }
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

    ::trading::Day day(
        std::string day,
        std::uint32_t offset = 0,
        std::uint32_t limit = 200,
        bool oldest_first = false) const
    {
        return session_.day(std::move(day), offset, limit, oldest_first).get();
    }

    ::trading::Orders orders(std::uint32_t offset = 0, std::uint32_t limit = 200) const
    {
        return session_.orders(offset, limit).get();
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
        const auto collect = [&](auto read, const char* field) {
            auto rows = Json::array();
            for (std::uint32_t offset = 0;; offset += 200) {
                const auto reply = read(offset);
                std::string bytes;
                if (!google::protobuf::util::MessageToJsonString(reply, &bytes, options).ok())
                    throw std::runtime_error("cannot print the history page");
                const auto page = plain(Json::parse(bytes));
                for (const auto& row : page.at(field))
                    rows.push_back(row);
                if (offset + 200 >= reply.total())
                    break;
            }
            std::reverse(rows.begin(), rows.end());
            return rows;
        };
        const auto order_rows = collect(
            [&](auto offset) {
                return orders(offset);
            },
            "orders");
        const auto trade_rows = collect(
            [&](auto offset) {
                return day("", offset);
            },
            "trades");
        // Reconciliation must be observed after the fills used by a wait predicate.
        std::string text;
        if (!google::protobuf::util::MessageToJsonString(wire_snapshot(), &text, options).ok())
            throw std::runtime_error("cannot print the snapshot");
        auto state = plain(Json::parse(text));
        state["orders"] = order_rows;
        state["trades"] = trade_rows;
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
            {"max_contract_quantity", policy.at("limits").at("max_contract_lots")},
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
        std::optional<asterion::trading::StrategyWeightBudget> budget = std::nullopt,
        asterion::trading::StrategyExecutionOptions execution = {})
    {
        return session_
            .register_strategy(std::move(instance), std::move(contracts), budget, {}, execution);
    }

    void accept_target(
        asterion::trading::StrategyAuthorityPtr authority,
        ::strategy::Decision decision,
        std::vector<MarketQuote> quotes,
        std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt)
    {
        session_
            .accept_strategy_target(
                std::move(authority),
                std::move(decision),
                std::move(quotes),
                deadline.value_or(steady_now(*clock_) + 10s))
            .get();
    }

    struct AdvanceFuture
    {
        std::future<void> future;

        void get()
        {
            if (test_clock) {
                const auto deadline = std::chrono::steady_clock::now() + 15s;
                while (future.wait_for(5ms) != std::future_status::ready) {
                    if (std::chrono::steady_clock::now() >= deadline)
                        throw std::runtime_error("test strategy target step did not complete");
                    if (!sqlite::commit_waiting_for_testing())
                        test_clock->advance(50ms);
                }
            }
            future.get();
        }

        template <class Rep, class Period>
        std::future_status wait_for(std::chrono::duration<Rep, Period> timeout) const
        {
            return future.wait_for(timeout);
        }
    };

    AdvanceFuture advance_target(
        asterion::trading::StrategyAuthorityPtr authority,
        std::vector<MarketQuote> quotes = {})
    {
        return {session_.advance_strategy_target(std::move(authority), std::move(quotes))};
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
        AdvanceFuture{session_.flatten_strategy(std::move(authority), std::move(quotes))}.get();
    }

    auto strategy_events(asterion::trading::StrategyAuthorityPtr authority)
    {
        return session_.take_strategy_events(std::move(authority)).get();
    }

    void advance_quotes(std::vector<MarketQuote> quotes)
    {
        session_.observe_quotes(std::move(quotes)).get();
    }

    AdvanceFuture execute_strategy(asterion::trading::StrategyOrderBasis basis, const Json& command)
    {
        auto request =
            asterion::trading::AccountRequest::from_wire(protocol::encode_command(command));
        return {session_.execute_strategy(
            std::move(basis),
            std::get<asterion::trading::SubmitOrder>(request.operation))};
    }

private:
    static std::string account_of(const fs::path& journal, const Json& create_manifest)
    {
        if (create_manifest.is_object() && create_manifest.contains("account_id"))
            return create_manifest.at("account_id").get<std::string>();
        return asterion::read_journal_header(journal, asterion::trading::AccountJournal::sidecars)
            .at("account_id");
    }

    // The account the journal records: the manifest's, or a recovered journal's.
    std::string account_;
    std::shared_ptr<const Clock> clock_;
    asterion::trading::LiveSession session_;
};

// Test control entry point exported by the CTP SDK double. The handle keeps
// the library, and so its exchange state, loaded across sessions.
struct FakeExchange
{
    void reset()
    {
        library.symbol<void (*)()>()();
        if (test_clock)
            set_clock(test_clock.get());
    }

    template <class F, class... Args>
    auto call(const char* name, Args... args)
    {
        ctp::SharedLibrary control(ASTERION_TEST_CTP_TRADER, name);
        return control.symbol<F>()(args...);
    }

    void set_clock(const Clock* clock)
    {
        using ClockFn = std::int64_t (*)(void*);
        const ClockFn monotonic = clock
            ? +[](void* ctx) { return static_cast<const Clock*>(ctx)->monotonic_now(); }
            : nullptr;
        const ClockFn utc = clock
            ? +[](void* ctx) { return static_cast<const Clock*>(ctx)->utc_now(); }
            : nullptr;
        call<void (*)(ClockFn, ClockFn, void*)>(
            "asterion_fake_trader_set_clock",
            monotonic,
            utc,
            const_cast<Clock*>(clock));
    }

    ctp::SharedLibrary library{ASTERION_TEST_CTP_TRADER, "asterion_fake_trader_reset"};
};

struct Directory
{
    Directory()
    {
        fs::create_directory(path);
    }

    ~Directory()
    {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }

    fs::path path = fs::temp_directory_path() / ("asterion-live-" + unique_process_id());
};

inline Json contract(const char* symbol, const char* product = "rb", const char* month = "2026-10")
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
inline Json manifest(std::uint64_t working_orders = 1, const char* deviation = "0.02")
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
                     {"max_contract_quantity", "10"},
                     {"max_working_orders", working_orders}}},
                {"max_price_deviation", deviation},
                {"contracts", Json::array({contract("rb2610")})}}}
    };
}

inline Json submit(
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

inline Json authorize(std::string id = "authorize", std::string user = "000001")
{
    return {{"request_id", std::move(id)}, {"action", "live_authorize"}, {"user_id", user}};
}

// ProtoJSON prints 64-bit integers as strings.
inline std::int64_t number(const Json& value)
{
    return std::stoll(value.get<std::string>());
}

inline std::string decimal(const Json& value)
{
    return Decimal::parse(value.get<std::string>()).str();
}

inline Json find_order(const Json& snapshot, const std::string& id)
{
    for (const auto& order : snapshot.at("orders"))
        if (order.at("id") == id)
            return order;
    return nullptr;
}

inline bool order_has_status(const Json& state, const std::string& id, std::string_view status)
{
    const auto order = find_order(state, id);
    return order.is_object() && order.at("status") == status;
}

template <class F>
Json wait_for(const LiveSession& session, F done, std::chrono::milliseconds step = 50ms)
{
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    auto state = session.snapshot();
    while (!done(state) && std::chrono::steady_clock::now() < deadline) {
        if (test_clock)
            test_clock->advance(step);
        state = session.snapshot();
        if (done(state))
            break;
        std::this_thread::sleep_for(test_clock ? 5ms : 20ms);
    }
    return state;
}

inline void act(LiveSession& session, const Json& command)
{
    const auto state = session.snapshot();
    auto result = session.execute(
        state.at("account_id").get<std::string>(),
        state.at("policy_revision").get<std::string>(),
        command);
    if (test_clock) {
        const auto action = command.at("action").get<std::string>();
        const bool uses_broker_pacing =
            action == "submit" || action == "cancel" || action == "live_revoke";
        const auto deadline = std::chrono::steady_clock::now() + 15s;
        while (result.wait_for(5ms) != std::future_status::ready) {
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("test account command did not complete");
            if (uses_broker_pacing && !sqlite::commit_waiting_for_testing())
                test_clock->advance(50ms);
        }
    }
    result.get();
}

inline Json policy_command(const LiveSession& session, Json definition)
{
    return {
        {"request_id", "policy.change"},
        {"action", "live_policy"},
        {"policy", std::move(definition)}
    };
}

inline Json ready(LiveSession& session)
{
    session.connect("secret", "auth-code");
    auto state = wait_for(
        session,
        [](const Json& s) {
            return s.at("phase") == "ready";
        },
        250ms);
    if (test_clock)
        test_clock->advance(1100ms);
    return state;
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
        while (
            !sqlite::commit_waiting_for_testing() && std::chrono::steady_clock::now() < deadline) {
            if (test_clock)
                test_clock->advance(50ms);
            std::this_thread::sleep_for(5ms);
        }
        return sqlite::commit_waiting_for_testing();
    }
};

class Live : public ::testing::Test
{
protected:
    void SetUp() override
    {
        exchange.reset();
        clock = std::make_shared<ManualClock>(1'790'557'200'000'000'000LL);
        test_clock = clock;
        exchange.set_clock(clock.get());
    }

    void TearDown() override
    {
        exchange.set_clock(nullptr);
        test_clock.reset();
    }

    FakeExchange exchange;
    std::shared_ptr<ManualClock> clock;
    Directory directory;
    Directory owners;
};

} // namespace asterion::test::live
