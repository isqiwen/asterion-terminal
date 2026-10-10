#include <asterion/protocol/task_execution.hpp>
#include <algorithm>
#include <stdexcept>
namespace asterion::protocol {
namespace {
data::v1::BarDatasetRequest selection(const data::v1::BarDataset& dataset) {
  data::v1::BarDatasetRequest result;
  *result.mutable_contract() = dataset.contract();
  *result.mutable_source_dataset_ids() = dataset.source_dataset_ids();
  *result.mutable_settlement_dataset_ids() = dataset.settlement_dataset_ids();
  // Include reported boundary gaps too: they are part of the fixed evidence.
  auto begin = dataset.days(0).trading_day();
  auto end = dataset.days(dataset.days_size() - 1).trading_day();
  for (const auto& day : dataset.uncovered_days()) {
    begin = std::min(begin, day);
    end = std::max(end, day);
  }
  result.set_begin_day(begin);
  result.set_end_day(end);
  return result;
}
} // namespace
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
    for (const auto& contract : input.paper().contracts()) {
      auto* item = parameters->add_contracts();
      *item->mutable_data() = selection(contract.dataset());
      *item->mutable_cost_schedule() = contract.cost_schedule();
    }
    *result.mutable_schedules() = input.series();
  } else if (task.has_factor()) {
    const auto& input = task.factor();
    auto* parameters = result.mutable_factor();
    for (const auto& series : input.series()) {
      if (series.has_bars())
        *parameters->add_series()->mutable_bars() = selection(series.bars());
      else if (series.has_daily())
        parameters->add_series()->set_daily_dataset_id(series.daily().source_dataset_id());
      else {
        // The months as they were fixed, and their schedule beside them.
        auto* months = parameters->add_series()->mutable_dominant();
        for (const auto& month : series.dominant().months())
          *months->add_months() = selection(month);
        *result.add_schedules() = series.dominant().schedule();
      }
    }
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
