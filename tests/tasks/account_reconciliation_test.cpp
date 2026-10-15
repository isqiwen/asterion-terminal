#include "account_reconciliation.hpp"
#include <gtest/gtest.h>
#include <algorithm>
using namespace asterion;
namespace {
Decimal d(const char* value) {
  return Decimal::parse(value);
}
void put(protocol::v1::Decimal* field, const char* value) {
  field->set_units(d(value).raw());
}
struct Evidence {
  backtest::v1::BacktestInput input;
  backtest::v1::BacktestResult result;
  Evidence() {
    put(input.mutable_paper()->mutable_deposit(), "1000");
    auto* fixed = input.mutable_paper()->add_contracts();
    auto* contract = fixed->mutable_dataset()->mutable_contract();
    contract->set_venue("SHFE");
    contract->set_symbol("rb2401");
    put(contract->mutable_multiplier(), "10");
    auto* version = fixed->mutable_cost_schedule()->add_versions();
    version->set_effective_from("2024-01-01");
    version->set_source("explicit test assumption");
    auto* values = version->mutable_values();
    values->mutable_open_fee();
    values->mutable_close_today_fee();
    values->mutable_close_yesterday_fee();
    values->mutable_open_fee_rate();
    values->mutable_close_today_fee_rate();
    values->mutable_close_yesterday_fee_rate();
    put(values->mutable_margin_per_lot(), "50");
    put(version->mutable_values()->mutable_margin_rate(), "0.1");
  }
  void fill(const char* id, const char* day, bool buy, const char* qty, const char* price,
            const char* fee) {
    auto* order = result.mutable_account()->add_orders();
    order->set_id(id);
    order->set_side(buy ? protocol::v1::BUY : protocol::v1::SELL);
    auto* fill = result.mutable_account()->add_fills();
    fill->set_id(id);
    fill->set_order_id(id);
    put(fill->mutable_quantity(), qty);
    put(fill->mutable_price(), price);
    put(fill->mutable_fee(), fee);
    auto* label = result.add_fill_days();
    label->set_fill_id(id);
    label->set_trading_day(day);
  }
  void day(const char* date, const char* price, const char* qty, const char* balance,
           const char* realized, const char* fees) {
    auto* fixed = input.mutable_paper()->mutable_contracts(0)->mutable_dataset()->add_days();
    fixed->set_trading_day(date);
    put(fixed->mutable_settlement_price(), price);
    auto* day = result.add_settlements();
    day->set_trading_day(date);
    put(day->mutable_balance(), balance);
    put(day->mutable_equity(), balance);
    put(day->mutable_realized(), realized);
    put(day->mutable_fees(), fees);
    auto* row = day->add_contracts();
    put(row->mutable_price(), price);
    put(row->mutable_position_quantity(), qty);
  }
  void final(const char* balance, const char* qty, const char* realized, const char* fees,
             const char* margin, const char* available) {
    auto* a = result.mutable_account();
    put(a->mutable_balance(), balance);
    put(a->mutable_equity(), balance);
    put(a->mutable_realized(), realized);
    put(a->mutable_fees(), fees);
    put(a->mutable_margin(), margin);
    put(a->mutable_available(), available);
    auto* lot = a->add_positions();
    lot->set_side(protocol::v1::BUY);
    put(lot->mutable_quantity(), qty);
  }
};
const AccountCheck& check(const std::vector<AccountCheck>& checks, const char* metric) {
  return *std::ranges::find_if(checks, [&](const auto& c) { return c.metric == metric; });
}
} // namespace
TEST(AccountReconciliation, FillsAndDailySettlementExplainLotsGrossFeesAndFundsWithoutMutation) {
  Evidence e;
  e.fill("open", "2024-01-02", true, "2", "100", "2");
  e.fill("partial-close", "2024-01-03", false, "1", "108", "3");
  e.day("2024-01-02", "105", "2", "1098", "100", "2");
  e.day("2024-01-03", "110", "1", "1175", "180", "5");
  e.day("2024-01-04", "107", "1", "1145", "150", "5");
  e.final("1145", "1", "150", "5", "157", "988");
  const auto input = e.input.SerializeAsString(), result = e.result.SerializeAsString();
  const auto a = reconcile_backtest(e.input, e.result);
  ASSERT_EQ(a.state, AccountReconciliation::State::available);
  ASSERT_EQ(a.days.size(), 3U);
  EXPECT_EQ(a.days[0].gross, d("100"));
  EXPECT_EQ(a.days[1].gross, d("80"));
  EXPECT_EQ(a.days[2].gross, d("-30"));
  EXPECT_TRUE(a.days[2].fills.empty());
  EXPECT_EQ(a.days[1].quantity_change, d("-1"));
  EXPECT_EQ(a.days[1].fills[0].order_id, "partial-close");
  EXPECT_EQ(a.signed_notional, d("1070"));
  EXPECT_EQ(a.gross_notional, d("1070"));
  EXPECT_EQ(a.cost_source, "explicit test assumption");
  for (const auto& day : a.days)
    for (const auto& c : day.checks)
      EXPECT_EQ(c.expected, c.recorded) << c.metric;
  for (const auto& c : a.final_checks)
    EXPECT_EQ(c.expected, c.recorded) << c.metric;
  EXPECT_EQ(e.input.SerializeAsString(), input);
  EXPECT_EQ(e.result.SerializeAsString(), result);
}
TEST(AccountReconciliation, DifferencesStayVisibleAndAreNotRepaired) {
  Evidence e;
  e.fill("open", "2024-01-02", true, "1", "100", "1");
  e.day("2024-01-02", "105", "2", "1049", "50", "1");
  e.final("1048", "1", "50", "1", "999", "894");
  const auto bytes = e.result.SerializeAsString();
  const auto a = reconcile_backtest(e.input, e.result);
  EXPECT_EQ(check(a.days[0].checks, "position").recorded -
                check(a.days[0].checks, "position").expected,
            d("1"));
  EXPECT_EQ(check(a.final_checks, "balance").recorded - check(a.final_checks, "balance").expected,
            d("-1"));
  EXPECT_EQ(check(a.final_checks, "margin").expected, d("155"));
  EXPECT_EQ(e.result.SerializeAsString(), bytes);
}
TEST(AccountReconciliation, MissingDatesAndOtherInputsRemainUnknownButNoFillsNeedNoDates) {
  Evidence e;
  e.day("2024-01-02", "100", "0", "1000", "0", "0");
  auto a = reconcile_backtest(e.input, e.result);
  EXPECT_EQ(a.state, AccountReconciliation::State::available);
  e.fill("open", "2024-01-02", true, "1", "100", "1");
  e.result.clear_fill_days();
  a = reconcile_backtest(e.input, e.result);
  EXPECT_EQ(a.state, AccountReconciliation::State::missing_fill_days);
  EXPECT_TRUE(a.days.empty());
  e.input.mutable_paper()->mutable_contracts(0)->mutable_dataset()->set_interval_minutes(1);
  EXPECT_EQ(reconcile_backtest(e.input, e.result).state,
            AccountReconciliation::State::unsupported_input);
}
TEST(AccountReconciliation, MarginSumsSavedLotsWithOriginalRoundingAndLastEffectiveCost) {
  Evidence e;
  auto* fixed = e.input.mutable_paper()->mutable_contracts(0);
  put(fixed->mutable_dataset()->mutable_contract()->mutable_multiplier(), "1");
  auto* cost = fixed->mutable_cost_schedule()->add_versions();
  cost->set_effective_from("2024-01-02");
  cost->set_source("second fixed assumption");
  *cost->mutable_values() = fixed->cost_schedule().versions(0).values();
  put(cost->mutable_values()->mutable_margin_per_lot(), "0");
  put(cost->mutable_values()->mutable_margin_rate(), "0.5");
  e.fill("one", "2024-01-02", true, "0.01", "100", "0");
  e.fill("two", "2024-01-02", true, "0.01", "100", "0");
  e.day("2024-01-02", "101", "0.02", "1000.02", "0.02", "0");
  e.final("1000.02", "0.01", "0.02", "0", "1.02", "999");
  auto* lot = e.result.mutable_account()->add_positions();
  lot->set_side(protocol::v1::BUY);
  put(lot->mutable_quantity(), "0.01");
  const auto a = reconcile_backtest(e.input, e.result);
  EXPECT_EQ(a.cost_source, "second fixed assumption");
  EXPECT_EQ(check(a.final_checks, "margin").expected, d("1.02"));
  for (const auto& c : a.final_checks)
    EXPECT_EQ(c.expected, c.recorded) << c.metric;
}
