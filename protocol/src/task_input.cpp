#include "task_input.hpp"
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/backtest.hpp>
#include <asterion/protocol/data_client.hpp>
#include <asterion/protocol/data.hpp>
#include <asterion/protocol/factor.hpp>
namespace asterion::protocol {
void resolve_task_input(task::v1::TaskAttempt& attempt, std::stop_token stop) {
  auto& task = *attempt.mutable_task();
  if (task.kind() == task::v1::MINUTE_DOWNLOAD || task.kind() == task::v1::DAILY_DOWNLOAD) {
    if (attempt.has_execution())
      throw std::invalid_argument("unexpected calculation definition");
    return;
  }
  const auto& execution = attempt.execution();
  if (task.definition_case() != task::v1::Task::DEFINITION_NOT_SET ||
      execution.input_sha256().size() != 64 ||
      (task.kind() == task::v1::BACKTEST ? !execution.has_backtest() : !execution.has_factor()))
    throw std::invalid_argument("invalid calculation execution definition");
  DataClient data(attempt.data_endpoint(), attempt.data_instance());
  const auto check_stop = [&] {
    if (stop.stop_requested())
      throw Error(ErrorCode::cancelled, "task cancelled");
  };
  const auto bars = [&](const data::v1::BarDatasetRequest& selection) {
    check_stop();
    data::v1::DataRequest request;
    *request.mutable_bar_dataset() = selection;
    auto reply = data.call(std::move(request));
    check_stop();
    return std::move(*reply.mutable_bar_dataset());
  };
  std::string digest;
  if (execution.has_backtest()) {
    const auto& parameters = execution.backtest();
    if (!parameters.series().empty())
      throw std::invalid_argument("calculation requires fixed dominant schedules");
    auto* input = task.mutable_input();
    input->set_version(10);
    *input->mutable_series() = execution.schedules();
    auto* paper = input->mutable_paper();
    *paper->mutable_deposit() = parameters.deposit();
    *paper->mutable_risk() = parameters.risk();
    std::size_t count = 0;
    for (const auto& contract : parameters.contracts()) {
      auto dataset = bars(contract.data());
      count += static_cast<std::size_t>(dataset.bars_size());
      if (count > max_dataset_bars)
        throw std::invalid_argument("dataset exceeds 200000 bars; narrow the date range");
      auto* item = paper->add_contracts();
      *item->mutable_dataset() = std::move(dataset);
      *item->mutable_cost_schedule() = contract.cost_schedule();
    }
    set_backtest_strategies(*input, parameters);
    input->set_dataset_revision(dataset_revision(*paper));
    digest = sha256_bytes(input->SerializeAsString());
  } else {
    const auto& parameters = execution.factor();
    auto* input = task.mutable_factor();
    *input = factor_input(parameters);
    int schedule = 0;
    for (const auto& source : parameters.series()) {
      check_stop();
      if (source.has_dominant()) {
        // The fixed months under their fixed schedule: nothing is selected again.
        if (schedule == execution.schedules_size())
          throw std::invalid_argument("calculation requires fixed dominant schedules");
        factor::v1::FactorSeries series;
        for (const auto& month : source.dominant().months())
          *series.mutable_dominant()->add_months() = bars(month);
        *series.mutable_dominant()->mutable_schedule() = execution.schedules(schedule++);
        add_factor_series(*input, std::move(series));
        continue;
      }
      auto reply = data.call(factor_series_query(source));
      check_stop();
      add_factor_series(*input, factor_series(source, std::move(reply)));
    }
    input->set_dataset_revision(factor_revision(input->series()));
    digest = sha256_bytes(input->SerializeAsString());
  }
  if (digest != execution.input_sha256())
    throw std::invalid_argument("calculation input does not match fixed task evidence");
  check_stop();
}
} // namespace asterion::protocol
