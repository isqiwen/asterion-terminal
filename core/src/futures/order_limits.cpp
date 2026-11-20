#include <asterion/futures/order_limits.hpp>
#include <limits>
#include <set>
#include <stdexcept>

namespace asterion {

void OrderLimitsConfig::validate() const
{
    if (max_order_quantity <= Decimal{} || max_gross_quantity <= Decimal{}
        || max_contract_quantity <= Decimal{} || max_working_orders == 0)
        throw std::invalid_argument("risk limits must be positive");
    if (max_contract_quantity > max_gross_quantity)
        throw std::invalid_argument("contract risk limit cannot exceed account gross limit");
    if (max_daily_loss < Decimal{} || max_drawdown < Decimal{} || max_margin_ratio < Decimal{}
        || max_margin_ratio > Decimal::parse("1"))
        throw std::invalid_argument(
            "account loss, drawdown and margin ratio limits must be nonnegative and margin ratio at most 1");
}

Json encode_order_limits(const OrderLimitsConfig& config)
{
    config.validate();
    Json out{
        {"max_order_quantity", config.max_order_quantity.str()},
        {"max_gross_quantity", config.max_gross_quantity.str()},
        {"max_working_orders", config.max_working_orders},
        {"max_contract_quantity", config.max_contract_quantity.str()}
    };
    if (config.max_daily_loss > Decimal{})
        out["max_daily_loss"] = config.max_daily_loss.str();
    if (config.max_drawdown > Decimal{})
        out["max_drawdown"] = config.max_drawdown.str();
    if (config.max_margin_ratio > Decimal{})
        out["max_margin_ratio"] = config.max_margin_ratio.str();
    if (config.max_daily_cancels_per_contract > 0)
        out["max_daily_cancels_per_contract"] = config.max_daily_cancels_per_contract;
    return out;
}

OrderLimitsConfig decode_order_limits(const Json& value)
{
    if (!value.is_object())
        throw Error(ErrorCode::invalid_request, "unexpected object fields");
    static const std::set<std::string_view> allowed_keys{
        "max_order_quantity",
        "max_gross_quantity",
        "max_contract_quantity",
        "max_working_orders",
        "max_daily_loss",
        "max_drawdown",
        "max_margin_ratio",
        "max_daily_cancels_per_contract"
    };
    for (const auto& [key, _] : value.items())
        if (!allowed_keys.contains(key))
            throw Error(ErrorCode::invalid_request, "unexpected object fields");
    for (const auto* required :
        {"max_order_quantity", "max_gross_quantity", "max_contract_quantity", "max_working_orders"})
        if (!value.contains(required))
            throw Error(ErrorCode::invalid_request, "missing object field");
    if (!value.at("max_working_orders").is_number_unsigned()
        || value.at("max_working_orders").get<std::uint64_t>()
            > std::numeric_limits<std::size_t>::max())
        throw std::invalid_argument("risk working order limit must be an unsigned integer");
    auto decimal = [&](const char* key) {
        const auto text = value.at(key).get<std::string>();
        const auto result = Decimal::parse(text);
        if (result.str() != text)
            throw std::invalid_argument("risk quantity must use canonical decimal text");
        return result;
    };
    auto optional_positive_decimal = [&](const char* key) {
        if (!value.contains(key))
            return Decimal{};
        const auto result = decimal(key);
        if (result <= Decimal{})
            throw std::invalid_argument("optional risk limit must be positive");
        return result;
    };
    std::uint32_t max_cancels = 0;
    if (value.contains("max_daily_cancels_per_contract")) {
        const auto& raw = value.at("max_daily_cancels_per_contract");
        const bool ok = raw.is_number_unsigned()
            ? (raw.get<std::uint64_t>() > 0
                  && raw.get<std::uint64_t>() <= std::numeric_limits<std::uint32_t>::max())
            : (raw.is_number_integer() && raw.get<std::int64_t>() > 0
                  && raw.get<std::int64_t>() <= std::numeric_limits<std::uint32_t>::max());
        if (!ok)
            throw std::invalid_argument("daily cancel limit must be a positive 32-bit integer");
        max_cancels = raw.get<std::uint32_t>();
    }
    OrderLimitsConfig result{
        decimal("max_order_quantity"),
        decimal("max_gross_quantity"),
        value.at("max_working_orders").get<std::size_t>(),
        decimal("max_contract_quantity"),
        optional_positive_decimal("max_daily_loss"),
        optional_positive_decimal("max_drawdown"),
        optional_positive_decimal("max_margin_ratio"),
        max_cancels
    };
    result.validate();
    return result;
}

OrderLimits::OrderLimits(OrderLimitsConfig config)
    : config_(config)
{
    config_.validate();
}

RiskDecision OrderLimits::evaluate(const PreTradeRiskContext& c) const
{
    c.order.validate(c.instrument);
    if ((c.offset != Offset::open && c.offset != Offset::close_today
            && c.offset != Offset::close_yesterday && c.offset != Offset::close)
        || c.gross_position_quantity < Decimal{} || c.pending_open_quantity < Decimal{}
        || c.account_gross_quantity < Decimal{} || c.account_pending_open_quantity < Decimal{}
        || c.daily_loss < Decimal{} || c.drawdown < Decimal{} || c.account_margin < Decimal{}
        || !c.gross_position_quantity.multiple_of(c.instrument.quantity_increment)
        || !c.pending_open_quantity.multiple_of(c.instrument.quantity_increment))
        return {RiskReason::invalid_context};
    if (c.order.quantity > config_.max_order_quantity)
        return {RiskReason::order_quantity};
    if (c.offset == Offset::open) {
        if (config_.max_daily_loss > Decimal{} && c.daily_loss >= config_.max_daily_loss)
            return {RiskReason::daily_loss};
        if (config_.max_drawdown > Decimal{} && c.drawdown >= config_.max_drawdown)
            return {RiskReason::drawdown};
        if (config_.max_margin_ratio > Decimal{}
            && (c.account_equity <= Decimal{}
                || c.account_margin
                    > multiply(c.account_equity, config_.max_margin_ratio, Rounding::toward_zero)))
            return {RiskReason::margin_ratio};
        if (config_.max_daily_cancels_per_contract > 0
            && c.contract_daily_cancels >= config_.max_daily_cancels_per_contract)
            return {RiskReason::daily_cancels};
        if (c.working_orders >= config_.max_working_orders)
            return {RiskReason::working_orders};
        // Subtract from headroom to avoid overflowing sums for an already
        // over-limit account. Reaching the exact configured bound is allowed.
        const auto within = [&](Decimal held, Decimal pending, Decimal bound) {
            if (held > bound)
                return false;
            const auto headroom = bound - held;
            return pending <= headroom && c.order.quantity <= headroom - pending;
        };
        if (!within(
                c.gross_position_quantity,
                c.pending_open_quantity,
                config_.max_contract_quantity)
            || !within(
                c.account_gross_quantity,
                c.account_pending_open_quantity,
                config_.max_gross_quantity))
            return {RiskReason::gross_quantity};
    }
    return {RiskReason::allowed};
}

void RiskExposure::add_unreconciled_open(Decimal quantity)
{
    account_gross_quantity = account_gross_quantity + quantity;
}

void RiskExposure::add_position(
    const InstrumentId& target,
    const InstrumentId& instrument,
    Decimal quantity)
{
    account_gross_quantity = account_gross_quantity + quantity;
    if (instrument == target)
        gross_position_quantity = gross_position_quantity + quantity;
}

void RiskExposure::add_working_order(
    const InstrumentId& target,
    const InstrumentId& instrument,
    Offset offset,
    Decimal remaining_quantity)
{
    ++working_orders;
    if (offset == Offset::open) {
        account_pending_open_quantity = account_pending_open_quantity + remaining_quantity;
        if (instrument == target)
            pending_open_quantity = pending_open_quantity + remaining_quantity;
    }
}

PreTradeRiskContext RiskExposure::context(
    const Instrument& instrument,
    const LimitOrder& order,
    Offset offset) const noexcept
{
    return {
        instrument,
        order,
        offset,
        gross_position_quantity,
        pending_open_quantity,
        working_orders,
        account_gross_quantity,
        account_pending_open_quantity,
    };
}

RiskDecision assess_order(
    const OrderLimits& risk,
    const FuturesAccount& account,
    const LimitOrder& order,
    Offset offset)
{
    const auto& instrument =
        account.contracts()[account.contract_index(order.instrument)].instrument;
    RiskExposure exposure;
    for (const auto& lot : account.positions())
        exposure.add_position(order.instrument, lot.instrument, lot.quantity);
    for (const auto index : account.working_orders()) {
        const auto& item = account.orders()[index];
        exposure.add_working_order(
            order.instrument,
            item.order.request().instrument,
            item.offset,
            item.order.remaining_quantity());
    }
    return risk.evaluate(exposure.context(instrument, order, offset));
}

} // namespace asterion
