#pragma once
#include <asterion/domain/order_limits.hpp>
#include <asterion/domain/futures.hpp>
#include <asterion/trading.pb.h>

namespace asterion::trading {

// Validated policy values shared by admission, preparation and the account owner.
struct PolicyDefinition
{
    OrderLimitsConfig limits;
    Decimal max_price_deviation;
    std::vector<FuturesContract> contracts;
    static PolicyDefinition from_wire(const protocol::proto::LivePolicy& policy);
};

// One immutable policy owns its typed parameters and built-in limits. It owns no
// positions or order reservations: those remain facts of the account.
struct AccountPolicy
{
    std::string revision;
    PolicyDefinition definition;
    std::shared_ptr<RiskPort> risk;

    AccountPolicy(PolicyDefinition definition, std::string revision);
    const FuturesContract* find(const InstrumentId& id) const;
    void preserve_exposure(const AccountPolicy& previous, const InstrumentId& id) const;
};

} // namespace asterion::trading
