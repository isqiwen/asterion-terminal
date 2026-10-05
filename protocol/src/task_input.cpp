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
      (task.kind() == task::v1::BACKTEST ? !execution.has_backtest()
       : task.kind() == task::v1::FACTOR ? !execution.has_factor()
                                         : !execution.has_daily_factor()))
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
    input->set_version(8);
    *input->mutable_sma() = parameters.sma();
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
    input->set_dataset_revision(dataset_revision(*paper));
    digest = sha256_bytes(input->SerializeAsString());
  } else if (execution.has_factor()) {
    const auto& parameters = execution.factor();
    auto* input = task.mutable_factor();
    input->set_version(5);
    *input->mutable_dataset() = bars(parameters.data());
    input->set_dataset_revision(input->dataset().revision());
    *input->mutable_lookbacks() = parameters.lookbacks();
    input->set_horizon(parameters.horizon());
    if (parameters.has_full_sample())
      input->set_full_sample(parameters.full_sample());
    else if (parameters.has_holdout_start())
      input->set_holdout_start(parameters.holdout_start());
    else if (parameters.has_walk_forward())
      *input->mutable_walk_forward() = parameters.walk_forward();
    digest = sha256_bytes(input->SerializeAsString());
  } else {
    const auto& parameters = execution.daily_factor();
    check_stop();
    data::v1::DataRequest request;
    request.mutable_daily_factor_dataset()->set_id(parameters.source_dataset_id());
    auto reply = data.call(std::move(request));
    check_stop();
    auto* input = task.mutable_daily_factor();
    input->set_version(1);
    *input->mutable_dataset() = std::move(*reply.mutable_daily_factor_dataset());
    input->set_dataset_revision(daily_factor_revision(input->dataset()));
    input->set_lookback(parameters.lookback());
    input->set_horizon(parameters.horizon());
    if (parameters.has_full_sample())
      input->set_full_sample(parameters.full_sample());
    else if (parameters.has_holdout_start())
      input->set_holdout_start(parameters.holdout_start());
    digest = sha256_bytes(input->SerializeAsString());
  }
  if (digest != execution.input_sha256())
    throw std::invalid_argument("calculation input does not match fixed task evidence");
  check_stop();
}
} // namespace asterion::protocol
