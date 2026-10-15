#include "portfolio_budget.hpp"
#include <gtest/gtest.h>
using namespace asterion;
namespace {
Decimal d(const char* s) {
  return Decimal::parse(s);
}
void put(protocol::v1::Decimal* p, const char* s) {
  p->set_units(d(s).raw());
}
struct Evidence {
  backtest::v1::BacktestInput input;
  backtest::v1::BacktestResult result;
  Evidence() {
    auto* risk = input.mutable_paper()->mutable_risk();
    put(risk->mutable_max_order_quantity(), "3");
    put(risk->mutable_max_gross_quantity(), "5");
    risk->set_max_working_orders(4);
    result.add_settlements()->set_trading_day("2024-01-02");
  }
  void contract(const char* symbol, const char* multiplier, const char* mark, bool buy,
                const char* quantity, const char* per_lot, const char* rate) {
    auto* fixed = input.mutable_paper()->add_contracts();
    auto* terms = fixed->mutable_dataset()->mutable_contract();
    terms->set_venue("SHFE");
    terms->set_symbol(symbol);
    put(terms->mutable_multiplier(), multiplier);
    put(terms->mutable_quantity_increment(), "1");
    auto* version = fixed->mutable_cost_schedule()->add_versions();
    version->set_effective_from("2024-01-01");
    version->set_source("explicit synthetic assumption");
    auto* c = version->mutable_values();
    put(c->mutable_margin_per_lot(), per_lot);
    put(c->mutable_margin_rate(), rate);
    c->mutable_open_fee();
    c->mutable_close_today_fee();
    c->mutable_close_yesterday_fee();
    c->mutable_open_fee_rate();
    c->mutable_close_today_fee_rate();
    c->mutable_close_yesterday_fee_rate();
    auto* saved = result.mutable_account()->add_contracts();
    *saved->mutable_contract() = *terms;
    put(saved->mutable_mark(), mark);
    if (d(quantity) != Decimal{}) {
      auto* lot = result.mutable_account()->add_positions();
      lot->set_venue("SHFE");
      lot->set_symbol(symbol);
      lot->set_side(buy ? protocol::v1::BUY : protocol::v1::SELL);
      put(lot->mutable_quantity(), quantity);
    }
  }
};
} // namespace
TEST(PortfolioBudget, RoundedLongShortLotsKeepGrossNetMarginAndCashDistinctWithoutMutation) {
  Evidence e;
  e.contract("rb", "10", "101", true, "3", "100", "0.1");
  e.contract("cu", "5", "210", false, "2", "50", "0.2");
  e.contract("al", "5", "0", true, "0", "50", "0");
  auto* a = e.result.mutable_account();
  put(a->mutable_balance(), "10000");
  put(a->mutable_equity(), "10000");
  put(a->mutable_available(), "8877");
  put(a->mutable_margin(), "1123");
  const auto input = e.input.SerializeAsString(), result = e.result.SerializeAsString();
  const auto p = portfolio_exposure(e.input, e.result);
  ASSERT_EQ(p.contracts.size(), 3U);
  EXPECT_EQ(p.contracts[0].margin, d("603"));
  EXPECT_EQ(p.contracts[1].margin, d("520"));
  EXPECT_EQ(p.net_notional, d("930"));
  EXPECT_EQ(p.gross_notional, d("5130"));
  EXPECT_EQ(p.margin, d("1123"));
  EXPECT_EQ(p.gross_quantity, d("5"));
  EXPECT_EQ(p.balance, d("10000"));
  EXPECT_EQ(p.available, d("8877"));
  EXPECT_EQ(p.max_gross_quantity - p.gross_quantity, d("0"));
  EXPECT_TRUE(p.contracts[0].quantities_on_grid);
  EXPECT_EQ(p.contracts[2].gross_notional, Decimal{});
  EXPECT_EQ(e.input.SerializeAsString(), input);
  EXPECT_EQ(e.result.SerializeAsString(), result);
}
TEST(PortfolioBudget, ExplicitLimitsCanBeExceededExactlyMetZeroOrUnsetWithoutChangingExecution) {
  const auto over = review_budget(d("5130"), d("5000"));
  ASSERT_TRUE(over.headroom);
  EXPECT_EQ(*over.headroom, d("-130"));
  EXPECT_EQ(*review_budget(d("5130"), d("5130")).headroom, Decimal{});
  EXPECT_EQ(*review_budget(d("1123"), d("1200")).headroom, d("77"));
  EXPECT_EQ(*review_budget(d("5130"), d("0")).headroom, d("-5130"));
  EXPECT_FALSE(review_budget(d("5130"), std::nullopt).headroom);
  EXPECT_THROW(review_budget(d("1"), d("-1")), std::invalid_argument);
}
TEST(PortfolioBudget, QuantitiesOffTheFixedGridRemainVisibleRatherThanRoundedAgain) {
  Evidence e;
  e.contract("rb", "10", "100", true, "1.5", "100", "0");
  const auto p = portfolio_exposure(e.input, e.result);
  EXPECT_FALSE(p.contracts[0].quantities_on_grid);
  EXPECT_EQ(p.contracts[0].net_quantity, d("1.5"));
}
