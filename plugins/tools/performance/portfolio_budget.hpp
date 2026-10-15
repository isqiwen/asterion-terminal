#pragma once
#include <asterion/protocol/backtest.hpp>
namespace asterion {
struct ContractExposure {
  std::string venue, symbol, cost_source, cost_effective_from;
  Decimal mark, multiplier, quantity_increment, net_quantity, gross_quantity;
  Decimal net_notional, gross_notional, margin;
  FuturesCosts costs;
  bool quantities_on_grid = true;
};
struct PortfolioExposure {
  std::vector<ContractExposure> contracts;
  Decimal net_notional, gross_notional, gross_quantity, margin;
  Decimal balance, equity, available, frozen, recorded_margin;
  Decimal max_order_quantity, max_gross_quantity;
  std::uint64_t max_working_orders;
};
// Fixed final account evidence, not desired weights or peak intraday usage.
PortfolioExposure portfolio_exposure(const backtest::v1::BacktestInput&,
                                     const backtest::v1::BacktestResult&);
struct SettlementExposure {
  std::string day;
  Decimal net_notional, balance, equity;
  std::optional<Decimal> gross_notional, margin, available, frozen;
};
// Net exposure comes from fixed settlement prices and quantities. Gross and
// margin require the optional saved occupancy; never reconstruct missing lots.
std::vector<SettlementExposure> settlement_exposure(const backtest::v1::BacktestInput&,
                                                    const backtest::v1::BacktestResult&);
struct BudgetCheck {
  Decimal used;
  std::optional<Decimal> limit, headroom;
};
// A caller's explicit review threshold; never an execution authorization.
BudgetCheck review_budget(Decimal used, std::optional<Decimal> limit);
} // namespace asterion
