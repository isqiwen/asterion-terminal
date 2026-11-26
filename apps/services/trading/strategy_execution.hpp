#pragma once
#include "broker_execution.hpp"
#include <asterion/futures/market.hpp>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace asterion::trading {

class LiveAccountState;

struct StrategyWeightBudget
{
    Decimal gross, net;
    Decimal max_margin_ratio = {};
    Decimal min_rebalance_weight = {};
};

struct StrategyExecutionOptions
{
    std::uint32_t aggressive_ticks = 0;
    std::uint32_t max_reprices = 0;
    std::uint32_t reprice_interval_ms = 0;
    std::uint32_t twap_slices = 1;
    std::uint32_t twap_interval_ms = 0;
};

StrategyExecutionOptions normalize_execution_options(StrategyExecutionOptions options);
std::chrono::milliseconds strategy_execution_window(const StrategyExecutionOptions& options);

struct DominantSeriesSpec
{
    std::string symbol;
    std::vector<InstrumentId> candidates;
    std::size_t initial_index = 0;
};

// An account-issued, process-local identity. A restarted instance receives a
// different authority even if a caller repeats its name. Never serialized or
// exposed through the Python or remote order protocol.
class StrategyAuthority final
{
public:
    const std::string& instance() const noexcept
    {
        return instance_;
    }

    // Each contract or dominant series as research names it, in the instance's order.
    const std::vector<std::string>& symbols() const noexcept
    {
        return symbols_;
    }

private:
    friend class LiveAccountState;

    StrategyAuthority(
        std::string instance,
        std::vector<InstrumentId> contracts,
        std::vector<DominantSeriesSpec> series,
        std::vector<std::string> symbols,
        std::string policy)
        : instance_(std::move(instance)),
          contracts_(std::move(contracts)),
          series_(std::move(series)),
          symbols_(std::move(symbols)),
          policy_(std::move(policy))
    {}

    std::string instance_;
    std::vector<InstrumentId> contracts_;
    std::vector<DominantSeriesSpec> series_;
    std::vector<std::string> symbols_;
    std::string policy_;
};

using StrategyAuthorityPtr = std::shared_ptr<const StrategyAuthority>;

// The C++ target executor supplies one planned leg, never the Python child.
// A target starts at leg zero; further legs share its original deadline.
struct StrategyOrderBasis
{
    StrategyAuthorityPtr authority;
    std::uint64_t target_sequence = 0;
    std::uint32_t leg = 0;
    bool ready = false;
    std::chrono::steady_clock::time_point expires_at;
    std::optional<BrokerQuote> quote = std::nullopt;
};

} // namespace asterion::trading
