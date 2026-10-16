#pragma once
#include "risk_module.hpp"
#include <asterion/domain/futures.hpp>
#include <asterion/v1/trading.pb.h>

namespace asterion::trading {

// Validated policy values shared by admission, preparation and the account owner.
struct PolicyDefinition
{
    OrderLimitsConfig limits;
    Decimal max_price_deviation;
    std::vector<FuturesContract> contracts;
    static PolicyDefinition from_wire(const protocol::v1::LivePolicy& policy);
};

// One immutable policy owns its algorithm and typed parameters. It owns no
// positions or order reservations: those remain facts of the account.
struct AccountPolicy
{
    std::string revision;
    PolicyDefinition definition;
    risk_providers::Module algorithm;
    std::shared_ptr<RiskPort> risk;

    AccountPolicy(
        PolicyDefinition definition,
        std::string revision,
        risk_providers::Module algorithm);
    const FuturesContract* find(const InstrumentId& id) const;
    void preserve_exposure(const AccountPolicy& previous, const InstrumentId& id) const;
    // Digest directories retain every historical policy's exact algorithm bytes.
    void capture(const std::filesystem::path& plugins) const;
};

} // namespace asterion::trading
