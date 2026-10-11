#pragma once
#include "history_tables.hpp"
#include <asterion/v1/data.pb.h>
namespace asterion::data {
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
