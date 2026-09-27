#include "terminal_application.hpp"
#include "csv_market_data.hpp"
#include "market_client.hpp"
#include "moving_average.hpp"
#include "node_client.hpp"
#include "node_enrollment.hpp"
#include "remote_bundle.hpp"
#include "research_client.hpp"
#include "strategy_client.hpp"
#include "trading_client.hpp"
#include <asterion/domain/futures.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/runtime.hpp>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <mutex>
#include <stdexcept>

namespace asterion::terminal {
using nlohmann::json;
using namespace asterion;
void fields(const json& object, std::initializer_list<std::string_view> names) {
  if (!object.is_object() || object.size() != names.size())
    throw std::invalid_argument("request fields do not match the current contract");
  for (const auto name : names)
    if (!object.contains(name))
      throw std::invalid_argument("request is missing a required field");
}
json risk_parameters(const json& p) {
  const auto count = p.at("max_working_orders").get<std::string>();
  std::uint64_t parsed = 0;
  const auto [end, error] = std::from_chars(count.data(), count.data() + count.size(), parsed);
  if (error != std::errc{} || end != count.data() + count.size() ||
      std::to_string(parsed) != count || !parsed)
    throw std::invalid_argument("invalid working order limit");
  return {{"max_order_quantity", p.at("max_order_quantity")},
          {"max_gross_quantity", p.at("max_gross_quantity")},
          {"max_working_orders", parsed}};
}
std::string text(const json& object, const char* name) {
  const auto& value = object.at(name);
  if (!value.is_string())
    throw std::invalid_argument("text field has the wrong type");
  auto result = value.get<std::string>();
  if (result.empty() || result.find('\0') != std::string::npos)
    throw std::invalid_argument("text field is empty or contains an invalid character");
  return result;
}
unsigned short port_number(const json& p, const char* name) {
  const auto raw = text(p, name);
  unsigned int port = 0;
  const auto [end, ec] = std::from_chars(raw.data(), raw.data() + raw.size(), port);
  if (ec != std::errc{} || end != raw.data() + raw.size() || !port || port > 65535)
    throw std::invalid_argument("port must be between 1 and 65535");
  return static_cast<unsigned short>(port);
}
struct PreviewState {
  json dataset = nullptr;
  json replay = json::array();
  std::optional<data::v1::CsvSnapshot> source;
};
IdSequence runtime_scopes{"terminal"};
struct Application::Impl {
  std::unique_ptr<ResearchClient> research;
  std::unique_ptr<StrategyClient> strategy;
  json research_result = nullptr;
  std::unique_ptr<MarketClient> market;
  std::unique_ptr<TradingClient> paper;
  std::map<std::string, std::unique_ptr<NodeClient>> nodes;
  json firewall_plan = nullptr, firewall_parameters = nullptr, ssh_key = nullptr,
       agent_program = nullptr;
  std::chrono::steady_clock::time_point firewall_expiry{};
  Runtime core{runtime_scopes.next()};
  ResourceRegistry::Scope scope = core.resources().create_scope("terminal");
  Impl() {
    scope.publish("preview", std::make_shared<PreviewState>());
    auto positive_limit = [](const json& value) {
      return value.is_number_integer() && value > 0 && value <= 32 * 1024 * 1024;
    };
    core.configuration().declare("preview.max_bytes", 32 * 1024 * 1024, positive_limit);
    core.configuration().declare("preview.max_rows", 250000, positive_limit);
    core.access().grant("terminal.local", "runtime.read");
    core.access().grant("terminal.local", "data.inspect");
    core.command("runtime.snapshot", "runtime.read", [this](const json& params) {
      fields(params, {});
      return snapshot();
    });
    core.command("futures.inspect_csv", "data.inspect",
                 [this](const json& params) { return inspect(params); });
    core.access().grant("terminal.local", "paper.manage");
    core.command("paper.create", "paper.manage", [this](const json& p) {
      const bool remote = paper && paper->connection().at("transport") == "tcp_tls";
      if (remote)
        fields(p,
               {"deposit", "margin_per_lot", "open_fee", "close_today_fee", "close_yesterday_fee",
                "max_order_quantity", "max_gross_quantity", "max_working_orders"});
      else
        fields(p, {"directory", "deposit", "margin_per_lot", "open_fee", "close_today_fee",
                   "close_yesterday_fee", "max_order_quantity", "max_gross_quantity",
                   "max_working_orders"});
      if (paper && !remote)
        throw std::invalid_argument("close the current paper session first");
      auto preview = core.resources().resolve<PreviewState>("terminal", "preview").lock();
      if (preview->dataset.is_null())
        throw std::invalid_argument("import historical futures trades first");
      json contract = json::object();
      for (auto key : {"venue", "symbol", "currency", "price_increment", "quantity_increment",
                       "multiplier", "product", "delivery_month"})
        contract[key] = preview->dataset.at(key);
      json costs = json::object();
      for (auto key : {"margin_per_lot", "open_fee", "close_today_fee", "close_yesterday_fee"})
        costs[key] = text(p, key);
      json manifest{{"version", 1},
                    {"type", "historical_paper"},
                    {"contract", contract},
                    {"costs", costs},
                    {"risk", risk_parameters(p)},
                    {"deposit", text(p, "deposit")},
                    {"ticks", preview->replay}};
      if (remote)
        paper->create(manifest);
      else {
        if (!nodes.contains("local"))
          nodes.emplace("local", std::make_unique<NodeClient>(local_node()));
        const auto directory = text(p, "directory");
        paper = std::make_unique<TradingClient>(
            std::filesystem::path(std::u8string(directory.begin(), directory.end())), manifest);
      }
      return snapshot();
    });
    core.command("paper.connect", "paper.manage", [this](const json& p) {
      fields(p, {"host", "port", "session", "mode", "ca_file", "certificate_file",
                 "private_key_file"});
      if (paper)
        throw std::invalid_argument("disconnect the current trading connection first");
      if (text(p, "mode") != "paper")
        throw std::invalid_argument("live trading is not available");
      const auto port_text = text(p, "port");
      unsigned int port = 0;
      const auto [end, ec] =
          std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
      if (ec != std::errc{} || end != port_text.data() + port_text.size() || !port || port > 65535)
        throw std::invalid_argument("port must be between 1 and 65535");
      ServiceEndpoint config{
          text(p, "host"),
          text(p, "session"),
          static_cast<std::uint16_t>(port),
          {text(p, "ca_file"), text(p, "certificate_file"), text(p, "private_key_file")}};
      paper = std::make_unique<TradingClient>(config);
      return snapshot();
    });
    core.command("paper.reconnect", "paper.manage", [this](const json& p) {
      fields(p, {});
      if (!paper)
        throw std::invalid_argument("choose a connection profile first");
      paper->reconnect();
      return snapshot();
    });
    core.command("paper.open", "paper.manage", [this](const json& p) {
      fields(p, {"directory"});
      if (paper)
        throw std::invalid_argument("close the current paper session first");
      if (!nodes.contains("local"))
        nodes.emplace("local", std::make_unique<NodeClient>(local_node()));
      const auto directory = text(p, "directory");
      paper = std::make_unique<TradingClient>(
          std::filesystem::path(std::u8string(directory.begin(), directory.end())));
      return snapshot();
    });
    core.command("paper.close", "paper.manage", [this](const json& p) {
      fields(p, {});
      paper.reset();
      return snapshot();
    });
    core.command("paper.act", "paper.manage", [this](const json& p) {
      if (!paper)
        throw std::invalid_argument("create or recover a paper session first");
      paper->execute(p);
      return snapshot();
    });
    core.access().grant("terminal.local", "node.manage");
    core.command("node.initializer.export", "node.manage", [this](const json& p) {
      fields(p, {"path"});
      const auto destination = p.at("path").get<std::string>();
      auto result = snapshot();
      if (destination.empty())
        result["initializer"] = {{"name", "initialize-linux.py"},
                                 {"content", bundled_linux_initializer()}};
      else
        export_bundled_initializer(
            std::filesystem::path(std::u8string(destination.begin(), destination.end())));
      return result;
    });
    core.command("node.key.prepare", "node.manage", [this](const json& p) {
      fields(p, {"id"});
      ssh_key = prepare_ssh_key(text(p, "id"));
      return snapshot();
    });
    core.command("node.service_firewall", "node.manage", [this](const json& p) {
      fields(p, {"id", "service", "action", "token"});
      firewall_plan = nullptr;
      firewall_parameters = nullptr;
      firewall_plan =
          nodes.at(text(p, "id"))
              ->firewall(text(p, "service"), text(p, "action"), p.at("token").get<std::string>());
      return snapshot();
    });
    core.command("node.firewall.inspect", "node.manage", [this](const json& p) {
      fields(p, {"id", "host", "ssh_port", "username", "key_source", "private_key", "known_hosts",
                 "agent_port", "firewall_port", "firewall_action"});
      firewall_plan = nullptr;
      firewall_parameters = nullptr;
      auto plan = inspect_node_firewall(p);
      plan["transport"] = "ssh";
      plan["token"] = unique_process_id();
      firewall_parameters = p;
      firewall_parameters.erase("private_key");
      firewall_plan = std::move(plan);
      firewall_expiry = std::chrono::steady_clock::now() + std::chrono::minutes(5);
      return snapshot();
    });
    core.command("node.firewall.apply", "node.manage", [this](const json& p) {
      fields(p, {"token", "private_key"});
      if (firewall_plan.is_null() || firewall_parameters.is_null() ||
          text(p, "token") != firewall_plan.at("token").get<std::string>() ||
          std::chrono::steady_clock::now() > firewall_expiry)
        throw std::invalid_argument("firewall confirmation expired; inspect again");
      auto parameters = firewall_parameters;
      parameters["private_key"] = p.at("private_key").get<std::string>();
      firewall_parameters = nullptr;
      auto plan = firewall_plan;
      firewall_plan = nullptr;
      firewall_plan = change_node_firewall(parameters, plan);
      return snapshot();
    });
    core.command("node.bootstrap", "node.manage", [this](const json& p) {
      fields(p, {"id", "host", "ssh_port", "username", "key_source", "private_key", "known_hosts",
                 "agent_port"});
      auto config = enroll_node(p);
      const auto id = config.id;
      nodes.insert_or_assign(id, std::make_unique<NodeClient>(config));
      return snapshot();
    });
    core.command("node.connect", "node.manage", [this](const json& p) {
      fields(p, {"id"});
      auto config = enrolled_node(text(p, "id"));
      const auto id = config.id;
      nodes.insert_or_assign(id, std::make_unique<NodeClient>(config));
      return snapshot();
    });
    core.command("research.local", "node.manage", [this](const json& p) {
      fields(p, {});
      if (!nodes.contains("local"))
        nodes.emplace("local", std::make_unique<NodeClient>(local_node()));
      auto next = std::make_unique<ResearchClient>(nodes.at("local")->local_research());
      research = std::move(next);
      research_result = nullptr;
      return snapshot();
    });
    core.command("research.attach", "node.manage", [this](const json& p) {
      fields(p, {"id", "service"});
      auto next = std::make_unique<ResearchClient>(
          nodes.at(text(p, "id"))->service_endpoint(text(p, "service"), "research"));
      research = std::move(next);
      research_result = nullptr;
      return snapshot();
    });
    core.command("research.submit", "node.manage", [this](const json& p) {
      fields(p, {"id", "days", "calendar_task", "fast", "slow", "quantity", "deposit",
                 "margin_per_lot", "open_fee", "close_today_fee", "close_yesterday_fee",
                 "max_order_quantity", "max_gross_quantity", "max_working_orders"});
      if (!research)
        throw std::invalid_argument("research service is not connected");
      const auto preview = core.resources().resolve<PreviewState>("terminal", "preview").lock();
      if (preview->dataset.is_null())
        throw std::invalid_argument("import historical futures data first");
      json contract = json::object();
      for (auto key : {"venue", "symbol", "currency", "price_increment", "quantity_increment",
                       "multiplier", "product", "delivery_month"})
        contract[key] = preview->dataset.at(key);
      json costs = json::object();
      for (auto key : {"margin_per_lot", "open_fee", "close_today_fee", "close_yesterday_fee"})
        costs[key] = text(p, key);
      const json paper_input{{"version", 1},
                             {"type", "historical_paper"},
                             {"contract", contract},
                             {"costs", costs},
                             {"risk", risk_parameters(p)},
                             {"deposit", text(p, "deposit")},
                             {"ticks", preview->replay}};
      const auto encoded = protocol::encode_input(paper_input);
      json calendar_publication = nullptr;
      json days = p.at("days");
      const auto calendar_task = p.at("calendar_task").get<std::string>();
      if (!calendar_task.empty()) {
        if (!days.is_null())
          throw std::invalid_argument("published calendar cannot be combined with edited days");
        const auto evidence = research->result(calendar_task);
        if (evidence.at("kind") != "calendar_import")
          throw std::invalid_argument("selected task is not a calendar publication");
        calendar_publication = evidence.at("result");
        days = calendar_publication.at("calendar").at("days");
      }
      const json input{
          {"version", 5},
          {"calendar_publication", calendar_publication},
          {"days", days},
          {"dataset_revision", protocol::dataset_revision(encoded)},
          {"paper", paper_input},
          {"sma",
           {{"fast", p.at("fast")}, {"slow", p.at("slow")}, {"quantity", text(p, "quantity")}}}};
      research->submit(text(p, "id"), protocol::encode_backtest(input));
      return snapshot();
    });
    core.command("research.factor.submit", "node.manage", [this](const json& p) {
      fields(p, {"id", "lookbacks", "horizon", "evaluation"});
      if (!research)
        throw std::invalid_argument("research service is not connected");
      const auto preview = core.resources().resolve<PreviewState>("terminal", "preview").lock();
      if (preview->dataset.is_null())
        throw std::invalid_argument("import historical futures data first");
      json contract = json::object();
      for (auto key : {"venue", "symbol", "currency", "price_increment", "quantity_increment",
                       "multiplier", "product", "delivery_month"})
        contract[key] = preview->dataset.at(key);
      auto input = protocol::encode_factor({{"version", 4},
                                            {"dataset_revision", ""},
                                            {"contract", contract},
                                            {"ticks", preview->replay},
                                            {"lookbacks", p.at("lookbacks")},
                                            {"horizon", p.at("horizon")},
                                            {"evaluation", p.at("evaluation")}});
      input.set_dataset_revision(protocol::factor_dataset_revision(input));
      research->submit(text(p, "id"), input);
      return snapshot();
    });
    core.command("research.calendar.submit", "node.manage", [this](const json& p) {
      fields(p, {"id", "path"});
      if (!research)
        throw std::invalid_argument("research service is not connected");
      const auto preview = core.resources().resolve<PreviewState>("terminal", "preview").lock();
      if (preview->dataset.is_null())
        throw std::invalid_argument("select the futures contract first");
      const auto name = text(p, "path");
      if (name.find('\0') != std::string::npos)
        throw std::invalid_argument("invalid calendar source path");
      const std::filesystem::path path(std::u8string(name.begin(), name.end()));
      constexpr std::size_t limit = 1024 * 1024;
      if (!path.is_absolute() || std::filesystem::is_symlink(path) ||
          !std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > limit)
        throw std::invalid_argument(
            "calendar source requires an absolute regular file of at most 1 MiB");
      std::ifstream file(path, std::ios::binary);
      if (!file)
        throw std::runtime_error("cannot read calendar CSV");
      std::string bytes(limit + 1, '\0');
      file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      bytes.resize(static_cast<std::size_t>(file.gcount()));
      if (file.bad() || !file.eof() || bytes.size() > limit)
        throw std::invalid_argument("calendar source size or read failure");
      json contract = json::object();
      for (auto key : {"venue", "symbol", "currency", "price_increment", "quantity_increment",
                       "multiplier", "product", "delivery_month"})
        contract[key] = preview->dataset.at(key);
      data::v1::CalendarCsvSnapshot input;
      input.set_version(1);
      *input.mutable_contract() = protocol::encode_contract(contract);
      const auto filename = path.filename().u8string();
      input.set_source_name(std::string(filename.begin(), filename.end()));
      input.set_source_sha256(sha256_bytes(bytes));
      input.set_contents(std::move(bytes));
      static_cast<void>(protocol::decode_calendar_snapshot(input));
      research->submit(text(p, "id"), input);
      return snapshot();
    });
    core.command("research.data.submit", "node.manage", [this](const json& p) {
      fields(p, {"id"});
      if (!research)
        throw std::invalid_argument("research service is not connected");
      const auto preview = core.resources().resolve<PreviewState>("terminal", "preview").lock();
      if (!preview->source)
        throw std::invalid_argument("select a nonempty CSV with at most 10000 "
                                    "events and 4 MiB to publish");
      research->submit(text(p, "id"), *preview->source);
      return snapshot();
    });
    core.command("research.data.use", "node.manage", [this](const json& p) {
      fields(p, {"id"});
      if (!research)
        throw std::invalid_argument("research service is not connected");
      const auto response = research->result(text(p, "id"));
      if (response.at("kind") != "data_import")
        throw std::invalid_argument("not a data publication task");
      const auto& publication = response.at("result");
      const auto& data = publication.at("dataset");
      const auto& rows = data.at("ticks");
      auto next = data.at("contract");
      Decimal quantity;
      for (const auto& row : rows)
        quantity = quantity + Decimal::parse(row.at("quantity").get<std::string>());
      json recent = json::array();
      for (std::size_t i = rows.size() > 240 ? rows.size() - 240 : 0; i < rows.size(); ++i)
        recent.push_back(rows.at(i));
      next.update({{"filename", publication.at("source_name")},
                   {"count", rows.size()},
                   {"quantity", quantity.str()},
                   {"first_timestamp_ns", rows.front().at("timestamp_ns")},
                   {"last_timestamp_ns", rows.back().at("timestamp_ns")},
                   {"last_price", rows.back().at("price")},
                   {"ticks", recent},
                   {"source", "published_csv"},
                   {"persistent", true},
                   {"specification_source", "user_supplied"},
                   {"publication_ready", false},
                   {"publication_id", publication.at("id")},
                   {"revision", data.at("revision")}});
      auto preview = core.resources().resolve<PreviewState>("terminal", "preview").lock();
      preview->dataset = std::move(next);
      preview->replay = rows;
      preview->source.reset();
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
      research_result = research->result(text(p, "id"));
      return snapshot();
    });
    core.command("strategy.run", "paper.manage", [this](const json& p) {
      fields(p, {"id", "fast", "slow", "quantity", "calendar_task"});
      if (!paper || paper->endpoint().endpoint.empty())
        throw std::invalid_argument("automatic strategy setup currently "
                                    "requires a connected local paper account");
      const auto id = text(p, "id");
      validate_id(id);
      if (id.size() > 40 || id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTU"
                                                 "VWXYZ0123456789_-") != std::string::npos)
        throw std::invalid_argument("invalid strategy run identity");
      auto integer = [&](const char* key) {
        const auto raw = text(p, key);
        std::uint32_t value = 0;
        const auto [end, ec] = std::from_chars(raw.data(), raw.data() + raw.size(), value);
        if (ec != std::errc{} || end != raw.data() + raw.size() || std::to_string(value) != raw)
          throw std::invalid_argument("invalid strategy window");
        return value;
      };
      auto preview = core.resources().resolve<PreviewState>("terminal", "preview").lock();
      if (preview->dataset.is_null() || preview->replay.empty())
        throw std::invalid_argument("select a bounded futures dataset first");
      json contract = json::object();
      for (const auto* key : {"venue", "symbol", "currency", "price_increment",
                              "quantity_increment", "multiplier", "product", "delivery_month"})
        contract[key] = preview->dataset.at(key);
      strategy::v1::Config config;
      config.set_version(1);
      config.set_session_id("strategy-" + id);
      config.set_stream_id("history-" + id);
      config.set_plugin_id("asterion.strategy.cta.sma-long-flat");
      config.set_fast(integer("fast"));
      config.set_slow(integer("slow"));
      const auto quantity = Decimal::parse(text(p, "quantity"));
      config.mutable_quantity()->set_units(quantity.raw());
      *config.mutable_contract() = protocol::encode_contract(contract);
      Instrument instrument{{text(contract, "venue"), text(contract, "symbol")},
                            AssetClass::futures,
                            text(contract, "currency"),
                            Decimal::parse(text(contract, "price_increment")),
                            Decimal::parse(text(contract, "quantity_increment")),
                            Decimal::parse(text(contract, "multiplier"))};
      MovingAverage validation(instrument, config.fast(), config.slow(), quantity);
      (void)validation;
      google::protobuf::RepeatedPtrField<protocol::v1::Tick> ticks;
      for (const auto& row : preview->replay)
        *ticks.Add() = protocol::encode_tick(row);
      const auto local = local_node();
      auto* plan = config.mutable_replay();
      plan->set_version(2);
      *plan->mutable_dataset() = protocol::make_trade_dataset(config.contract(), ticks);
      plan->set_trading_session(paper->endpoint().session);
      plan->set_grant_id("grant." + config.session_id());
      plan->set_agent_endpoint(local.endpoint);
      const auto calendar_task = p.at("calendar_task").get<std::string>();
      if (!calendar_task.empty()) {
        if (!research)
          throw std::invalid_argument("research service is not connected");
        const auto evidence = research->result(calendar_task);
        if (evidence.at("kind") != "calendar_import")
          throw std::invalid_argument("selected task is not a calendar publication");
        *plan->mutable_calendar_publication() =
            protocol::encode_calendar_publication(evidence.at("result"));
      }
      if (paper->snapshot().contains("replay") && !plan->has_calendar_publication())
        throw std::invalid_argument("account already has a bound calendar");
      static_cast<void>(protocol::decode_replay_plan(*plan));
      if (!nodes.contains("local"))
        nodes.emplace("local", std::make_unique<NodeClient>(local));
      auto next =
          std::make_unique<StrategyClient>(nodes.at("local")->local_strategy(config.session_id()));
      if (plan->has_calendar_publication()) {
        const auto publication =
            protocol::decode_calendar_publication(plan->calendar_publication());
        const auto state = paper->snapshot();
        if (state.contains("replay")) {
          if (state.at("replay").at("publication") != publication)
            throw std::invalid_argument("selected calendar differs from account binding");
        } else
          paper->execute({{"request_id", "calendar." + config.session_id()},
                          {"action", "replay_calendar"},
                          {"publication", publication}});
      }
      paper->execute({{"request_id", plan->grant_id()},
                      {"action", "strategy_grant"},
                      {"grant_id", plan->grant_id()},
                      {"strategy_id", config.session_id()},
                      {"stream_id", config.stream_id()},
                      {"dataset_revision", plan->dataset().revision()},
                      {"max_quantity", quantity.str()}});
      // Keep the observation handle even if delivery acknowledgement is lost.
      // The caller retains the same run id and may explicitly retry this plan.
      strategy = std::move(next);
      strategy->create(config);
      return snapshot();
    });
    core.command("strategy.attach", "node.manage", [this](const json& p) {
      fields(p, {"id", "service"});
      if (text(p, "id") == "local" && !nodes.contains("local"))
        nodes.emplace("local", std::make_unique<NodeClient>(local_node()));
      strategy = std::make_unique<StrategyClient>(
          nodes.at(text(p, "id"))->service_endpoint(text(p, "service"), "strategy"));
      return snapshot();
    });
    core.command("strategy.revoke", "paper.manage", [this](const json& p) {
      fields(p, {"grant_id"});
      const auto grant = text(p, "grant_id");
      validate_id(grant);
      std::unique_ptr<TradingClient> observer;
      TradingClient* account = paper.get();
      if (!account || !account->snapshot().contains("strategy") ||
          account->snapshot().at("strategy").at("grant_id") != grant) {
        if (!strategy)
          throw std::invalid_argument("connect the matching paper account before revoking");
        const auto& config = strategy->config();
        const auto& plan = config.replay();
        if (plan.grant_id() != grant)
          throw std::invalid_argument("strategy authorization mismatch");
        const auto local = local_node();
        if (plan.agent_endpoint() != local.endpoint)
          throw std::invalid_argument("connect the matching paper account "
                                      "before revoking this strategy");
        if (!nodes.contains("local"))
          nodes.emplace("local", std::make_unique<NodeClient>(local));
        observer = std::make_unique<TradingClient>(
            nodes.at("local")->service_endpoint(plan.trading_session(), "paper"));
        account = observer.get();
      }
      const auto state = account->snapshot();
      if (state.at("storage_state") != "ready" || !state.contains("strategy") ||
          state.at("strategy").at("grant_id") != grant)
        throw std::invalid_argument("strategy account authorization mismatch");
      if (state.at("strategy").at("active") == true)
        account->execute({{"request_id", "revoke." + grant},
                          {"action", "strategy_revoke"},
                          {"grant_id", grant}});
      return this->snapshot();
    });
    core.command("market.local", "node.manage", [this](const json& p) {
      fields(p, {});
      if (!nodes.contains("local"))
        nodes.emplace("local", std::make_unique<NodeClient>(local_node()));
      if (!market)
        market = std::make_unique<MarketClient>(nodes.at("local")->local_market());
      return snapshot();
    });
    core.command("market.attach", "node.manage", [this](const json& p) {
      fields(p, {"id", "service"});
      auto next = std::make_unique<MarketClient>(
          nodes.at(text(p, "id"))->service_endpoint(text(p, "service"), "market"));
      market = std::move(next);
      return snapshot();
    });
    core.command("market.connect", "node.manage", [this](const json& p) {
      fields(p, {"front", "broker", "user", "password", "instruments"});
      if (!market)
        throw std::invalid_argument("start or select a market service first");
      market->connect(p);
      return snapshot();
    });
    core.command("market.subscribe", "node.manage", [this](const json& p) {
      fields(p, {"instruments"});
      if (!market)
        throw std::invalid_argument("select a market service first");
      market->subscribe(p.at("instruments"));
      return snapshot();
    });
    core.command("market.disconnect", "node.manage", [this](const json& p) {
      fields(p, {});
      if (market)
        market->disconnect();
      return snapshot();
    });
    core.command("node.agent.upgrade", "node.manage", [this](const json& p) {
      fields(p, {"expected_digest"});
      const auto expected = text(p, "expected_digest");
      agent_program = local_node_program_status();
      if ((agent_program.at("state") != "update_available" &&
           agent_program.at("state") != "recovery_required") ||
          agent_program.at("expected_digest") != expected)
        throw std::runtime_error("inspect the current Agent update before continuing");
      try {
        const auto endpoint = upgrade_local_node(expected);
        nodes.erase("local");
        nodes.emplace("local", std::make_unique<NodeClient>(endpoint));
        agent_program = local_node_program_status();
      } catch (...) {
        try {
          agent_program = local_node_program_status();
        } catch (...) {
          agent_program = nullptr;
        }
        throw;
      }
      return snapshot();
    });
    core.command("node.agent.inspect", "node.manage", [this](const json& p) {
      fields(p, {});
      agent_program = nullptr;
      agent_program = local_node_program_status();
      return snapshot();
    });
    core.command("node.local", "node.manage", [this](const json& p) {
      fields(p, {});
      if (!nodes.contains("local"))
        nodes.emplace("local", std::make_unique<NodeClient>(local_node()));
      return snapshot();
    });
    core.command("node.disconnect", "node.manage", [this](const json& p) {
      fields(p, {"id"});
      if (text(p, "id") == "local")
        throw std::invalid_argument("the local node monitor cannot be removed");
      nodes.erase(text(p, "id"));
      return snapshot();
    });
    core.command("node.attach", "node.manage", [this](const json& p) {
      fields(p, {"id", "service"});
      if (paper)
        throw std::invalid_argument("disconnect the current trading session first");
      paper = std::make_unique<TradingClient>(
          nodes.at(text(p, "id"))->service_endpoint(text(p, "service")));
      return snapshot();
    });
    core.command("node.deploy", "node.manage", [this](const json& p) {
      fields(p, {"id", "service", "port", "kind"});
      auto& node = *nodes.at(text(p, "id"));
      const auto status = node.status();
      if (status.at("state") != "online" || status.at("health").at("os") != "linux" ||
          status.at("health").at("version") != ASTERION_PRODUCT_VERSION)
        throw std::invalid_argument(
            "remote services require an online Linux Agent of the same version");
      const auto arch = status.at("health").at("arch").get<std::string>();
      const auto kind = text(p, "kind");
      if (kind != "paper" && kind != "market" && kind != "research")
        throw std::invalid_argument("invalid service kind");
      node.deploy(bundled_linux_program(arch, kind == "market"     ? "asterion-market-data"
                                              : kind == "research" ? "asterion-task-service"
                                                                   : "asterion-trading"),
                  "linux", arch, text(p, "service"), port_number(p, "port"), {}, kind,
                  kind == "market" && arch == "x86_64" ? bundled_linux_program(arch, "ctp-md.so")
                                                       : std::filesystem::path{},
                  kind == "research" ? bundled_linux_program(arch, "asterion-backtest")
                                     : std::filesystem::path{},
                  kind == "research" ? bundled_linux_program(arch, "asterion-factor")
                                     : std::filesystem::path{},
                  kind == "research" ? bundled_linux_program(arch, "asterion-data-pipeline")
                                     : std::filesystem::path{});
      return snapshot();
    });
    core.command("node.update", "node.manage", [this](const json& p) {
      fields(p, {"id", "service", "revision"});
      const auto id = text(p, "id"), service = text(p, "service");
      auto& node = *nodes.at(id);
      const auto state = node.status();
      if (state.at("state") != "online")
        throw std::invalid_argument("connect the node before updating");
      const auto& health = state.at("health");
      const auto os = text(health, "os"), arch = text(health, "arch");
      std::string kind;
      for (const auto& s : health.at("services"))
        if (s.at("id") == service)
          kind = text(s, "kind");
      if (kind != "paper" && kind != "market" && kind != "research" && kind != "strategy")
        throw std::invalid_argument("unknown service kind");
      auto program = [&](const char* variable, const char* name) {
        if (id != "local")
          return bundled_linux_program(arch, name);
        const auto* configured = std::getenv(variable);
        return configured ? std::filesystem::path(
                                std::u8string(configured, configured + std::strlen(configured)))
                          : current_executable().parent_path() /
                                (std::string(name) + (os == "windows" ? ".exe" : ""));
      };
      const auto executable =
          kind == "paper"      ? program("ASTERION_TRADING_EXECUTABLE", "asterion-trading")
          : kind == "market"   ? program("ASTERION_MARKET_EXECUTABLE", "asterion-market-data")
          : kind == "research" ? program("ASTERION_TASK_EXECUTABLE", "asterion-task-service")
                               : program("ASTERION_STRATEGY_EXECUTABLE", "asterion-strategy");
      std::filesystem::path provider;
      if (kind == "market") {
        if (id != "local")
          provider = bundled_linux_program(arch, "ctp-md.so");
        else {
          const auto* configured = std::getenv("ASTERION_CTP_LIBRARY");
          provider = configured ? std::filesystem::path(std::u8string(
                                      configured, configured + std::strlen(configured)))
                                : current_executable().parent_path() /
                                      ("ctp-md" + std::string(os == "windows" ? ".dll"
                                                              : os == "macos" ? ".dylib"
                                                                              : ".so"));
        }
      }
      node.update(executable, os, arch, service, text(p, "revision"), provider,
                  kind == "research" ? program("ASTERION_BACKTEST_EXECUTABLE", "asterion-backtest")
                                     : std::filesystem::path{},
                  kind == "research" ? program("ASTERION_FACTOR_EXECUTABLE", "asterion-factor")
                                     : std::filesystem::path{},
                  kind == "research"
                      ? program("ASTERION_DATA_PIPELINE_EXECUTABLE", "asterion-data-pipeline")
                      : std::filesystem::path{});
      return snapshot();
    });
    core.command("node.action", "node.manage", [this](const json& p) {
      fields(p, {"id", "service", "action"});
      nodes.at(text(p, "id"))->action(text(p, "service"), text(p, "action"));
      return snapshot();
    });
    core.start();
  }
  json snapshot() {
    const auto preview = core.resources().resolve<PreviewState>("terminal", "preview").lock();
    const auto metrics = core.observations().metrics();
    const auto account = paper ? paper->snapshot() : json(nullptr);
    json node_status = json::array();
    for (const auto& [id, node] : nodes) {
      (void)id;
      node_status.push_back(node->status());
    }
    return {
        {"research", research ? research->status() : json(nullptr)},
        {"research_result", research_result},
        {"strategy", strategy ? strategy->status() : json(nullptr)},
        {"market", market ? market->snapshot() : json(nullptr)},
        {"ssh_key", ssh_key},
        {"agent_program", agent_program},
        {"firewall_plan", firewall_plan},
        {"nodes", node_status},
        {"connection", paper ? paper->connection() : json(nullptr)},
        {"protocol", 1},
        {"product", "Asterion Terminal"},
        {"core", "C++20"},
        {"phase", "ready"},
        {"asset", "futures"},
        {"paper", account},
        {"dataset", preview->dataset},
        {"diagnostics",
         {{"succeeded", metrics.succeeded},
          {"failed", metrics.failed},
          {"trading_process_id", paper ? json(paper->process_id()) : json(nullptr)}}},
        {"plugins",
         json::array(
             {{{"id", "asterion.data.csv"}, {"kind", "data"}, {"state", "available"}},
              {{"id", "asterion.execution.paper"}, {"kind", "execution"}, {"state", "available"}},
              {{"id", "asterion.storage.filesystem-journal"},
               {"kind", "storage"},
               {"state", "available"}}})},
        {"live_market", "not_connected"},
        {"execution", "paper_only"}};
  }
  json dispatch(const json& request) {
    fields(request, {"version", "method", "params"});
    if (request.at("version") != 1 || !request.at("version").is_number_integer())
      throw std::invalid_argument("unsupported API version");
    const auto method = text(request, "method");
    const auto& params = request.at("params");
    std::unique_lock operation(operations, std::defer_lock);
    if (method == "runtime.snapshot") {
      if (!operation.try_lock()) {
        {
          std::lock_guard cached(cache_mutex);
          if (!cache.is_null()) {
            auto result = cache;
            result["stale"] = true;
            return result;
          }
        }
        operation.lock();
      }
    } else
      operation.lock();
    auto result = core.dispatch("terminal.local", method, params);
    if (result.is_object() && result.contains("protocol")) {
      auto next = result;
      // One-shot payloads belong to the requesting call, not to later polls.
      next.erase("initializer");
      std::lock_guard cached(cache_mutex);
      cache = std::move(next);
    }
    return result;
  }
  std::mutex operations, cache_mutex;
  json cache = nullptr;
  json inspect(const json& params) {
    fields(params, {"path", "venue", "symbol", "product", "delivery_month", "currency",
                    "price_increment", "quantity_increment", "multiplier"});
    FuturesContract contract{{{text(params, "venue"), text(params, "symbol")},
                              AssetClass::futures,
                              text(params, "currency"),
                              Decimal::parse(text(params, "price_increment")),
                              Decimal::parse(text(params, "quantity_increment")),
                              Decimal::parse(text(params, "multiplier"))},
                             text(params, "product"),
                             text(params, "delivery_month")};
    contract.validate();
    const auto utf8_path = text(params, "path");
    const auto path = std::filesystem::path(std::u8string(utf8_path.begin(), utf8_path.end()));
    if (!path.is_absolute() || !std::filesystem::is_regular_file(path))
      throw std::invalid_argument("choose an existing local CSV file");
    if (std::filesystem::file_size(path) >
        core.configuration().at("preview.max_bytes").get<std::uintmax_t>())
      throw std::invalid_argument("preview file must not exceed 32 MiB");
    const auto limit = core.configuration().at("preview.max_bytes").get<std::size_t>();
    std::ifstream file(path, std::ios::binary);
    std::string bytes(limit + 1, '\0');
    file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    const auto size = static_cast<std::size_t>(file.gcount());
    if (file.bad() || !file.eof() || size > limit)
      throw std::invalid_argument("CSV snapshot cannot be read within the preview limit");
    bytes.resize(size);
    PluginManager host;
    auto source = std::make_unique<CsvMarketData>(contract.instrument, bytes);
    auto* input = source.get();
    host.add(std::move(source));
    host.start();
    std::deque<json> preview;
    json replay = json::array();
    std::size_t count = 0;
    std::string first;
    Decimal quantity;
    while (auto tick = input->next()) {
      if (++count > core.configuration().at("preview.max_rows").get<std::size_t>())
        throw std::invalid_argument("preview reads at most 250000 trades");
      if (count == 1)
        first = std::to_string(tick->timestamp_ns);
      quantity = quantity + tick->quantity;
      preview.push_back({{"timestamp_ns", std::to_string(tick->timestamp_ns)},
                         {"price", tick->price.str()},
                         {"quantity", tick->quantity.str()}});
      if (count <= 10000)
        replay.push_back(preview.back());
      if (preview.size() > 240)
        preview.pop_front();
    }
    // Only publish the new preview after the complete file passes validation.
    const auto filename = path.filename().u8string();
    json next = {
        {"filename", std::string(filename.begin(), filename.end())},
        {"venue", contract.instrument.id.venue},
        {"symbol", contract.instrument.id.symbol},
        {"product", contract.product},
        {"delivery_month", contract.delivery_month},
        {"currency", contract.instrument.quote_currency},
        {"multiplier", contract.instrument.multiplier.str()},
        {"price_increment", contract.instrument.price_increment.str()},
        {"quantity_increment", contract.instrument.quantity_increment.str()},
        {"count", count},
        {"quantity", quantity.str()},
        {"first_timestamp_ns", count ? json(first) : json(nullptr)},
        {"last_timestamp_ns", count ? preview.back().at("timestamp_ns") : json(nullptr)},
        {"last_price", count ? preview.back().at("price") : json(nullptr)},
        {"ticks", preview},
        {"source", "local_csv"},
        {"specification_source", "user_supplied"},
        {"persistent", false},
        {"publication_ready", count > 0 && count <= 10000 && bytes.size() <= 4 * 1024 * 1024}};
    auto state = core.resources().resolve<PreviewState>("terminal", "preview").lock();
    if (count > 10000)
      replay = json::array();
    std::optional<data::v1::CsvSnapshot> captured;
    if (next.at("publication_ready").get<bool>()) {
      json spec = json::object();
      for (auto key : {"venue", "symbol", "currency", "price_increment", "quantity_increment",
                       "multiplier", "product", "delivery_month"})
        spec[key] = next.at(key);
      captured = protocol::encode_csv_snapshot({{"version", 1},
                                                {"source_name", next.at("filename")},
                                                {"source_sha256", sha256_bytes(bytes)},
                                                {"contract", spec},
                                                {"contents", bytes}});
    }
    state->dataset = std::move(next);
    state->replay = std::move(replay);
    state->source = std::move(captured);
    return snapshot();
  }
};
Application::Application() : impl_(std::make_unique<Impl>()) {}
Application::~Application() = default;
json Application::dispatch(const json& request) {
  return impl_->dispatch(request);
}
} // namespace asterion::terminal
