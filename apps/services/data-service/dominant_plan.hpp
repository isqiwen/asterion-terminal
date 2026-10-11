#pragma once
#include <asterion/foundation/decimal.hpp>
#include <asterion/v1/data.pb.h>
#include <map>
#include <span>
#include <string>
#include <vector>
namespace asterion::data {
// One month contract of a product, as the dominant rule reads it: what it
// settled at and how much of it was held, by trading day.
struct DominantMonth {
  std::string name; // how a failure names it
  std::string delivery_month;
  std::map<std::string, Decimal> settlement, open_interest;
};
// From `begin` on, `month` is the dominant one. `before` is the trading day
// before `begin`; `factor` multiplies the month's prices to the level of the
// last month (ratio back-adjustment: 1 for the last segment).
struct DominantSegment {
  std::size_t month = 0;
  std::string begin, before;
  Decimal factor;
};
struct DominantPlan {
  std::vector<std::string> days; // every day one of the months settled, ascending
  std::vector<DominantSegment> segments;
  std::vector<data::v1::TermPoint> terms;
};
// What a roll asks of the month that is left. A position held in it is closed
// on the roll day, so a series that is traded needs the month to settle that
// day. A series that is only read does not: a month still dominant on its last
// day simply hands over on the next.
enum class RollDay { must_settle, may_have_expired };
// Which of a product's months is dominant on each trading day. `months` are in
// delivery-month order, each once. The dominant month of a day is the one with
// the largest open interest on the previous trading day, among the current
// month and later ones: it never moves back to an earlier month, and a change
// takes effect on the next trading day, so the first day belongs to no month.
// A term point compares the dominant month with the later month most held the
// day before, at that day's settlements.
//
// Fails rather than guesses: when no month has data on a day, when the
// outgoing month did not settle on a roll day it must settle on, or when the
// two months lack a common settlement on the day before a roll (the
// adjustment ratio).
DominantPlan plan_dominant(std::span<const DominantMonth> months, RollDay);
} // namespace asterion::data
