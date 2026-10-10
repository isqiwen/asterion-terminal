#pragma once
#include <asterion/domain/market.hpp>
#include <asterion/domain/position_target.hpp>
#include <asterion/v1/trading.pb.h>
#include <memory>
#include <optional>
namespace asterion {
// A trading rule applied to one contract's completed bars. It has no clock,
// data source or execution: its host feeds the bars and routes the targets.
class Strategy {
public:
  virtual ~Strategy() = default;
  void start();
  void stop() noexcept;
  // Called once per completed bar, in order. The returned target is the
  // position wanted in lots, positive long and negative short: the quantity
  // on the side the rule takes, where the strategy may hold that side, and
  // zero otherwise. Absent until the rule has seen enough bars. It is an
  // intent, never an authorized order.
  std::optional<Decimal> on_bar(const MarketBar& bar);

protected:
  Strategy(Instrument instrument, Decimal quantity, PositionSides sides);

private:
  // Forget every bar seen.
  virtual void reset() = 0;
  // The side the rule takes after this bar: 1 long, -1 short, 0 none.
  virtual std::optional<int> decide(const MarketBar& bar) = 0;
  Instrument instrument_;
  Decimal quantity_;
  PositionSides sides_;
  std::int64_t last_time_ = -1;
  bool running_ = false;
};
// The definition is valid for the contract or this throws.
std::unique_ptr<Strategy> make_strategy(const protocol::v1::Strategy& definition,
                                        Instrument instrument);
} // namespace asterion
