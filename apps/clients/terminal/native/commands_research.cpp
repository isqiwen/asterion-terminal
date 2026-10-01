#include "application_impl.hpp"
#include <asterion/domain/history_identity.hpp>

#include <stdexcept>
#include <algorithm>

namespace asterion::terminal {
data::v1::MinutePageQuery minute_page_query(const json& p) {
  auto parameters = p;
  parameters.erase("archive");
  fields(parameters, {"id", "offset", "limit", "start", "end", "include_macd"});
  if (!p.at("include_macd").is_boolean())
    throw std::invalid_argument("invalid minute dataset page query");
  if (!p.at("offset").is_number_integer() || p.at("offset") < 0 || p.at("offset") > 60000000 ||
      !p.at("limit").is_number_integer() || p.at("limit") < 1 || p.at("limit") > 200)
    throw std::invalid_argument("invalid minute dataset page query");
  data::v1::MinutePageQuery query;
  if (p.contains("archive") && !p.at("archive").is_boolean())
    throw std::invalid_argument("invalid historical archive query");
  if (p.value("archive", false))
    query.set_dataset_id(text(p, "id"));
  else
    query.set_task_id(text(p, "id"));
  query.set_include_macd(p.at("include_macd").get<bool>());
  query.set_offset(p.at("offset").get<std::uint64_t>());
  query.set_limit(p.at("limit").get<unsigned>());
  if (!p.at("start").is_string() || !p.at("end").is_string())
    throw std::invalid_argument("invalid minute dataset page query");
  if (!p.at("start").get_ref<const std::string&>().empty())
    query.set_begin_ns(parse_shanghai_time(text(p, "start")));
  if (!p.at("end").get_ref<const std::string&>().empty())
    query.set_end_ns(parse_shanghai_time(text(p, "end")));
  return query;
}

data::v1::DailyPageQuery daily_page_query(const json& p) {
  auto parameters = p;
  parameters.erase("archive");
  fields(parameters, {"id", "offset", "limit", "start", "end", "include_macd", "period"});
  if (!p.at("include_macd").is_boolean() || !p.at("offset").is_number_integer() ||
      p.at("offset") < 0 || p.at("offset") > 20 * 366 + 1 || !p.at("limit").is_number_integer() ||
      p.at("limit") < 1 || p.at("limit") > 200 || !p.at("start").is_string() ||
      !p.at("end").is_string())
    throw std::invalid_argument("invalid daily dataset page query");
  data::v1::DailyPageQuery query;
  if (p.contains("period") && !p.at("period").is_string())
    throw std::invalid_argument("invalid daily dataset page query");
  const auto period = text(p, "period");
  if (period == "day")
    query.set_period(data::v1::DAY);
  else if (period == "week")
    query.set_period(data::v1::WEEK);
  else if (period == "month")
    query.set_period(data::v1::MONTH);
  else if (period == "quarter")
    query.set_period(data::v1::QUARTER);
  else if (period == "year")
    query.set_period(data::v1::YEAR);
  else
    throw std::invalid_argument("invalid daily dataset page query");
  if (p.contains("archive") && !p.at("archive").is_boolean())
    throw std::invalid_argument("invalid historical archive query");
  if (p.value("archive", false))
    query.set_dataset_id(text(p, "id"));
  else
    query.set_task_id(text(p, "id"));
  validate_id(text(p, "id"));
  query.set_offset(p.at("offset").get<std::uint64_t>());
  query.set_limit(p.at("limit").get<unsigned>());
  query.set_include_macd(p.at("include_macd").get<bool>());
  query.set_begin_day(p.at("start").get<std::string>());
  query.set_end_day(p.at("end").get<std::string>());
  for (const auto& date : {query.begin_day(), query.end_day()})
    if (!date.empty())
      (void)parse_trading_date(date);
  if (!query.begin_day().empty() && !query.end_day().empty() && query.begin_day() > query.end_day())
    throw std::invalid_argument("invalid daily dataset page query");
  return query;
}
// Research tasks: dataset selection, backtest, factor and data-source downloads.
void Application::Impl::register_research_commands() {
  core.command("research.datasets", "node.manage", [this](const json& p) {
    fields(p, {"venue", "product", "contract_id", "source"});
    if (!research)
      throw std::invalid_argument("connect research service first");
    return snapshot();
  });
  core.command("research.coverage", "node.manage", [this](const json& p) {
    fields(p, {"venue", "product", "contract_id", "source"});
    if (!research)
      throw std::invalid_argument("connect research service first");
    return snapshot();
  });
  core.command("research.daily.page", "node.manage", [this](const json& p) {
    (void)daily_page_query(p);
    if (!research)
      throw std::invalid_argument("connect research service first");
    auto published = read_published(json::object());
    return published.is_null() ? snapshot() : published;
  });
  core.command("research.daily.submit", "node.manage", [this](const json& p) {
    fields(p, {"id", "source", "contract_id", "requests_per_minute", "token", "catalog_cutoff_ns",
               "connection", "connection_revision"});
    if (!research)
      throw std::invalid_argument("connect research service first");
    const auto& rpm = p.at("requests_per_minute");
    if (!rpm.is_number_integer() || rpm < 1 || rpm > 500)
      throw std::invalid_argument("invalid daily download definition");
    if (text(p, "connection", true) != history_connection ||
        text(p, "connection_revision", true) != history_connection_revision ||
        text(p, "source") != history_source ||
        text(p, "catalog_cutoff_ns") != std::to_string(history_cutoff))
      throw std::invalid_argument("contract catalog changed; select the contract again");
    const auto found =
        std::ranges::find(history_contracts, text(p, "contract_id"),
                          [](const HistoryListing& item) { return item.identity.key(); });
    if (found == history_contracts.end())
      throw std::invalid_argument("load and select a dated futures contract first");
    const auto end =
        std::min(found->delist_date, format_shanghai_time(history_cutoff).substr(0, 10));
    data::v1::DailyDownload input;
    input.set_version(2);
    input.set_source(text(p, "source"));
    input.set_contract_id(found->identity.key());
    input.set_source_instrument(found->source_instrument);
    input.set_begin_day(found->list_date);
    input.set_end_day(end);
    input.set_requests_per_minute(rpm.get<unsigned>());
    auto credential = text(p, "token", true);
    if (p.contains("connection") && !text(p, "connection", true).empty()) {
      if (!credential.empty())
        throw std::invalid_argument("choose a saved connection or a temporary credential");
      const auto connection = resolve_data_connection(
          text(p, "connection"), text(p, "connection_revision"), input.source());
      if (input.requests_per_minute() != connection.requests_per_minute)
        throw std::invalid_argument("data connection request budget changed; inspect again");
      credential = connection.credential;
    }
    research->submit(text(p, "id"), input, credential);
    return snapshot();
  });
  core.command("research.minutes.page", "node.manage", [this](const json& p) {
    (void)minute_page_query(p);
    if (!research)
      throw std::invalid_argument("connect research service first");
    auto published = read_published(json::object());
    return published.is_null() ? snapshot() : published;
  });
  core.command("research.contracts.load", "node.manage", [this](const json& p) {
    fields(p, {"source", "exchange", "product", "token", "connection", "connection_revision"});
    return snapshot();
  });
  core.command("research.minutes.submit", "node.manage", [this](const json& p) {
    fields(p, {"id", "source", "contract_id", "interval_minutes", "requests_per_minute", "token",
               "catalog_cutoff_ns", "connection", "connection_revision"});
    if (!research)
      throw std::invalid_argument("connect research service first");
    for (const auto* name : {"interval_minutes", "requests_per_minute"}) {
      const auto& value = p.at(name);
      if (!value.is_number_integer() || value < 1 ||
          value > (std::string_view(name) == "interval_minutes" ? 1440 : 500))
        throw std::invalid_argument("invalid minute download definition");
    }
    data::v1::MinuteDownload input;
    input.set_version(2);
    input.set_source(text(p, "source"));
    input.set_contract_id(text(p, "contract_id"));
    input.set_interval_minutes(p.at("interval_minutes").get<unsigned>());
    input.set_requests_per_minute(p.at("requests_per_minute").get<unsigned>());
    if (text(p, "connection", true) != history_connection ||
        text(p, "connection_revision", true) != history_connection_revision ||
        text(p, "source") != history_source ||
        text(p, "catalog_cutoff_ns") != std::to_string(history_cutoff))
      throw std::invalid_argument("contract catalog changed; select the contract again");
    const auto found =
        std::ranges::find(history_contracts, input.contract_id(),
                          [](const HistoryListing& item) { return item.identity.key(); });
    if (found == history_contracts.end())
      throw std::invalid_argument("load and select a dated futures contract first");
    input.set_source_instrument(found->source_instrument);
    input.set_begin_ns(parse_shanghai_time(found->list_date + " 00:00:00"));
    input.set_end_ns(std::min(history_cutoff / 1000000000 * 1000000000,
                              parse_shanghai_time(found->delist_date + " 23:59:59")));
    auto credential = text(p, "token", true);
    if (p.contains("connection") && !text(p, "connection", true).empty()) {
      if (!credential.empty())
        throw std::invalid_argument("choose a saved connection or a temporary credential");
      const auto connection = resolve_data_connection(
          text(p, "connection"), text(p, "connection_revision"), input.source());
      if (input.requests_per_minute() != connection.requests_per_minute)
        throw std::invalid_argument("data connection request budget changed; inspect again");
      credential = connection.credential;
    }
    research->submit(text(p, "id"), input, credential);
    return snapshot();
  });
  core.command("research.local", "node.manage", [this](const json& p) {
    fields(p, {});
    auto [node, next] = without_operations([existing = existing_local_node()] {
      auto node = local_node_client(existing);
      auto client = std::make_shared<ResearchClient>(node->local_research());
      return std::pair{std::move(node), std::move(client)};
    });
    nodes.try_emplace("local", std::move(node));
    research = std::move(next);
    research_result = nullptr;
    ++research_generation;
    return snapshot();
  });
  core.command("research.attach", "node.manage", [this](const json& p) {
    fields(p, {"id", "service"});
    auto next = std::make_shared<ResearchClient>(
        nodes.at(text(p, "id"))
            ->service_endpoint(text(p, "service"), asterion::node::v1::TASK_SERVICE));
    research = std::move(next);
    research_result = nullptr;
    ++research_generation;
    return snapshot();
  });
  // Selects downloaded bars and a contract specification; the research
  // service resolves and verifies them. Required before paper trading,
  // backtests, factors and strategy runs.
  core.command("research.dataset.select", "node.manage", [this](const json& p) {
    fields(p, {"source_task_id", "settlement_task_id", "begin_day", "end_day", "price_increment",
               "multiplier"});
    if (!research)
      throw std::invalid_argument("research service is not connected");
    // The contract comes from the download's unified identity; only the units
    // a data source does not provide are entered by the user.
    std::string key;
    for (const auto& task : research->tasks())
      if (task.at("id") == p.at("source_task_id") && task.at("state") == "succeeded" &&
          (task.at("kind") == "minute_download" || task.at("kind") == "daily_download"))
        key = task.at("instrument").get<std::string>();
    if (key.empty())
      throw std::invalid_argument("select a completed minute or daily download");
    const auto id = HistoryIdentity::parse(key).exchange_id();
    const auto identity = HistoryIdentity::parse(key);
    auto product = id.symbol.substr(0, identity.product.size());
    const auto request =
        protocol::encode_bar_dataset_request({{"source_task_id", p.at("source_task_id")},
                                              {"settlement_task_id", p.at("settlement_task_id")},
                                              {"begin_day", p.at("begin_day")},
                                              {"end_day", p.at("end_day")},
                                              {"contract",
                                               {{"venue", id.venue},
                                                {"symbol", id.symbol},
                                                {"currency", "CNY"},
                                                {"price_increment", text(p, "price_increment")},
                                                {"quantity_increment", "1"},
                                                {"multiplier", text(p, "multiplier")},
                                                {"product", std::move(product)},
                                                {"delivery_month", identity.delivery_month}}}});
    auto dataset = research->bar_dataset(request);
    const auto& bars = dataset.bars();
    const auto& first = bars.Get(0);
    const auto& last = bars.Get(bars.size() - 1);
    json uncovered = json::array();
    for (const auto& day : dataset.uncovered_days())
      uncovered.push_back(day);
    json summary = protocol::decode_bar_dataset_request(request);
    summary.update({{"venue", dataset.contract().venue()},
                    {"symbol", dataset.contract().symbol()},
                    {"revision", dataset.revision()},
                    {"source", dataset.source()},
                    {"interval_minutes", dataset.interval_minutes()},
                    {"count", bars.size()},
                    {"days", dataset.days_size()},
                    {"first_day", first.trading_day()},
                    {"last_day", last.trading_day()},
                    {"first_timestamp_ns", std::to_string(first.timestamp_ns())},
                    {"last_timestamp_ns", std::to_string(last.timestamp_ns())},
                    {"last_close", Decimal::from_raw(last.close().units()).str()},
                    {"uncovered_days", uncovered}});
    // One dataset per contract: selecting a contract again replaces it.
    DatasetSelection next{request, std::move(dataset), std::move(summary)};
    const auto same = std::ranges::find_if(selections, [&](const DatasetSelection& item) {
      return item.dataset.contract().venue() == next.dataset.contract().venue() &&
             item.dataset.contract().symbol() == next.dataset.contract().symbol();
    });
    if (same != selections.end())
      *same = std::move(next);
    else if (selections.size() == max_portfolio_contracts)
      throw std::invalid_argument("a portfolio holds at most 20 contracts");
    else
      selections.push_back(std::move(next));
    return snapshot();
  });
  core.command("research.dataset.remove", "node.manage", [this](const json& p) {
    fields(p, {"venue", "symbol"});
    const auto count = std::erase_if(selections, [&](const DatasetSelection& item) {
      return item.dataset.contract().venue() == text(p, "venue") &&
             item.dataset.contract().symbol() == text(p, "symbol");
    });
    if (!count)
      throw std::invalid_argument("this contract is not selected");
    return snapshot();
  });
  core.command("research.dataset.clear", "node.manage", [this](const json& p) {
    fields(p, {});
    selections.clear();
    return snapshot();
  });
  core.command("research.submit", "node.manage", [this](const json& p) {
    fields_with_risk(p, {"id", "fast", "slow", "quantity", "deposit", "contracts"});
    if (!research)
      throw std::invalid_argument("research service is not connected");
    const auto costs = selection_costs(p.at("contracts"));
    json contracts = json::array();
    for (std::size_t i = 0; i < selected().size(); ++i)
      contracts.push_back({{"data", protocol::decode_bar_dataset_request(selected()[i].request)},
                           {"costs", costs[i]}});
    auto request = protocol::encode_backtest_request(
        {{"contracts", std::move(contracts)},
         {"deposit", text(p, "deposit")},
         {"risk", risk_parameters(p)},
         {"sma",
          {{"fast", p.at("fast")}, {"slow", p.at("slow")}, {"quantity", text(p, "quantity")}}}});
    research->submit(text(p, "id"), request);
    return snapshot();
  });
  core.command("research.daily-factor.submit", "node.manage", [this](const json& p) {
    fields(p, {"id", "source_task_id", "lookback", "horizon", "evaluation"});
    validate_id(text(p, "id"));
    auto definition = p;
    definition.erase("id");
    (void)protocol::encode_daily_factor_request(definition);
    if (!research)
      throw std::invalid_argument("research service is not connected");
    auto published = read_published(json::object());
    return published.is_null() ? snapshot() : published;
  });
  core.command("research.factor.submit", "node.manage", [this](const json& p) {
    fields(p, {"id", "lookbacks", "horizon", "evaluation"});
    if (!research)
      throw std::invalid_argument("research service is not connected");
    if (selected().size() != 1)
      throw std::invalid_argument("factor analysis studies one contract; keep one dataset");
    const auto request = protocol::encode_factor_request(
        {{"data", protocol::decode_bar_dataset_request(selected().front().request)},
         {"lookbacks", p.at("lookbacks")},
         {"horizon", p.at("horizon")},
         {"evaluation", p.at("evaluation")}});
    research->submit(text(p, "id"), request);
    return snapshot();
  });
  core.command("research.action", "node.manage", [this](const json& p) {
    fields(p, {"id", "action"});
    if (!research)
      throw std::invalid_argument("research service is not connected");
    research->action(text(p, "id"), text(p, "action"));
    return snapshot();
  });
  core.command("research.result", "node.manage", [this](const json& p) {
    fields(p, {"id"});
    if (!research)
      throw std::invalid_argument("research service is not connected");
    validate_id(text(p, "id"));
    auto published = read_published(json::object());
    return published.is_null() ? snapshot() : published;
  });
}
} // namespace asterion::terminal
