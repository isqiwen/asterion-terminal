#pragma once
#include "history_tables.hpp"
#include <asterion/foundation/decimal.hpp>
#include <asterion/v1/data.pb.h>
#include <map>
#include <string>
#include <vector>
namespace asterion::data {
// A day a contract traded on in the auction: the table has all five of these
// for it.
struct DailyTrade {
  Decimal open, high, low, close, volume;
};
// What contracts of one exchange did on the trading days of a range, as the
// daily table holds it under a snapshot. A day a contract has no row on, or
// no value for, is not in its map.
struct DailyFacts {
  // By contract, then by trading day.
  std::map<std::string, std::map<std::string, Decimal>> settlement, open_interest;
  // Read only where trades are asked for: the days a contract traded on. A
  // day it did not has a settlement, and a close equal to it, and is not here.
  std::map<std::string, std::map<std::string, DailyTrade>> trades;
  // When the partition of each trading day of the range was acquired.
  std::map<std::string, std::int64_t> acquired_at_ns;
};
// A name is a contract (SHFE/rb/2026-10), or a product ending in a slash
// (SHFE/rb/) for every contract of it. Fails rather than reads around a hole,
// as table_series does.
DailyFacts daily_facts(const history_files::TableStore&, const v1::DailyTable&,
                       const std::string& venue, const std::string& begin_day,
                       const std::string& end_day, const std::vector<std::string>& names,
                       bool trades = false);
// The series of a request's instruments, read from the market-wide daily table
// as the request's snapshot holds it. Nothing is read of another snapshot, so
// the same request always makes the same series.
//
// A contract is its closes on the days it has one. A product is its dominant
// series (see plan_dominant): the months of the product that have rows in the
// range, each read while it is dominant. A product's series therefore begins
// on the second trading day of the range.
//
// Fails rather than reads around a hole: when the snapshot's calendar does not
// cover the range, or a day the calendar has open is not stored.
v1::TableSeriesGroup table_series(const history_files::TableStore&, const v1::TableSeriesRequest&);
// The products of every exchange the snapshot holds a contract list of.
v1::TableProducts table_products(const history_files::TableStore&, const v1::TableProductsQuery&);
} // namespace asterion::data
