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

    // Each contract as research names it, EXCHANGE/product/YYYY-MM, from the policy.
    const std::vector<std::string>& symbols() const noexcept
    {
        return symbols_;
    }

private:
    friend class LiveAccountState;

    StrategyAuthority(
        std::string instance,
        std::vector<InstrumentId> contracts,
        std::vector<std::string> symbols,
        std::string policy)
        : instance_(std::move(instance)),
          contracts_(std::move(contracts)),
          symbols_(std::move(symbols)),
          policy_(std::move(policy))
    {}

    std::string instance_;
    std::vector<InstrumentId> contracts_;
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
