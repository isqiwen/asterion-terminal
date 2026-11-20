#pragma once
#include <asterion/base/serialization.hpp>
#include <asterion/futures/account.hpp>
#include <cstddef>
#include <string_view>

namespace asterion {

// Gross pre-submit exposure; unfilled close orders do not reduce potential exposure.
struct PreTradeRiskContext
{
    const Instrument& instrument;
    const LimitOrder& order;
    Offset offset;
    Decimal gross_position_quantity;
    Decimal pending_open_quantity;
    std::size_t working_orders;
    Decimal account_gross_quantity = gross_position_quantity;
    Decimal account_pending_open_quantity = pending_open_quantity;
    Decimal daily_loss = {};
    Decimal drawdown = {};
    Decimal account_equity = {};
    Decimal account_margin = {};
    std::uint32_t contract_daily_cancels = 0;
};

// Shared pre-trade exposure aggregation for backtest and live accounts.
struct RiskExposure
{
    void add_unreconciled_open(Decimal quantity);
    void add_position(const InstrumentId& target, const InstrumentId& instrument, Decimal quantity);
    void add_working_order(
        const InstrumentId& target,
        const InstrumentId& instrument,
        Offset offset,
        Decimal remaining_quantity);
    [[nodiscard]] PreTradeRiskContext
    context(const Instrument& instrument, const LimitOrder& order, Offset offset) const noexcept;

    Decimal gross_position_quantity;
    Decimal pending_open_quantity;
    std::size_t working_orders = 0;
    Decimal account_gross_quantity;
    Decimal account_pending_open_quantity;
};

enum class RiskReason
{
    allowed,
    invalid_context,
    order_quantity,
    gross_quantity,
    working_orders,
    insufficient_funds,
    daily_loss,
    drawdown,
    margin_ratio,
    daily_cancels
};

constexpr std::string_view risk_reason_name(RiskReason reason) noexcept
{
    switch (reason) {
        case RiskReason::allowed:
            return "allowed";
        case RiskReason::invalid_context:
            return "invalid_context";
        case RiskReason::order_quantity:
            return "order_quantity";
        case RiskReason::gross_quantity:
            return "gross_quantity";
        case RiskReason::working_orders:
            return "working_orders";
        case RiskReason::insufficient_funds:
            return "insufficient_funds";
        case RiskReason::daily_loss:
            return "daily_loss";
        case RiskReason::drawdown:
            return "drawdown";
        case RiskReason::margin_ratio:
            return "margin_ratio";
        case RiskReason::daily_cancels:
            return "daily_cancels";
    }
    return "invalid_context";
}

struct RiskDecision
{
    [[nodiscard]] bool allowed() const noexcept
    {
        return reason == RiskReason::allowed;
    }

    RiskReason reason;
};

struct OrderLimitsConfig
{
    void validate() const;

    Decimal max_order_quantity;
    Decimal max_gross_quantity;
    std::size_t max_working_orders;
    Decimal max_contract_quantity;
    Decimal max_daily_loss = {};
    Decimal max_drawdown = {};
    Decimal max_margin_ratio = {};
    std::uint32_t max_daily_cancels_per_contract = 0;
};

Json encode_order_limits(const OrderLimitsConfig& config);
OrderLimitsConfig decode_order_limits(const Json& value);

// Per-contract and account limits, with pending opening orders reserved at
// their full remaining size. No credit for cancellations or closes not filled.
class OrderLimits
{
public:
    explicit OrderLimits(OrderLimitsConfig config);
    RiskDecision evaluate(const PreTradeRiskContext& context) const;

private:
    OrderLimitsConfig config_;
};

RiskDecision assess_order(
    const OrderLimits& risk,
    const FuturesAccount& account,
    const LimitOrder& order,
    Offset offset);

} // namespace asterion
