#include <asterion/domain/trading_schedule.hpp>
#include <asterion/foundation/decimal.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/data.hpp>
#include <asterion/protocol/factor.hpp>
#include <asterion/protocol/research.hpp>
#include <charconv>
namespace asterion::protocol {
std::string dataset_revision(const v1::PaperInput &input) {
  return make_trade_dataset(input.contract(), input.ticks()).revision();
}
research::v1::BacktestInput encode_backtest(const Json &input) {
  require_fields(input, {"version", "dataset_revision", "paper", "sma", "days",
                         "calendar_publication"});
  if (!input.at("version").is_number_integer() || input.at("version") != 5)
    throw std::invalid_argument("unsupported backtest input version");
  research::v1::BacktestInput result;
  result.set_version(5);
  result.set_dataset_revision(input.at("dataset_revision").get<std::string>());
  encode_settlement_days(input.at("days"), *result.mutable_days());
  if (!input.at("calendar_publication").is_null())
    *result.mutable_calendar_publication() =
        encode_calendar_publication(input.at("calendar_publication"));
  *result.mutable_paper() = encode_input(input.at("paper"));
  const auto &sma = input.at("sma");
  require_fields(sma, {"fast", "slow", "quantity"});
  for (auto name : {"fast", "slow"})
    if (!sma.at(name).is_number_integer() || sma.at(name) < 1 ||
        sma.at(name) > 10000)
      throw std::invalid_argument("invalid SMA period");
  result.mutable_sma()->set_fast(sma.at("fast").get<unsigned>());
  result.mutable_sma()->set_slow(sma.at("slow").get<unsigned>());
  result.mutable_sma()->mutable_quantity()->set_units(
      Decimal::parse(sma.at("quantity").get<std::string>()).raw());
  static_cast<void>(decode_backtest(result));
  return result;
}
Json decode_backtest(const research::v1::BacktestInput &input) {
  validate_message(input);
  if (input.version() != 5 || !input.has_paper() || !input.has_sma() ||
      !input.sma().has_quantity() || input.days().empty() ||
      input.days_size() > 64)
    throw std::invalid_argument("incomplete backtest input");
  if (input.dataset_revision() != dataset_revision(input.paper()))
    throw std::invalid_argument(
        "dataset revision does not match input snapshot");
  const auto days = decode_settlement_days(input.days());
  for (const auto &day : input.days())
    if (day.settlement_price().units() <= 0)
      throw std::invalid_argument(
          "backtest requires positive settlement prices");
  Json publication = nullptr;
  if (input.has_calendar_publication()) {
    publication = decode_calendar_publication(input.calendar_publication());
    if (publication.at("calendar").at("days") != days ||
        publication.at("calendar").at("contract") !=
            decode_contract(input.paper().contract()))
      throw std::invalid_argument(
          "calendar publication does not match backtest contract and days");
  }
  return {{"version", 5},
          {"calendar_publication", publication},
          {"days", days},
          {"dataset_revision", input.dataset_revision()},
          {"paper", decode_input(input.paper())},
          {"sma",
           {{"fast", input.sma().fast()},
            {"slow", input.sma().slow()},
            {"quantity",
             Decimal::from_raw(input.sma().quantity().units()).str()}}}};
}
Json decode_backtest_result(const research::v1::BacktestResult &result) {
  validate_message(result);
  if (result.version() != 3 || !result.has_account() ||
      !result.has_max_drawdown() || result.equity().empty() ||
      result.settlements().empty() || result.settlements_size() > 64)
    throw std::invalid_argument("incomplete backtest result");
  Json curve = Json::array(), settlements = Json::array();
  for (const auto &point : result.equity()) {
    if (!point.has_equity() || point.timestamp_ns() < 0 ||
        (point.event() != research::v1::TRADE_MARK &&
         point.event() != research::v1::DAILY_SETTLEMENT))
      throw std::invalid_argument("invalid backtest equity event");
    curve.push_back(
        {{"timestamp_ns", std::to_string(point.timestamp_ns())},
         {"equity", Decimal::from_raw(point.equity().units()).str()},
         {"event",
          point.event() == research::v1::TRADE_MARK ? "trade" : "settlement"}});
  }
  for (const auto &day : result.settlements()) {
    if (!day.has_price() || !day.has_balance() || !day.has_equity() ||
        !day.has_realized() || !day.has_fees() || !day.has_position_quantity())
      throw std::invalid_argument("incomplete settlement result");
    settlements.push_back(
        {{"trading_day", day.trading_day()},
         {"timestamp_ns", std::to_string(day.timestamp_ns())},
         {"price", Decimal::from_raw(day.price().units()).str()},
         {"balance", Decimal::from_raw(day.balance().units()).str()},
         {"equity", Decimal::from_raw(day.equity().units()).str()},
         {"realized", Decimal::from_raw(day.realized().units()).str()},
         {"fees", Decimal::from_raw(day.fees().units()).str()},
         {"position_quantity",
          Decimal::from_raw(day.position_quantity().units()).str()}});
  }
  auto account = protocol::decode_snapshot(result.account());
  account["mode"] = "backtest";
  account["persistent"] = false;
  return {
      {"version", result.version()},
      {"dataset_revision", result.dataset_revision()},
      {"engine_version", result.engine_version()},
      {"account", account},
      {"equity", curve},
      {"settlements", settlements},
      {"max_drawdown", Decimal::from_raw(result.max_drawdown().units()).str()}};
}
Json decode_task(const research::v1::Task &task) {
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
  if (task.kind() != research::v1::BACKTEST &&
      task.kind() != research::v1::FACTOR &&
      task.kind() != research::v1::DATA_IMPORT &&
      task.kind() != research::v1::CALENDAR_IMPORT)
    throw std::invalid_argument("invalid task kind");
  return {{"kind", task.kind() == research::v1::CALENDAR_IMPORT
                       ? "calendar_import"
                   : task.kind() == research::v1::DATA_IMPORT ? "data_import"
                   : task.kind() == research::v1::FACTOR      ? "factor"
                                                              : "backtest"},
          {"id", task.id()},
          {"state", state},
          {"attempt", task.attempt()},
          {"completed", task.completed()},
          {"total", task.total()},
          {"error", task.error()},
          {"result_digest", task.result_digest()},
          {"trading_day", task.trading_day()},
          {"instrument", task.instrument()},
          {"source_name", task.source_name()},
          {"submission_sequence", task.submission_sequence()},
          {"submitted_at_ms", task.submitted_at_ms()},
          {"updated_at_ms", task.updated_at_ms()}};
}

Json decode_task_result(const research::v1::TaskResponse &response,
                        const std::string &id) {
  validate_message(response);
  if (!response.has_result_task())
    throw std::invalid_argument("research result is missing its task evidence");
  const auto &task = response.result_task();
  const auto metadata = decode_task(task);
  if (task.id() != id || task.state() != research::v1::SUCCEEDED ||
      !task.attempt() || task.completed() != task.total() ||
      task.result_digest().size() != 64 ||
      task.result_digest().find_first_not_of("0123456789abcdef") !=
          std::string::npos)
    throw std::invalid_argument(
        "research result task identity or completion mismatch");
  Json envelope = {
      {"id", id}, {"kind", metadata.at("kind")}, {"task", metadata}};
  auto range = [](const Json &ticks) {
    if (!ticks.is_array() || ticks.empty())
      throw std::invalid_argument("research input has no observations");
    return Json{{"count", ticks.size()},
                {"first_timestamp_ns", ticks.front().at("timestamp_ns")},
                {"last_timestamp_ns", ticks.back().at("timestamp_ns")}};
  };
  if (task.kind() == research::v1::BACKTEST && task.has_input() &&
      response.has_backtest()) {
    auto experiment = decode_backtest(task.input());
    if (response.backtest().dataset_revision() !=
        task.input().dataset_revision())
      throw std::invalid_argument(
          "backtest result does not belong to input data");
    experiment["data"] = range(experiment.at("paper").at("ticks"));
    experiment["paper"].erase("ticks");
    envelope["experiment"] = std::move(experiment);
    envelope["result"] = decode_backtest_result(response.backtest());
  } else if (task.kind() == research::v1::FACTOR && task.has_factor() &&
             response.has_factor()) {
    auto experiment = decode_factor(task.factor());
    if (response.factor().dataset_revision() !=
        task.factor().dataset_revision())
      throw std::invalid_argument(
          "factor result does not belong to input data");
    experiment["data"] = range(experiment.at("ticks"));
    experiment.erase("ticks");
    envelope["experiment"] = std::move(experiment);
    envelope["result"] = decode_factor_result(response.factor());
  } else if (task.kind() == research::v1::CALENDAR_IMPORT &&
             task.has_calendar() && response.has_calendar_publication()) {
    const auto input = decode_calendar_snapshot(task.calendar());
    const auto result =
        decode_calendar_publication(response.calendar_publication());
    for (const auto *name : {"source_name", "source_sha256"})
      if (input.at(name) != result.at(name))
        throw std::invalid_argument("calendar result source mismatch");
    if (input.at("contract") != result.at("calendar").at("contract") ||
        task.calendar().contents().size() !=
            response.calendar_publication().source_bytes())
      throw std::invalid_argument(
          "calendar result contract or source size mismatch");
    envelope["result"] = result;
  } else if (task.kind() == research::v1::DATA_IMPORT && task.has_data() &&
             response.has_publication()) {
    const auto input = decode_csv_snapshot(task.data());
    const auto result = decode_publication(response.publication());
    for (const auto *name : {"source_name", "source_sha256"})
      if (input.at(name) != result.at(name))
        throw std::invalid_argument(
            "publication result does not belong to input source");
    envelope["result"] = result;
  } else {
    throw std::invalid_argument("research task input and result kind mismatch");
  }
  return envelope;
}

} // namespace asterion::protocol
