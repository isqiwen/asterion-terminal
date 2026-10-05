#include "native_risk.hpp"
#include <asterion/foundation/error.hpp>
#include <limits>
namespace asterion {
NativeRisk::NativeRisk(NativeLibrary library,
                       const std::vector<std::pair<std::string, std::string>>& settings)
    : library_(std::move(library)) {
  std::vector<AstSetting> values;
  for (const auto& [key, value] : settings)
    values.push_back({key.c_str(), value.c_str()});
  instance_ = library_.create(AST_RISK_V1, values);
  table_ = static_cast<const AstRiskV1*>(instance_->query(AST_RISK_V1, 1, sizeof(AstRiskV1)));
  if (table_->size != sizeof(AstRiskV1) || table_->version != 1 || !table_->evaluate)
    throw std::invalid_argument("invalid native risk capability");
}
void NativeRisk::start() {
  instance_->start();
  running_ = true;
}
void NativeRisk::stop() noexcept {
  running_ = false;
  instance_->stop();
}
RiskDecision NativeRisk::evaluate(const PreTradeRiskContext& context) const {
  if (!running_)
    return {RiskReason::unavailable};
  context.order.validate(context.instrument);
  const auto& i = context.instrument;
  const auto& o = context.order;
  uint32_t offset = std::numeric_limits<uint32_t>::max();
  switch (context.offset) {
  case Offset::open:
    offset = AST_RISK_OPEN;
    break;
  case Offset::close_today:
    offset = AST_RISK_CLOSE_TODAY;
    break;
  case Offset::close_yesterday:
    offset = AST_RISK_CLOSE_YESTERDAY;
    break;
  case Offset::close:
    offset = AST_RISK_CLOSE;
    break;
  }
  const AstRiskContext input{i.id.venue.c_str(),
                             i.id.symbol.c_str(),
                             i.quote_currency.c_str(),
                             i.price_increment.raw(),
                             i.quantity_increment.raw(),
                             i.multiplier.raw(),
                             o.id.c_str(),
                             o.instrument.venue.c_str(),
                             o.instrument.symbol.c_str(),
                             o.side == Side::buy ? AST_RISK_BUY : AST_RISK_SELL,
                             offset,
                             o.quantity.raw(),
                             o.limit_price.raw(),
                             context.gross_position_quantity.raw(),
                             context.pending_open_quantity.raw(),
                             context.working_orders};
  uint32_t decision = std::numeric_limits<uint32_t>::max();
  check_plugin_status(table_->evaluate(instance_->handle(), &input, &decision));
  switch (decision) {
  case AST_RISK_ALLOW:
    return {RiskReason::allowed};
  case AST_RISK_UNAVAILABLE:
    return {RiskReason::unavailable};
  case AST_RISK_INVALID_CONTEXT:
    return {RiskReason::invalid_context};
  case AST_RISK_ORDER_QUANTITY:
    return {RiskReason::order_quantity};
  case AST_RISK_GROSS_QUANTITY:
    return {RiskReason::gross_quantity};
  case AST_RISK_WORKING_ORDERS:
    return {RiskReason::working_orders};
  default:
    throw std::invalid_argument("invalid native risk decision");
  }
}
} // namespace asterion
