#pragma once
#include <asterion/domain/market.hpp>
#include <asterion/domain/position_target.hpp>
#include <asterion/v1/trading.pb.h>
#include <deque>
#include <memory>
#include <optional>
#include <vector>
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
// The definition is valid for the contract or this throws. A rule over
// several contracts is not a strategy for one.
std::unique_ptr<Strategy> make_strategy(const protocol::v1::Strategy& definition,
                                        Instrument instrument);
// A rule over several units at once, each a contract or a product's dominant
// series: it ranks them on the bars they all have and says which to hold long
// and which short. How many lots that is for each is its host's to work out.
// Like a Strategy it has no clock, data source or execution.
class CrossSection {
public:
  // Whether the definition's rule ranks several units.
  static bool defines(const protocol::v1::Strategy& definition);
  // The definition ranks `units` units or this throws.
  CrossSection(const protocol::v1::Strategy& definition, std::size_t units);
  // One completed bar of one unit, with what the rule's factor reads of it:
  // its close for momentum, the carry its day began with for the term
  // structure. Timestamps never go back; a unit has one bar at a timestamp.
  void on_bar(std::size_t unit, std::int64_t timestamp_ns, Decimal value);
  // Called when no more bars of the last timestamp will come. Where every
  // unit had a bar and the rule rebalances, the side wanted of each unit from
  // now on: 1 long, -1 short, 0 none. Absent otherwise.
  std::optional<std::vector<int>> rank();

private:
  // Momentum compares a value with the one a lookback earlier; the term
  // structure averages the lookback's values. `window_` is how many of the
  // bars every unit had are kept for that.
  bool momentum_;
  std::size_t window_, rebalance_, count_;
  PositionSides sides_;
  std::int64_t time_ = -1;
  // The bars of the current timestamp, and the values of the last bars every
  // unit had.
  std::vector<std::optional<Decimal>> latest_;
  std::vector<std::deque<Decimal>> values_;
  std::size_t shared_ = 0;
};
} // namespace asterion
