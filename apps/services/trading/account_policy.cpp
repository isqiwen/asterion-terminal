#include "account_policy.hpp"
#include <asterion/protocol/trading.hpp>

namespace asterion::trading {

PolicyDefinition PolicyDefinition::from_wire(const ::trading::Policy& policy)
{
    protocol::validate_policy(policy);
    PolicyDefinition result;
    const auto gross = Decimal::parse(policy.limits().max_gross_lots().text());
    result.limits = {
        Decimal::parse(policy.limits().max_order_lots().text()),
        gross,
        policy.limits().max_working_orders(),
        policy.limits().has_max_contract_lots()
                && !policy.limits().max_contract_lots().text().empty()
            ? Decimal::parse(policy.limits().max_contract_lots().text())
            : gross
    };
    result.max_price_deviation = Decimal::parse(policy.max_price_deviation().text());
    for (const auto& contract : policy.contracts())
        result.contracts.push_back(
            {protocol::terms(contract), contract.product(), contract.delivery_month()});
    return result;
}

AccountPolicy::AccountPolicy(PolicyDefinition value, std::string id)
    : revision(std::move(id)),
      definition(std::move(value))
{
    validate_id(revision);
    auto instance = std::make_shared<OrderLimits>(definition.limits);
    instance->start();
    risk = std::move(instance);
}

const FuturesContract* AccountPolicy::find(const InstrumentId& id) const
{
    for (const auto& contract : definition.contracts)
        if (contract.instrument.id == id)
            return &contract;
    return nullptr;
}

void AccountPolicy::preserve_exposure(const AccountPolicy& previous, const InstrumentId& id) const
{
    const auto* next = find(id);
    if (!next)
        throw Error(
            ErrorCode::conflict,
            "policy must retain contracts with positions or working orders");
    const auto* old = previous.find(id);
    if (old
        && (old->instrument.quote_currency != next->instrument.quote_currency
            || old->instrument.price_increment != next->instrument.price_increment
            || old->instrument.quantity_increment != next->instrument.quantity_increment
            || old->instrument.multiplier != next->instrument.multiplier
            || old->product != next->product || old->delivery_month != next->delivery_month))
        throw Error(
            ErrorCode::conflict,
            "policy cannot change contract units while exposure exists");
}

} // namespace asterion::trading
