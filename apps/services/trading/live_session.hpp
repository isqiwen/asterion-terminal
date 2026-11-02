#pragma once
#include "account_records.hpp"
#include "strategy_execution.hpp"
#include <asterion/futures/live_market.hpp>
#include <asterion/trading.pb.h>
#include <asterion/base/serialization.hpp>
#include <filesystem>
#include <asterion/system/progress.hpp>
#include <future>
#include <memory>
#include <string>
#include <string_view>

namespace asterion::trading {

// Thread-safe entry point for one account. Accepted operations run in order on
// its account thread; shutdown drains them before destroying account resources.
// Callers authenticate and validate the request envelope before entry. Admission
// fixes each command's control revision; revoke/disconnect close send permission
// before the account processes their state changes.
class LiveSession final
{
public:
    LiveSession(
        std::filesystem::path directory,
        const std::filesystem::path& ctp_library,
        const std::filesystem::path& ownership_directory,
        const Json& create_manifest = nullptr);
    ~LiveSession();
    LiveSession(const LiveSession&) = delete;
    LiveSession& operator=(const LiveSession&) = delete;
    // Initialization runs on the account owner. Await it before submitting operations.
    std::shared_future<void> initialized() const;
    // Quiesce mutation admission and invalidate sends; accepted replies may still read.
    void close_admission();
    // After reply drain, stop all resources on their owners. Completion includes SDK release.
    void stop();
    std::shared_future<void> stopped() const;
    std::future<void> connect(std::string password, std::string auth_code);
    std::future<void> disconnect();
    std::future<void> query_costs();
    // Admission is bounded; completion acknowledges the durable command outcome.
    [[nodiscard]] std::future<void>
    execute(std::string_view account_id, std::string_view policy_revision, AccountRequest request);
    // Internal C++ execution boundary. Registration does not authorize trading.
    std::future<StrategyAuthorityPtr> register_strategy(
        std::string instance,
        std::vector<InstrumentId> contracts,
        std::optional<StrategyWeightBudget> budget = std::nullopt);
    std::future<void> release_strategy(StrategyAuthorityPtr authority);
    std::future<std::string> strategy_failure(StrategyAuthorityPtr authority) const;
    std::future<void> pause_strategy(StrategyAuthorityPtr authority, bool paused);
    std::future<void>
    flatten_strategy(StrategyAuthorityPtr authority, std::vector<MarketQuote> quotes);
    std::future<std::vector<std::string>> take_strategy_events(StrategyAuthorityPtr authority);
    std::future<void> execute_strategy(StrategyOrderBasis basis, SubmitOrder order);
    std::future<void> accept_strategy_target(
        StrategyAuthorityPtr authority,
        strategy::proto::Decision decision,
        std::vector<MarketQuote> quotes,
        std::chrono::steady_clock::time_point expires_at);
    // `quotes`: the freshest direct observations; each leg is priced from them.
    std::future<void>
    advance_strategy_target(StrategyAuthorityPtr authority, std::vector<MarketQuote> quotes = {});
    // Detached transport value constructed on the account owner; JSON belongs to Terminal.
    std::future<protocol::proto::LiveSnapshot> snapshot() const;

    bool recovery_required() const noexcept;

    struct Health
    {
        Progress::Observation state, persistence, command;
        bool business_ready;
    };

    Health health() const noexcept;

private:
    struct Loop;
    std::unique_ptr<Loop> loop_;
};

} // namespace asterion::trading
