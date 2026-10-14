#include <asterion/protocol/task_execution.hpp>
#include <asterion/protocol/data.hpp>
#include <stdexcept>
namespace asterion::protocol {
data::v1::BarDatasetRequest bar_dataset_selection(const data::v1::BarDataset& dataset) {
  data::v1::BarDatasetRequest result;
  result.set_contract(history_identity(dataset.contract()).key());
  *result.mutable_source_dataset_ids() = dataset.source_dataset_ids();
  *result.mutable_daily() = dataset.daily();
  // Its days are every day of the range it settled on, with bars or without.
  result.set_begin_day(dataset.days(0).trading_day());
  result.set_end_day(dataset.days(dataset.days_size() - 1).trading_day());
  return result;
}
task::v1::TaskExecution task_execution(const task::v1::Task& task,
                                       const std::string& input_sha256) {
  task::v1::TaskExecution result;
  result.set_input_sha256(input_sha256);
  if (task.has_input()) {
    const auto& input = task.input();
    auto* parameters = result.mutable_backtest();
    *parameters->mutable_deposit() = input.paper().deposit();
    *parameters->mutable_risk() = input.paper().risk();
    *parameters->mutable_strategies() = input.strategies();
    parameters->set_holdout_from(input.holdout_day());
    if (input.has_walk_forward())
      *parameters->mutable_walk_forward() = input.walk_forward();
    for (const auto& contract : input.paper().contracts()) {
      auto* item = parameters->add_contracts();
      *item->mutable_data() = bar_dataset_selection(contract.dataset());
      *item->mutable_cost_schedule() = contract.cost_schedule();
      item->set_slippage_ticks(contract.slippage_ticks());
    }
    *result.mutable_schedules() = input.series();
  } else if (task.has_factor()) {
    const auto& input = task.factor();
    auto* parameters = result.mutable_factor();
    for (const auto& series : input.series()) {
      if (series.has_bars())
        *parameters->add_series()->mutable_bars() = bar_dataset_selection(series.bars());
      else if (series.has_table()) {
        // Instruments read under one snapshot and range are asked for together.
        const auto& table = series.table();
        const bool same = parameters->series_size() &&
                          parameters->series(parameters->series_size() - 1).has_tables() &&
                          [&](const data::v1::TableSeriesRequest& last) {
                            return last.snapshot() == table.snapshot() &&
                                   last.daily_source() == table.daily_source() &&
                                   last.calendar_source() == table.calendar_source() &&
                                   last.begin_day() == table.begin_day() &&
                                   last.end_day() == table.end_day();
                          }(parameters->series(parameters->series_size() - 1).tables());
        auto* request =
            same ? parameters->mutable_series(parameters->series_size() - 1)->mutable_tables()
                 : parameters->add_series()->mutable_tables();
        request->set_snapshot(table.snapshot());
        request->set_daily_source(table.daily_source());
        request->set_calendar_source(table.calendar_source());
        request->set_begin_day(table.begin_day());
        request->set_end_day(table.end_day());
        request->add_instruments(table.instrument());
      } else {
        // The months as they were fixed, and their schedule beside them.
        auto* months = parameters->add_series()->mutable_dominant();
        for (const auto& month : series.dominant().months())
          *months->add_months() = bar_dataset_selection(month);
        *result.add_schedules() = series.dominant().schedule();
      }
    }
    parameters->set_factor(input.factor());
    *parameters->mutable_lookbacks() = input.lookbacks();
    parameters->set_horizon(input.horizon());
    if (input.has_full_sample())
      parameters->set_full_sample(input.full_sample());
    else if (input.has_holdout_start())
      parameters->set_holdout_start(input.holdout_start());
    else
      *parameters->mutable_walk_forward() = input.walk_forward();
  } else {
    throw std::invalid_argument("missing calculation input");
  }
  return result;
}
} // namespace asterion::protocol
