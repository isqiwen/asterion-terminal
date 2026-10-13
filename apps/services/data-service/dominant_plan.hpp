#pragma once
#include <asterion/foundation/decimal.hpp>
#include <asterion/v1/data.pb.h>
#include <map>
#include <span>
#include <string>
#include <vector>
namespace asterion::data {
// One month contract of a product, as the dominant rule reads it: what it
// settled at and how much of it was held, by trading day, as the daily table
// has them whether it traded that day or not.
struct DominantMonth {
  std::string name; // how a failure names it
  std::string delivery_month;
  std::map<std::string, Decimal> settlement, open_interest;
  // The last trading day the month has bars on; empty where it has none.
  // Only a series that is traded reads it.
  std::string last_bar;
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
  std::vector<std::string> days; // every day one of the months settled on, ascending
  std::vector<DominantSegment> segments;
  std::vector<data::v1::TermPoint> terms;
};
// What a series is for. One that is traded must be able to leave a month: on
// its last day the month hands over to the most held of the others, whatever
// it holds itself, and it has bars on the roll day or after it, on which what
// is held of it is closed. A series that is only read needs neither: a month
// still dominant on its last day simply hands over on the next.
enum class SeriesUse { traded, read };
// Which of a product's months is dominant on each trading day. `months` are in
// delivery-month order, each once. The dominant month of a day is the one with
// the largest open interest on the previous trading day, among the current
// month and later ones that settle that day: it never moves back to an
// earlier month, and a change takes effect on the next trading day, so the
// first day belongs to no month. A month need not trade on a day to be
// dominant on it: a day it is halted, or nobody trades it, changes nothing of
// what is held. A term point compares the dominant month with the later month
// most held the day before, at that day's settlements.
//
// Fails rather than guesses: when no month can be dominant on a day, when a
// series that is traded leaves a month that has no bars from the roll day
// on, or when the two months lack a common settlement on the day before a
// roll (the adjustment ratio).
DominantPlan plan_dominant(std::span<const DominantMonth> months, SeriesUse);
} // namespace asterion::data
