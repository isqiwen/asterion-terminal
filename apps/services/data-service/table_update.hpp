#pragma once
#include "history_tables.hpp"
#include <asterion/v1/data_service.pb.h>
namespace asterion::data {
// A table one installed plugin provides, as the registry reports it.
struct TableOffer {
  history_files::TableSeries series;
  std::string plugin_id;
  std::vector<std::string> venues;
};
// The tables every day table depends on.
inline constexpr std::string_view contracts_table = "futures.contracts";
inline constexpr std::string_view calendar_table = "futures.calendar";
// What one step of an update still has to read. `offers` are the tables of
// the update's plugin; `today` is the current date in Asia/Shanghai. Only days
// that have ended are planned: a source may publish a day in parts while it is
// open. A step needs what the one before stored, and refuses to guess without
// it: the calendar is planned from the year an exchange listed its first
// contract, the days from the calendar.
v1::TableUpdatePlan table_update_plan(const history_files::TableStore&,
                                      const std::vector<TableOffer>& offers, const v1::TableUpdate&,
                                      v1::TablePlanStep, const std::string& today);
// Whether a partition is one this update may deliver.
bool table_update_covers(const std::vector<TableOffer>& offers, const v1::TableUpdate&,
                         const v1::TablePartitionKey&);
v1::TableCoverage table_coverage(const history_files::TableStore&,
                                 const std::vector<TableOffer>& installed);
v1::TablePage table_page(const history_files::TableStore&, const v1::TablePageQuery&);
v1::TableRevision encode_table_revision(const history_files::TableRevision&);
} // namespace asterion::data
