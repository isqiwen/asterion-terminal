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
  // What a lot of a contract was worth on a day it has a turnover: the amount
  // over the volume, which is a price times the contract's multiplier. It
  // checks a multiplier and is no ledger figure.
  std::map<std::string, std::map<std::string, long double>> lot_value;
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
// A product's price increment on each day of [begin_day, end_day] it traded
// on, as its prices show it: the contract table has only today's, and an
// exchange changes it (gold traded on 0.05 until 2019-12-09, on 0.02 since).
//
// Every price of every month that traded in the auction on a day is a whole
// multiple of the day's increment; their greatest common divisor over a few
// days can be a multiple of it by chance, over twenty never was in the data.
// So a day's increment is the greatest common divisor of the prices of the
// increment_days days the product traded on up to it, or of those from it
// on, whichever is coarser: across a change the window that reaches over it
// divides both increments, and the other is all on one. Near either end of
// what is read a window takes the first or the last of those days, and with
// fewer than that, all of them. What is read is the range and the
// increment_days stored days of the exchange on either side of it, so that a
// day's increment does not depend on the range it is read in.
inline constexpr std::size_t increment_days = 20;
std::map<std::string, Decimal> price_increments(const history_files::TableStore&,
                                                const v1::DailyTable&, const std::string& venue,
                                                const std::string& product,
                                                const std::string& begin_day,
                                                const std::string& end_day);
// What a lot of each contract is worth per unit of its price, from the
// snapshot's contract table: its multiplier (one where it has none) times its
// lot in the unit it is traded in, taken to the unit its price is quoted per.
// Fails for a contract the table lacks, and for units it cannot read.
std::map<std::string, Decimal> contract_multipliers(const history_files::TableStore&,
                                                    const v1::DailyTable&, const std::string& venue,
                                                    const std::vector<std::string>& contracts);
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
// cover the range, or a day the calendar has open is not stored. Instruments
// that cannot be read over a whole range (no closes, or a product whose months
// leave a day without a dominant one) are refused together, each named with
// why, so that a long list is corrected once.
v1::TableSeriesGroup table_series(const history_files::TableStore&, const v1::TableSeriesRequest&);
// The products of every exchange the snapshot holds a contract list of.
v1::TableProducts table_products(const history_files::TableStore&, const v1::TableProductsQuery&);
// The contracts of one product of one exchange in a snapshot's contract list
// that have a listing day, a delisting day and the source's code for them,
// ascending; what a download of one of them asks for. `source` names the
// reply; the list read is `contracts_source`.
v1::HistoryCatalog table_contracts(const history_files::TableStore&, const std::string& snapshot,
                                   const std::string& source, const std::string& contracts_source,
                                   const std::string& exchange, const std::string& product);
// The part of [first, last] the snapshot's daily table holds of a contract's
// exchange, from the first such day to the last, and the days of it the
// contract traded in the auction, ascending. Empty when the table holds none
// of them. Fails rather than reads around a hole inside that part, as
// table_series does.
struct TradedDays {
  std::string first, last;
  std::vector<std::string> traded;
};
TradedDays traded_days(const history_files::TableStore&, const v1::DailyTable&,
                       const std::string& contract, const std::string& first,
                       const std::string& last);
// One contract as a chart draws it: the days it traded in the auction, from
// the day the snapshot's contract list lists it to the day it delists it, and
// of those the stored ones; grouped by the period asked for, with MACD from
// the first candle. Unlike a series it does not fail on a day the table lacks:
// that day is not drawn. A contract the contract list lacks is refused.
v1::TableDailyPage table_daily(const history_files::TableStore&, const v1::TableDailyQuery&);
} // namespace asterion::data
