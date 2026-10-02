#include <asterion/domain/daily_bars.hpp>
#include <asterion/foundation/decimal.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/data.hpp>
#include <asterion/protocol/factor.hpp>
#include <asterion/protocol/research.hpp>
#include <charconv>
#include <stdexcept>
namespace asterion::protocol {
namespace {
research::v1::SmaStrategy sma(const Json& value) {
  require_fields(value, {"fast", "slow", "quantity"});
  for (auto name : {"fast", "slow"})
    if (!value.at(name).is_number_integer() || value.at(name) < 1 || value.at(name) > 10000)
      throw std::invalid_argument("invalid SMA period");
  research::v1::SmaStrategy result;
  result.set_fast(value.at("fast").get<unsigned>());
  result.set_slow(value.at("slow").get<unsigned>());
  result.mutable_quantity()->set_units(
      Decimal::parse(value.at("quantity").get<std::string>()).raw());
  return result;
}
} // namespace
research::v1::BacktestInput encode_backtest(const Json& input) {
  require_fields(input, {"version", "dataset_revision", "paper", "sma"});
  if (!input.at("version").is_number_integer() || input.at("version") != 8)
    throw std::invalid_argument("unsupported backtest input version");
  research::v1::BacktestInput result;
  result.set_version(8);
  result.set_dataset_revision(input.at("dataset_revision").get<std::string>());
  *result.mutable_paper() = encode_input(input.at("paper"));
  *result.mutable_sma() = sma(input.at("sma"));
  static_cast<void>(decode_backtest(result));
  return result;
}
research::v1::BacktestRequest encode_backtest_request(const Json& input) {
  require_fields(input, {"contracts", "deposit", "risk", "sma"});
  if (!input.at("contracts").is_array() || input.at("contracts").empty() ||
      input.at("contracts").size() > max_portfolio_contracts)
    throw std::invalid_argument("backtest requires 1 to 20 contracts");
  research::v1::BacktestRequest result;
  for (const auto& contract : input.at("contracts")) {
    require_fields(contract, {"data", "cost_schedule"});
    auto* item = result.add_contracts();
    *item->mutable_data() = encode_bar_dataset_request(contract.at("data"));
    *item->mutable_cost_schedule() = encode_cost_schedule(contract.at("cost_schedule"));
  }
  result.mutable_deposit()->set_units(Decimal::parse(input.at("deposit").get<std::string>()).raw());
  *result.mutable_risk() = encode_risk(input.at("risk"));
  *result.mutable_sma() = sma(input.at("sma"));
  return result;
}
Json decode_backtest(const research::v1::BacktestInput& input) {
  validate_message(input);
  if (input.version() != 8 || !input.has_paper() || !input.has_sma() || !input.sma().has_quantity())
    throw std::invalid_argument("incomplete backtest input");
  if (input.dataset_revision() != dataset_revision(input.paper()))
    throw std::invalid_argument("dataset revision does not match input snapshot");
  // Evaluated before the braced initializer: GCC < 13 leaks already-built
  // initializer_list elements when a later element throws (PR66139).
  auto paper = decode_input(input.paper());
  return {{"version", 8},
          {"dataset_revision", input.dataset_revision()},
          {"paper", std::move(paper)},
          {"sma",
           {{"fast", input.sma().fast()},
            {"slow", input.sma().slow()},
            {"quantity", Decimal::from_raw(input.sma().quantity().units()).str()}}}};
}
Json decode_backtest_result(const research::v1::BacktestResult& result) {
  validate_message(result);
  if (result.version() != 5 || !result.has_account() || !result.has_max_drawdown() ||
      result.equity().empty() || result.settlements().empty() ||
      static_cast<std::size_t>(result.settlements_size()) > max_dataset_bars)
    throw std::invalid_argument("incomplete backtest result");
  Json curve = Json::array(), settlements = Json::array();
  for (const auto& point : result.equity()) {
    if (!point.has_equity() || point.timestamp_ns() < 0 ||
        (point.event() != research::v1::TRADE_MARK &&
         point.event() != research::v1::DAILY_SETTLEMENT))
      throw std::invalid_argument("invalid backtest equity event");
    curve.push_back(
        {{"timestamp_ns", std::to_string(point.timestamp_ns())},
         {"equity", Decimal::from_raw(point.equity().units()).str()},
         {"event", point.event() == research::v1::TRADE_MARK ? "trade" : "settlement"}});
  }
  for (const auto& day : result.settlements()) {
    if (!day.has_balance() || !day.has_equity() || !day.has_realized() || !day.has_fees() ||
        day.contracts().empty())
      throw std::invalid_argument("incomplete settlement result");
    Json contracts = Json::array();
    for (const auto& c : day.contracts()) {
      if (!c.has_price() || !c.has_position_quantity())
        throw std::invalid_argument("incomplete settlement result");
      InstrumentId{c.venue(), c.symbol()}.validate();
      contracts.push_back(
          {{"venue", c.venue()},
           {"symbol", c.symbol()},
           {"price", Decimal::from_raw(c.price().units()).str()},
           {"position_quantity", Decimal::from_raw(c.position_quantity().units()).str()}});
    }
    settlements.push_back({{"trading_day", day.trading_day()},
                           {"timestamp_ns", std::to_string(day.timestamp_ns())},
                           {"balance", Decimal::from_raw(day.balance().units()).str()},
                           {"equity", Decimal::from_raw(day.equity().units()).str()},
                           {"realized", Decimal::from_raw(day.realized().units()).str()},
                           {"fees", Decimal::from_raw(day.fees().units()).str()},
                           {"contracts", std::move(contracts)}});
  }
  auto account = protocol::decode_snapshot(result.account());
  account["mode"] = "backtest";
  account["persistent"] = false;
  return {{"version", result.version()},
          {"dataset_revision", result.dataset_revision()},
          {"engine_version", result.engine_version()},
          {"account", account},
          {"equity", curve},
          {"settlements", settlements},
          {"max_drawdown", Decimal::from_raw(result.max_drawdown().units()).str()}};
}
Json decode_task(const research::v1::Task& task) {
  validate_message(task);
  if (!task.submission_sequence() || task.submission_sequence() > 1000 ||
      task.submitted_at_ms() <= 0 || task.submitted_at_ms() > 9223372036854LL ||
      task.updated_at_ms() <= 0 || task.updated_at_ms() > 9223372036854LL)
    throw std::invalid_argument("invalid task chronology");
  std::string state;
  switch (task.state()) {
  case research::v1::QUEUED:
    state = "queued";
    break;
  case research::v1::RUNNING:
    state = "running";
    break;
  case research::v1::CANCEL_REQUESTED:
    state = "cancel_requested";
    break;
  case research::v1::SUCCEEDED:
    state = "succeeded";
    break;
  case research::v1::FAILED:
    state = "failed";
    break;
  case research::v1::CANCELLED:
    state = "cancelled";
    break;
  case research::v1::INTERRUPTED:
    state = "interrupted";
    break;
  default:
    throw std::invalid_argument("invalid task state");
  }
  if (task.kind() != research::v1::BACKTEST && task.kind() != research::v1::FACTOR &&
      task.kind() != research::v1::DAILY_FACTOR && task.kind() != research::v1::MINUTE_DOWNLOAD &&
      task.kind() != research::v1::DAILY_DOWNLOAD)
    throw std::invalid_argument("invalid task kind");
  Json output = {{"kind", task.kind() == research::v1::DAILY_FACTOR      ? "daily_factor"
                          : task.kind() == research::v1::DAILY_DOWNLOAD  ? "daily_download"
                          : task.kind() == research::v1::MINUTE_DOWNLOAD ? "minute_download"
                          : task.kind() == research::v1::FACTOR          ? "factor"
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
  if (task.kind() == research::v1::MINUTE_DOWNLOAD) {
    const auto interval =
        task.has_minutes() ? task.minutes().interval_minutes() : task.minute_interval_minutes();
    if (interval < 1 || interval > 1440)
      throw std::invalid_argument("invalid minute dataset page response");
    output["minute_interval_minutes"] = interval;
  }
  return output;
}

Json decode_task_result(const research::v1::TaskResponse& response, const std::string& id) {
  validate_message(response);
  if (!response.has_result_task())
    throw std::invalid_argument("research result is missing its task evidence");
  const auto& task = response.result_task();
  const auto metadata = decode_task(task);
  if (task.id() != id || task.state() != research::v1::SUCCEEDED || !task.attempt() ||
      task.completed() != task.total() || task.result_digest().size() != 64 ||
      task.result_digest().find_first_not_of("0123456789abcdef") != std::string::npos)
    throw std::invalid_argument("research result task identity or completion mismatch");
  const auto kind = metadata.at("kind");
  Json envelope = {{"id", id}, {"kind", kind}, {"task", metadata}};
  // Summarizes bars instead of echoing them.
  auto range = [](Json& dataset) {
    const auto& bars = dataset.at("bars");
    if (!bars.is_array() || bars.empty())
      throw std::invalid_argument("research input has no observations");
    Json summary{{"count", bars.size()},
                 {"first_timestamp_ns", bars.front().at("timestamp_ns")},
                 {"last_timestamp_ns", bars.back().at("timestamp_ns")},
                 {"first_day", bars.front().at("trading_day")},
                 {"last_day", bars.back().at("trading_day")},
                 {"interval_minutes", dataset.at("interval_minutes")},
                 {"source", dataset.at("source")},
                 {"source_dataset_ids", dataset.at("source_dataset_ids")}};
    dataset.erase("bars");
    dataset.erase("days");
    return summary;
  };
  if (task.kind() == research::v1::DAILY_FACTOR && task.has_daily_factor() &&
      response.has_daily_factor()) {
    const auto decoded = decode_daily_factor(task.daily_factor(), response.daily_factor());
    envelope["experiment"] = decoded.at("experiment");
    envelope["result"] = decoded.at("result");
  } else if (task.kind() == research::v1::DAILY_DOWNLOAD && task.has_daily() &&
             response.has_daily()) {
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
  } else if (task.kind() == research::v1::MINUTE_DOWNLOAD && task.has_minutes() &&
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
  } else if (task.kind() == research::v1::BACKTEST && task.has_input() && response.has_backtest()) {
    auto experiment = decode_backtest(task.input());
    if (response.backtest().dataset_revision() != task.input().dataset_revision())
      throw std::invalid_argument("backtest result does not belong to input data");
    Json data = Json::array();
    for (auto& contract : experiment.at("paper").at("contracts"))
      data.push_back(range(contract.at("dataset")));
    experiment["data"] = std::move(data);
    envelope["experiment"] = std::move(experiment);
    envelope["result"] = decode_backtest_result(response.backtest());
  } else if (task.kind() == research::v1::FACTOR && task.has_factor() && response.has_factor()) {
    auto experiment = decode_factor(task.factor());
    if (response.factor().dataset_revision() != task.factor().dataset_revision())
      throw std::invalid_argument("factor result does not belong to input data");
    experiment["data"] = range(experiment.at("dataset"));
    envelope["experiment"] = std::move(experiment);
    envelope["result"] = decode_factor_result(response.factor());
  } else {
    throw std::invalid_argument("research task input and result kind mismatch");
  }
  return envelope;
}

} // namespace asterion::protocol
