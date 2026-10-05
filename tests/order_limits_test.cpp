#include "order_limits.hpp"
#include <gtest/gtest.h>
#include "native_risk.hpp"
#include "risk_module.hpp"
#include <asterion/kernel/process/child.hpp>
using namespace asterion;
namespace {
Decimal d(const char* text) {
  return Decimal::parse(text);
}
Instrument instrument() {
  return {{"SHFE", "rb2610"}, "CNY", d("1"), d("1"), d("10")};
}
} // namespace
TEST(OrderLimits, lifecycle_and_exact_bounds) {
  auto spec = instrument();
  LimitOrder order{"test", spec.id, Side::buy, d("2"), d("100")};
  PreTradeRiskContext context{spec, order, Offset::open, d("4"), d("4"), 1};
  OrderLimits risk({d("2"), d("10"), 2});
  EXPECT_EQ(risk.evaluate(context).reason, RiskReason::unavailable);
  risk.start();
  EXPECT_TRUE(risk.evaluate(context).allowed()); // Exactly 10 total and 2 working.
  context.pending_open_quantity = d("5");
  EXPECT_EQ(risk.evaluate(context).reason, RiskReason::gross_quantity);
  context.pending_open_quantity = d("4");
  context.working_orders = 2;
  EXPECT_EQ(risk.evaluate(context).reason, RiskReason::working_orders);
  context.working_orders = 1;
  order.quantity = d("3");
  EXPECT_EQ(risk.evaluate(context).reason, RiskReason::order_quantity);
  risk.stop();
  EXPECT_EQ(risk.evaluate(context).reason, RiskReason::unavailable);
}
TEST(OrderLimits, gross_risk_does_not_net_sides_or_credit_pending_closes) {
  auto spec = instrument();
  LimitOrder sell{"test", spec.id, Side::sell, d("1"), d("100")};
  PreTradeRiskContext context{spec, sell, Offset::open, d("10"), d("0"), 0};
  OrderLimits risk({d("3"), d("10"), 5});
  risk.start();
  EXPECT_EQ(risk.evaluate(context).reason, RiskReason::gross_quantity);
  context.offset = Offset::close_today;
  EXPECT_TRUE(risk.evaluate(context).allowed());
  context.gross_position_quantity = d("20");
  EXPECT_TRUE(risk.evaluate(context).allowed()); // Permit reducing over-limit risk.
  context.offset = Offset::open;
  EXPECT_EQ(risk.evaluate(context).reason, RiskReason::gross_quantity);
}
TEST(OrderLimits, rejects_bad_configuration_and_context) {
  EXPECT_THROW((OrderLimits({d("0"), d("10"), 1})), std::invalid_argument);
  EXPECT_THROW((OrderLimits({d("1"), d("-1"), 1})), std::invalid_argument);
  EXPECT_THROW((OrderLimits({d("1"), d("10"), 0})), std::invalid_argument);
  auto spec = instrument();
  LimitOrder order{"test", spec.id, Side::buy, d("1"), d("100")};
  PreTradeRiskContext context{spec, order, Offset::open, d("-1"), d("0"), 0};
  OrderLimits risk({d("1"), d("10"), 1});
  risk.start();
  EXPECT_EQ(risk.evaluate(context).reason, RiskReason::invalid_context);
  context.gross_position_quantity = d("0");
  context.pending_open_quantity = d("0.5");
  EXPECT_EQ(risk.evaluate(context).reason, RiskReason::invalid_context);
  context.pending_open_quantity = d("0");
  context.offset = static_cast<Offset>(50);
  EXPECT_EQ(risk.evaluate(context).reason, RiskReason::invalid_context);
  context.offset = Offset::open;
  order.quantity = d("0");
  EXPECT_THROW(risk.evaluate(context), std::invalid_argument);
}
TEST(OrderLimits, exposure_check_does_not_overflow_before_rejecting) {
  auto spec = instrument();
  LimitOrder order{"test", spec.id, Side::buy, d("1"), d("100")};
  const auto huge = d("92233720368");
  PreTradeRiskContext context{spec, order, Offset::open, huge, huge, 0};
  OrderLimits risk({d("1"), huge, 1});
  risk.start();
  EXPECT_EQ(risk.evaluate(context).reason, RiskReason::gross_quantity);
}

TEST(OrderLimits, derives_exposure_from_real_ledger_through_partial_fill_and_cancel) {
  auto spec = instrument();
  FuturesAccount account(d("10000"), {{spec, {d("100"), d("0"), d("0"), d("0")}}});
  account.mark(spec.id, d("100"));
  OrderLimits risk({d("5"), d("5"), 3});
  risk.start();
  LimitOrder first{"first", spec.id, Side::buy, d("4"), d("100")};
  ASSERT_TRUE(assess_order(risk, account, first, Offset::open).allowed());
  account.submit(first, Offset::open);
  LimitOrder next{"next", spec.id, Side::sell, d("2"), d("100")};
  const auto before = account.snapshot();
  EXPECT_EQ(assess_order(risk, account, next, Offset::open).reason, RiskReason::gross_quantity);
  EXPECT_EQ(account.snapshot(), before);
  account.fill({"fill.1", "first", d("2"), d("100")});
  // A partial fill moves pending exposure to positions, without freeing quota.
  EXPECT_EQ(assess_order(risk, account, next, Offset::open).reason, RiskReason::gross_quantity);
  account.cancel("first");
  EXPECT_TRUE(assess_order(risk, account, next, Offset::open).allowed());
  account.submit(next, Offset::open);
  account.fill({"fill.2", "next", d("2"), d("100")});
  // Long 2 + short 2 is gross 4, never net zero.
  LimitOrder third{"third", spec.id, Side::buy, d("2"), d("100")};
  EXPECT_EQ(assess_order(risk, account, third, Offset::open).reason, RiskReason::gross_quantity);
  LimitOrder close{"close", spec.id, Side::sell, d("2"), d("100")};
  account.submit(close, Offset::close_today);
  EXPECT_EQ(assess_order(risk, account, third, Offset::open).reason, RiskReason::gross_quantity);
  account.fill({"fill.3", "close", d("2"), d("100")});
  EXPECT_TRUE(assess_order(risk, account, third, Offset::open).allowed());
}

TEST(OrderLimits, configuration_roundtrips_without_defaults_or_rewriting) {
  const OrderLimitsConfig original{d("3"), d("10"), 7};
  const auto encoded = encode_order_limits(original);
  const auto decoded = decode_order_limits(encoded);
  EXPECT_EQ(decoded.max_order_quantity, original.max_order_quantity);
  EXPECT_EQ(decoded.max_gross_quantity, original.max_gross_quantity);
  EXPECT_EQ(decoded.max_working_orders, original.max_working_orders);
  EXPECT_EQ(encode_order_limits(decoded), encoded);
  for (const auto* key : {"max_order_quantity", "max_gross_quantity", "max_working_orders"}) {
    auto missing = encoded;
    missing.erase(key);
    EXPECT_THROW(decode_order_limits(missing), std::exception);
  }
  auto invalid = encoded;
  invalid["extra"] = true;
  EXPECT_THROW(decode_order_limits(invalid), std::exception);
  invalid = encoded;
  invalid["max_working_orders"] = -1;
  EXPECT_THROW(decode_order_limits(invalid), std::exception);
  invalid = encoded;
  invalid["max_working_orders"] = 1.5;
  EXPECT_THROW(decode_order_limits(invalid), std::exception);
  invalid = encoded;
  invalid["max_order_quantity"] = "3.0";
  EXPECT_THROW(decode_order_limits(invalid), std::exception);
  invalid = encoded;
  invalid["max_gross_quantity"] = "0";
  EXPECT_THROW(decode_order_limits(invalid), std::exception);
}

TEST(NativeRisk, MatchesQuantityPolicyAcrossSidesOffsetsReservationsAndLifecycle) {
  const std::vector<std::pair<std::string, std::string>> settings{
      {"max_order_quantity", "3"}, {"max_gross_quantity", "10"}, {"max_working_orders", "4"}};
  NativeRisk native(NativeLibrary(RISK_PLUGIN), settings);
  OrderLimits reference({d("3"), d("10"), 4});
  EXPECT_EQ(native.artifact().size(), 64);
  auto spec = instrument();
  LimitOrder order{"native-risk", spec.id, Side::buy, d("1"), d("100")};
  PreTradeRiskContext context{spec, order, Offset::open, d("0"), d("0"), 0};
  EXPECT_EQ(native.evaluate(context).reason, RiskReason::unavailable);
  native.start();
  reference.start();
  for (const auto side : {Side::buy, Side::sell})
    for (const auto offset :
         {Offset::open, Offset::close_today, Offset::close_yesterday, Offset::close})
      for (const auto quantity : {"1", "3", "4"})
        for (const auto gross : {"0", "6", "10", "92233720368"})
          for (const auto pending : {"0", "4", "92233720368"})
            for (const auto working : {0U, 3U, 4U}) {
              order.side = side;
              order.quantity = d(quantity);
              context.offset = offset;
              context.gross_position_quantity = d(gross);
              context.pending_open_quantity = d(pending);
              context.working_orders = working;
              EXPECT_EQ(native.evaluate(context).reason, reference.evaluate(context).reason);
            }
  context.offset = Offset::open;
  context.gross_position_quantity = d("-1");
  EXPECT_EQ(native.evaluate(context).reason, RiskReason::invalid_context);
  context.gross_position_quantity = d("0");
  context.pending_open_quantity = d("0.5");
  EXPECT_EQ(native.evaluate(context).reason, RiskReason::invalid_context);
  order.quantity = d("0");
  EXPECT_THROW(native.evaluate(context), std::invalid_argument);
  native.stop();
  EXPECT_EQ(native.evaluate(context).reason, RiskReason::unavailable);
}
TEST(NativeRisk, MissingOrInvalidConfigurationNeverCreatesAnEnabledPolicy) {
  EXPECT_THROW((NativeRisk(NativeLibrary(RISK_PLUGIN), {})), std::exception);
  EXPECT_THROW((NativeRisk(NativeLibrary(RISK_PLUGIN), {{"max_order_quantity", "1"},
                                                        {"max_gross_quantity", "10"},
                                                        {"max_working_orders", "0"}})),
               std::exception);
  EXPECT_THROW((NativeRisk(NativeLibrary(RISK_PLUGIN), {{"max_order_quantity", "1.0"},
                                                        {"max_gross_quantity", "10"},
                                                        {"max_working_orders", "1"}})),
               std::exception);
  EXPECT_THROW((NativeRisk(NativeLibrary(RISK_PLUGIN), {{"max_order_quantity", "1"},
                                                        {"max_order_quantity", "10"},
                                                        {"max_working_orders", "1"}})),
               std::exception);
}

TEST(NativeRisk, MissingDecisionUnknownDecisionAndPluginFailureNeverAllowOrders) {
  const auto spec = instrument();
  const LimitOrder order{"failure-check", spec.id, Side::buy, d("1"), d("100")};
  const PreTradeRiskContext context{spec, order, Offset::open, d("0"), d("0"), 0};
  for (const auto mode : {"0", "1", "2", "4"}) {
    NativeRisk policy(NativeLibrary(RISK_FIXTURE), {{"mode", mode}});
    policy.start();
    EXPECT_THROW(policy.evaluate(context), std::exception) << mode;
  }
  NativeRisk policy(NativeLibrary(RISK_FIXTURE), {{"mode", "3"}});
  policy.start();
  EXPECT_EQ(policy.evaluate(context).reason, RiskReason::order_quantity);
}

TEST(NativeRisk, MultipleAlgorithmsRequireAnExplicitSelectionInsteadOfChoosingOne) {
  struct Plugins {
    std::filesystem::path original = native_plugin_directory();
    std::filesystem::path directory =
        std::filesystem::temp_directory_path() / ("asterion-risk-choice-" + unique_process_id());
    ~Plugins() {
      configure_native_plugins(original);
      std::error_code ignored;
      std::filesystem::remove_all(directory, ignored);
    }
  } scope;
  std::filesystem::create_directory(scope.directory);
  configure_native_plugins(scope.directory);
  EXPECT_THROW(risk_providers::Module::selected(), std::exception);
  const auto suffix = std::filesystem::path(RISK_PLUGIN).extension().string();
  std::filesystem::copy_file(RISK_PLUGIN, scope.directory / ("one" + suffix));
  EXPECT_NO_THROW(risk_providers::Module::selected());
  std::filesystem::copy_file(RISK_FIXTURE, scope.directory / ("two" + suffix));
  EXPECT_THROW(risk_providers::Module::selected(), std::invalid_argument);
}
