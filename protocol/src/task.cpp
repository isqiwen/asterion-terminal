#include <asterion/protocol/task.hpp>
#include <asterion/protocol/factor.hpp>
#include <asterion/protocol/data.hpp>
#include <asterion/domain/daily_bars.hpp>
#include <stdexcept>
namespace asterion::protocol {
Json decode_task(const task::v1::Task& task) {
  validate_message(task);
  if (!task.submission_sequence() || task.submitted_at_ms() <= 0 ||
      task.submitted_at_ms() > 9223372036854LL || task.updated_at_ms() <= 0 ||
      task.updated_at_ms() > 9223372036854LL)
    throw std::invalid_argument("invalid task chronology");
  std::string state;
  switch (task.state()) {
  case task::v1::QUEUED:
    state = "queued";
    break;
  case task::v1::RUNNING:
    state = "running";
    break;
  case task::v1::CANCEL_REQUESTED:
    state = "cancel_requested";
    break;
  case task::v1::PUBLISHING:
    state = "publishing";
    break;
  case task::v1::SUCCEEDED:
    state = "succeeded";
    break;
  case task::v1::FAILED:
    state = "failed";
    break;
  case task::v1::CANCELLED:
    state = "cancelled";
    break;
  case task::v1::INTERRUPTED:
    state = "interrupted";
    break;
  default:
    throw std::invalid_argument("invalid task state");
  }
  if (task.kind() != task::v1::BACKTEST && task.kind() != task::v1::FACTOR &&
      task.kind() != task::v1::DAILY_FACTOR && task.kind() != task::v1::MINUTE_DOWNLOAD &&
      task.kind() != task::v1::DAILY_DOWNLOAD)
    throw std::invalid_argument("invalid task kind");
  Json output = {{"kind", task.kind() == task::v1::DAILY_FACTOR      ? "daily_factor"
                          : task.kind() == task::v1::DAILY_DOWNLOAD  ? "daily_download"
                          : task.kind() == task::v1::MINUTE_DOWNLOAD ? "minute_download"
                          : task.kind() == task::v1::FACTOR          ? "factor"
                                                                     : "backtest"},
                 {"id", task.id()},
                 {"provider_artifact", task.provider_artifact()},
                 {"risk_artifact", task.risk_artifact()},
                 {"state", state},
                 {"attempt", task.attempt()},
                 {"completed", task.completed()},
                 {"total", task.total()},
                 {"error", task.error()},
                 {"result_digest", task.result_digest()},
                 {"trading_day", task.trading_day()},
                 {"instrument", task.instrument()},
                 {"source_name", task.source_name()},
                 {"history_dataset_id", task.history_dataset_id()},
                 {"data_source", task.has_daily()     ? task.daily().source()
                                 : task.has_minutes() ? task.minutes().source()
                                                      : task.data_source()},
                 {"submission_sequence", task.submission_sequence()},
                 {"submitted_at_ms", task.submitted_at_ms()},
                 {"updated_at_ms", task.updated_at_ms()}};
  if (task.kind() == task::v1::MINUTE_DOWNLOAD) {
    const auto interval =
        task.has_minutes() ? task.minutes().interval_minutes() : task.minute_interval_minutes();
    if (interval < 1 || interval > 1440)
      throw std::invalid_argument("invalid minute dataset page response");
    output["minute_interval_minutes"] = interval;
  }
  return output;
}

Json decode_task_result(const task::v1::TaskResponse& response, const std::string& id) {
  validate_message(response);
  if (!response.has_result_task())
    throw std::invalid_argument("task result is missing its task evidence");
  const auto& task = response.result_task();
  const auto metadata = decode_task(task);
  if (task.id() != id || task.state() != task::v1::SUCCEEDED || !task.attempt() ||
      task.completed() != task.total() || task.result_digest().size() != 64 ||
      task.result_digest().find_first_not_of("0123456789abcdef") != std::string::npos)
    throw std::invalid_argument("task result task identity or completion mismatch");
  const auto kind = metadata.at("kind");
  Json envelope = {{"id", id}, {"kind", kind}, {"task", metadata}};
  // Summarizes bars instead of echoing them.
  auto range = [](const data::v1::BarDataset& dataset) -> Json {
    const auto& first = dataset.bars(0);
    const auto& last = dataset.bars(dataset.bars_size() - 1);
    return {{"count", dataset.bars_size()},
            {"first_timestamp_ns", std::to_string(first.timestamp_ns())},
            {"last_timestamp_ns", std::to_string(last.timestamp_ns())},
            {"first_day", first.trading_day()},
            {"last_day", last.trading_day()},
            {"interval_minutes", dataset.interval_minutes()},
            {"source", dataset.source()},
            {"source_dataset_ids", std::vector<std::string>(dataset.source_dataset_ids().begin(),
                                                            dataset.source_dataset_ids().end())}};
  };
  if (task.kind() == task::v1::DAILY_FACTOR && task.has_daily_factor() &&
      response.has_daily_factor()) {
    const auto decoded = decode_daily_factor(task.daily_factor(), response.daily_factor());
    envelope["experiment"] = decoded.at("experiment");
    envelope["result"] = decoded.at("result");
  } else if (task.kind() == task::v1::DAILY_DOWNLOAD && task.has_daily() && response.has_daily()) {
    const auto& result = response.daily();
    const auto& input = task.daily();
    const auto begin = parse_trading_date(input.begin_day());
    const auto end = parse_trading_date(input.end_day());
    const auto span = (std::chrono::sys_days(end) - std::chrono::sys_days(begin)).count();
    if (input.version() != 2 || input.contract_id().empty() || span < 0 || span > 20 * 366 ||
        result.version() != 2 || result.manifest_sha256().size() != 64 ||
        result.manifest_sha256().find_first_not_of("0123456789abcdef") != std::string::npos ||
        result.directory().empty() || result.pages() != static_cast<unsigned>(span / 366 + 1) ||
        result.rows() > static_cast<std::uint64_t>(span + 1))
      throw std::invalid_argument("invalid daily download result");
    envelope["result"] = {{"directory", result.directory()},
                          {"manifest_sha256", result.manifest_sha256()},
                          {"rows", result.rows()},
                          {"pages", result.pages()}};
    envelope["experiment"] = {{"source", input.source()},
                              {"contract_id", input.contract_id()},
                              {"begin_day", input.begin_day()},
                              {"end_day", input.end_day()}};
  } else if (task.kind() == task::v1::MINUTE_DOWNLOAD && task.has_minutes() &&
             response.has_minutes()) {
    const auto& result = response.minutes();
    const auto& input = task.minutes();
    if (result.version() != 2 || result.manifest_sha256().size() != 64 ||
        result.directory().empty())
      throw std::invalid_argument("invalid minute download result");
    envelope["result"] = {{"directory", result.directory()},
                          {"manifest_sha256", result.manifest_sha256()},
                          {"rows", result.rows()},
                          {"pages", result.pages()}};
    envelope["experiment"] = {{"source", input.source()},
                              {"contract_id", input.contract_id()},
                              {"interval_minutes", input.interval_minutes()},
                              {"begin_ns", std::to_string(input.begin_ns())},
                              {"end_ns", std::to_string(input.end_ns())}};
  } else if (task.kind() == task::v1::BACKTEST && task.has_input() && response.has_backtest()) {
    auto experiment = decode_backtest(task.input(), DatasetView::metadata);
    if (response.backtest().dataset_revision() != task.input().dataset_revision())
      throw std::invalid_argument("backtest result does not belong to input data");
    Json data = Json::array();
    for (const auto& contract : task.input().paper().contracts())
      data.push_back(range(contract.dataset()));
    experiment["data"] = std::move(data);
    envelope["experiment"] = std::move(experiment);
    envelope["result"] = decode_backtest_result(response.backtest());
  } else if (task.kind() == task::v1::FACTOR && task.has_factor() && response.has_factor()) {
    auto experiment = decode_factor(task.factor(), DatasetView::metadata);
    if (response.factor().dataset_revision() != task.factor().dataset_revision())
      throw std::invalid_argument("factor result does not belong to input data");
    experiment["data"] = range(task.factor().dataset());
    envelope["experiment"] = std::move(experiment);
    envelope["result"] = decode_factor_result(response.factor());
  } else {
    throw std::invalid_argument("task input and result kind mismatch");
  }
  return envelope;
}

} // namespace asterion::protocol
