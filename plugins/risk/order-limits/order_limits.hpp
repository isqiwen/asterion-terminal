#pragma once
#include <asterion/domain/risk_port.hpp>
namespace asterion {
struct OrderLimitsConfig {
  Decimal max_order_quantity;
  Decimal max_gross_quantity;
  std::size_t max_working_orders;
  void validate() const;
};
// Explicit immutable session configuration; unknown/missing fields reject.
Json encode_order_limits(const OrderLimitsConfig& config);
OrderLimitsConfig decode_order_limits(const Json& value);
// Single-instrument quantity limits, with pending opening orders reserved at
// their full remaining size. No credit for cancellations or closes not filled.
class OrderLimits final : public RiskPort {
public:
  explicit OrderLimits(OrderLimitsConfig config);
  PluginDescriptor descriptor() const override;
  void start() override { running_ = true; }
  void stop() noexcept override { running_ = false; }
  RiskDecision evaluate(const PreTradeRiskContext& context) const override;

private:
  OrderLimitsConfig config_;
  bool running_ = false;
};
} // namespace asterion
