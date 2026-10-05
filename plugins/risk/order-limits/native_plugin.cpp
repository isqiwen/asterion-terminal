#include "order_limits.hpp"
#include <asterion/plugin/risk.h>
#include <asterion/plugin/sdk.hpp>
#include <charconv>
#include <cstring>
#include <limits>
#include <memory>
using namespace asterion;
namespace {
std::string text(const char* value) {
  if (!value || strnlen(value, 257) == 257)
    throw std::invalid_argument("invalid risk text");
  return value;
}
AstStatus create(const char* capability, const AstSetting* settings, uint32_t count,
                 void** out) noexcept {
  if (!out)
    return AST_INVALID;
  *out = nullptr;
  return sdk::boundary([&] {
    if (!capability || std::string_view(capability) != AST_RISK_V1)
      throw AstStatus(AST_UNSUPPORTED);
    if (count != 3)
      throw AstStatus(AST_INVALID);
    const auto quantity = sdk::setting(settings, count, "max_order_quantity");
    const auto gross = sdk::setting(settings, count, "max_gross_quantity");
    const auto orders = sdk::setting(settings, count, "max_working_orders");
    std::size_t maximum = 0;
    const auto [end, error] =
        std::from_chars(orders.data(), orders.data() + orders.size(), maximum);
    if (error != std::errc{} || end != orders.data() + orders.size() ||
        std::to_string(maximum) != orders)
      throw AstStatus(AST_INVALID);
    const OrderLimitsConfig config{Decimal::parse(quantity), Decimal::parse(gross), maximum};
    if (config.max_order_quantity.str() != quantity || config.max_gross_quantity.str() != gross)
      throw AstStatus(AST_INVALID);
    *out = new OrderLimits(config);
  });
}
AstStatus start(void* self) noexcept {
  return sdk::boundary([&] {
    if (!self)
      throw AstStatus(AST_INVALID);
    static_cast<OrderLimits*>(self)->start();
  });
}
void stop(void* self) noexcept {
  if (self)
    static_cast<OrderLimits*>(self)->stop();
}
void destroy(void* self) noexcept {
  delete static_cast<OrderLimits*>(self);
}
AstStatus evaluate(void* self, const AstRiskContext* input, uint32_t* decision) noexcept {
  if (decision)
    *decision = AST_RISK_UNAVAILABLE;
  return sdk::boundary([&] {
    if (!self || !input || !decision || input->side > AST_RISK_SELL ||
        input->working_orders > std::numeric_limits<std::size_t>::max())
      throw AstStatus(AST_INVALID);
    const Instrument instrument{{text(input->venue), text(input->symbol)},
                                text(input->quote_currency),
                                Decimal::from_raw(input->price_increment),
                                Decimal::from_raw(input->quantity_increment),
                                Decimal::from_raw(input->multiplier)};
    const LimitOrder order{text(input->order_id),
                           {text(input->order_venue), text(input->order_symbol)},
                           input->side == AST_RISK_BUY ? Side::buy : Side::sell,
                           Decimal::from_raw(input->quantity),
                           Decimal::from_raw(input->limit_price)};
    Offset offset;
    switch (input->offset) {
    case AST_RISK_OPEN:
      offset = Offset::open;
      break;
    case AST_RISK_CLOSE_TODAY:
      offset = Offset::close_today;
      break;
    case AST_RISK_CLOSE_YESTERDAY:
      offset = Offset::close_yesterday;
      break;
    case AST_RISK_CLOSE:
      offset = Offset::close;
      break;
    default:
      *decision = AST_RISK_INVALID_CONTEXT;
      return;
    }
    const auto result = static_cast<OrderLimits*>(self)->evaluate(
        {instrument, order, offset, Decimal::from_raw(input->gross_position_quantity),
         Decimal::from_raw(input->pending_open_quantity),
         static_cast<std::size_t>(input->working_orders)});
    switch (result.reason) {
    case RiskReason::allowed:
      *decision = AST_RISK_ALLOW;
      break;
    case RiskReason::unavailable:
      *decision = AST_RISK_UNAVAILABLE;
      break;
    case RiskReason::invalid_context:
      *decision = AST_RISK_INVALID_CONTEXT;
      break;
    case RiskReason::order_quantity:
      *decision = AST_RISK_ORDER_QUANTITY;
      break;
    case RiskReason::gross_quantity:
      *decision = AST_RISK_GROSS_QUANTITY;
      break;
    case RiskReason::working_orders:
      *decision = AST_RISK_WORKING_ORDERS;
      break;
    }
  });
}
const AstRiskV1 risk{sizeof(AstRiskV1), 1, evaluate};
AstStatus query(void* self, const char* capability, uint32_t version, uint32_t size,
                const void** out) noexcept {
  if (!out)
    return AST_INVALID;
  *out = nullptr;
  if (!self || !capability)
    return AST_INVALID;
  if (std::string_view(capability) != AST_RISK_V1 || version != 1 || size != sizeof(AstRiskV1))
    return AST_UNSUPPORTED;
  *out = &risk;
  return AST_OK;
}
const AstCapability capabilities[] = {{AST_RISK_V1, 1, "risk"}};
const AstPluginV1 descriptor{sizeof(AstPluginV1),
                             ASTERION_PLUGIN_ABI_VERSION,
                             "asterion.risk.order-limits",
                             "1.0.0",
                             ASTERION_PLUGIN_PLATFORM,
                             capabilities,
                             1,
                             create,
                             start,
                             stop,
                             destroy,
                             query};
} // namespace
extern "C" ASTERION_PLUGIN_EXPORT const AstPluginV1*
asterion_plugin_entry_v1(uint32_t abi, uint32_t size) noexcept {
  return abi == ASTERION_PLUGIN_ABI_VERSION && size == sizeof(AstPluginV1) ? &descriptor : nullptr;
}
