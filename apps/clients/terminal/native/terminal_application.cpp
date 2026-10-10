#include "application_impl.hpp"
#include <array>
#include <stdexcept>
#include <ostream>
#include <streambuf>

namespace asterion::terminal {
namespace {
// The single response worker bounds its temporary encoding before admission
// to the retained-reply budget. No complete, unbounded dump is built first.
class ResponseBuffer final : public std::streambuf {
public:
  std::string bytes;

private:
  static constexpr std::size_t limit = 128 * 1024 * 1024;
  std::streamsize xsputn(const char* text, std::streamsize size) override {
    if (static_cast<std::size_t>(size) > limit - bytes.size())
      throw Error(ErrorCode::resource_exhausted,
                  "Native response exceeds 128 MiB; command outcome may be unknown");
    bytes.append(text, static_cast<std::size_t>(size));
    return size;
  }
  int_type overflow(int_type value) override {
    if (traits_type::eq_int_type(value, traits_type::eof()))
      return traits_type::not_eof(value);
    const auto character = traits_type::to_char_type(value);
    xsputn(&character, 1);
    return value;
  }
};
Payload encode_response(const json& envelope, const PayloadBudget& budget) {
  ResponseBuffer buffer;
  std::ostream stream(&buffer);
  stream.exceptions(std::ios::badbit | std::ios::failbit);
  stream << envelope;
  try {
    return budget.retain(std::move(buffer.bytes));
  } catch (const Error& error) {
    throw Error(error.code(), "Native response capacity reached; command outcome may be unknown");
  }
}
} // namespace

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
void fields_with_risk(const json& object, std::initializer_list<std::string_view> names) {
  if (!object.is_object() || object.size() != names.size() + 3)
    throw std::invalid_argument("request fields do not match the current contract");
  for (const auto name : names)
    if (!object.contains(name))
      throw std::invalid_argument("request is missing a required field");
  for (const auto* key : {"max_order_quantity", "max_gross_quantity", "max_working_orders"})
    if (!object.contains(key))
      throw std::invalid_argument("request is missing a required field");
}
std::string text(const json& object, const char* name, bool allow_empty) {
  const auto& value = object.at(name);
  if (!value.is_string())
    throw std::invalid_argument("text field has the wrong type");
  auto result = value.get<std::string>();
  if ((!allow_empty && result.empty()) || result.find('\0') != std::string::npos)
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
void Application::Impl::command(std::string name, Command handler) {
  if (!commands.emplace(std::move(name), std::move(handler)).second)
    throw std::logic_error("duplicate Terminal command registration");
}
PolledTask<Application::Impl::Response> Application::Impl::invoke(const std::string& method,
                                                                  const json& params) {
  const auto found = commands.find(method);
  const auto operation = found == commands.end() ? "terminal.unknown" : method;
  const auto trace = std::string(current_trace_id());
  const SystemClock clock;
  const auto start = clock.monotonic_now();
  auto finish = [&](bool success) noexcept {
    ++(success ? succeeded : failed);
    // Telemetry allocation or sink failure must not change a business result.
    try {
      logger->write(success ? (method == "runtime.snapshot" ? LogLevel::debug : LogLevel::info)
                            : LogLevel::warning,
                    operation,
                    {{"trace_id", trace},
                     {"duration_ns", clock.monotonic_now() - start},
                     {"success", success}});
    } catch (...) {
    }
  };
  try {
    if (found == commands.end())
      throw Error(ErrorCode::invalid_request, "unsupported command");
    // Selection and credentials must remain bound to the same market operation
    // while preparation suspends. Read-only minutes do not acquire this gate.
    std::optional<ResetFlag> market_admission;
    if ((method.starts_with("market.") && method != "market.minutes") ||
        method == "ctp.connections.market") {
      if (market_busy)
        throw Error(ErrorCode::conflict,
                    "another market operation is in progress; retry after it completes");
      market_busy = true;
      market_admission.emplace(market_busy);
    }
    auto result = co_await found->second(params);
    finish(true);
    co_return result;
  } catch (...) {
    finish(false);
    throw;
  }
}
Application::Impl::Impl() {
  // Registration precedes all accepted commands.
  command("runtime.snapshot",
          [this](const json&) -> PolledTask<Response> { co_return snapshot(); });
  register_live_commands();
  register_node_commands();
  register_data_commands();
  register_task_commands();
  register_backtest_commands();
  register_factor_commands();
  register_data_task_connection_commands();
  register_connection_commands();
  register_market_commands();
  logger->write(LogLevel::info, "terminal.started");
  initialization =
      service_io.submit<void>([this](std::stop_token) { return settings<void>([] {}); }).share();
  refresher = service_io.submit<void>([this](std::stop_token stop) { return refresh_loop(stop); },
                                      ServiceIo::Lane::observation);
  settings_refresher = service_io.submit<void>(
      [this](std::stop_token stop) { return settings_loop(stop); }, ServiceIo::Lane::observation);
}
Application::Impl::~Impl() {
  lifetime.request_stop();
  initialization.wait();
  refresher.get();
  settings_refresher.get();
  logger->write(LogLevel::info, "terminal.stopped");
  logger->flush();
}
Application::Impl::Publication Application::Impl::capture() {
  Publication next;
  next.history = history_contracts;
  next.task_result = task_result;
  next.plugins = native_plugins;
  next.settings = settings_view;
  for (const auto& [id, account] : live)
    next.live.emplace(id, account->client->owner_view());
  if (task_client)
    next.tasks = task_client->owner_view();
  const auto data = data_client ? data_client->owner_status() : json(nullptr);
  if (market)
    next.market = market->owner_read();
  for (const auto& [id, node] : nodes) {
    (void)id;
    next.nodes.push_back(node->owner_view());
  }
  next.metadata = {
      {"history_contracts",
       {{"source", history_source},
        {"exchange", history_exchange},
        {"product", history_product},
        {"cutoff_ns", std::to_string(history_cutoff)},
        {"items", json::array()}}},
      {"task_service", nullptr},
      {"data", data},
      {"credential_verification", credential_verification},
      {"history_page", nullptr},
      {"daily_page", nullptr},
      {"ssh_key", ssh_key},
      {"agent_program", agent_program},
      {"firewall_plan", firewall_plan},
      {"protocol", 1},
      {"product", "Asterion Terminal"},
      {"core", "C++20"},
      {"phase", "ready"},
      {"asset", "futures"},
      // How each exchange assigns closes to today's and yesterday's
      // positions; the order forms follow the core's rule.
      {"close_policies",
       [] {
         json policies = json::object();
         for (const auto* venue : {"SHFE", "INE", "CFFEX", "DCE", "CZCE", "GFEX"})
           switch (close_policy(venue)) {
           case ClosePolicy::explicit_buckets:
             policies[venue] = "explicit_buckets";
             break;
           case ClosePolicy::today_first:
             policies[venue] = "today_first";
             break;
           case ClosePolicy::yesterday_first:
             policies[venue] = "yesterday_first";
             break;
           }
         return policies;
       }()},
      {"datasets",
       [&] {
         json summaries = json::array();
         for (const auto& item : selections)
           summaries.push_back(item.summary);
         return summaries;
       }()},
      {"dataset_series",
       [&] {
         json summaries = json::array();
         for (const auto& item : dataset_series)
           summaries.push_back(item.summary);
         return summaries;
       }()},
      {"diagnostics",
       {{"succeeded", succeeded},
        {"failed", failed},
        {"log_failures", process_log_failures()},
        {"refresh_failures", refresh_failures},
        {"refresh_failed", refresh_failed}}},
      {"plugins",
       json::array(
           {{{"id", "asterion.data.ctp"}, {"kind", "data"}, {"state", "available"}},
            {{"id", "asterion.execution.ctp"}, {"kind", "execution"}, {"state", "available"}},
            {{"id", "asterion.storage.sqlite-journal"},
             {"kind", "storage"},
             {"state", "available"}}})}};
  return next;
}

bool Application::Impl::Publication::same_state(const Publication& other) const {
  return metadata == other.metadata && live == other.live && tasks == other.tasks &&
         nodes == other.nodes && settings == other.settings && history == other.history &&
         task_result == other.task_result && plugins == other.plugins &&
         market.header == other.market.header && market.catalog == other.market.catalog &&
         market.rows == other.market.rows;
}
json Application::Impl::Publication::render(std::optional<MarketCursor> held) const {
  auto result = metadata;
  auto& nodes_view = result["nodes"] = json::array();
  for (const auto& node : nodes)
    nodes_view.push_back(node_snapshot_json(node));
  result["data_credentials"] = settings->credentials;
  result["ctp_connections"] = settings->accounts;
  result["ctp_market"] = settings->market;
  auto& catalog = result.at("history_contracts").at("items");
  for (const auto& item : *history)
    catalog.push_back(
        {{"code", item.identity.key()},
         {"name", item.name},
         {"list_date", item.list_date},
         {"delist_date", item.delist_date},
         {"multiplier", item.multiplier ? json(item.multiplier->str()) : json(nullptr)},
         {"per_unit", item.per_unit ? json(item.per_unit->str()) : json(nullptr)},
         {"trade_unit", item.trade_unit ? json(*item.trade_unit) : json(nullptr)},
         {"quote_unit", item.quote_unit ? json(*item.quote_unit) : json(nullptr)}});
  auto& accounts = result["live"] = json::object();
  for (const auto& [id, account] : live) {
    accounts[id] = account.render();
    auto& connection = accounts[id].at("connection");
    for (const auto& node : nodes_view) {
      if (connection.at("transport") != "local" || node.at("id") != "local" ||
          node.at("health").is_null())
        continue;
      for (const auto& service : node.at("health").at("services"))
        if (service.at("id") == connection.at("session"))
          connection["restarts"] = service.at("restarts");
    }
  }
  result["market"] = market.render(held);
  result["task_service"] = tasks ? tasks->render() : json(nullptr);
  result["task_result"] = task_result ? *task_result : json(nullptr);
  result["native_plugins"] = plugins ? *plugins : json(nullptr);
  result["revision"] = revision;
  result["refreshed_at_ms"] = refreshed_at_ms;
  return result;
}
json Application::Impl::Response::render() && {
  if (!publication)
    return std::move(values);
  auto result = publication->render(cursor);
  for (auto& [key, value] : values.items())
    result[key] = std::move(value);
  return result;
}
void Application::Impl::publish() {
  auto next = capture();
  next.revision = cache ? cache->revision + !next.same_state(*cache) : 1;
  next.refreshed_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
  cache = std::make_shared<const Publication>(std::move(next));
}
Application::Impl::Response Application::Impl::snapshot() {
  publish();
  return read_published(json::object());
}
Application::Impl::Response Application::Impl::read_published(const json& params) {
  if (!cache)
    publish();
  const auto published = cache;
  if (params.contains("since") && params.at("since") == published->revision)
    return json{{"unchanged", true},
                {"revision", published->revision},
                {"refreshed_at_ms", published->refreshed_at_ms}};
  std::optional<MarketCursor> held;
  if (params.contains("market_rows"))
    held = MarketCursor{params.at("market_set").get<std::uint64_t>(),
                        params.at("market_rows").get<std::uint64_t>(),
                        params.at("catalog").get<std::uint64_t>()};
  return Response{published, held};
}
void Application::Impl::record_refresh_failure(ErrorCode code) noexcept {
  refresh_failed = true;
  const auto count = ++refresh_failures;
  log_process_failure("terminal", "snapshot.refresh_failed", code, count);
  try {
    if (cache) {
      auto next = std::make_shared<Publication>(*cache);
      auto& diagnostics = next->metadata.at("diagnostics");
      diagnostics["refresh_failed"] = true;
      diagnostics["refresh_failures"] = count;
      diagnostics["log_failures"] = process_log_failures();
      ++next->revision;
      cache = std::move(next);
    }
  } catch (...) {
    // Status allocation failure must not stop the I/O owner.
  }
}
PolledTask<void> Application::Impl::initialized() {
  co_await PollUntil{[this] {
    return initialization.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
  }};
  initialization.get();
}
PolledTask<void> Application::Impl::refresh_loop(std::stop_token stop) {
  try {
    co_await initialized();
  } catch (const std::exception& error) {
    record_refresh_failure(classify(error));
    co_return;
  } catch (...) {
    record_refresh_failure(ErrorCode::internal_error);
    co_return;
  }
  while (!stop.stop_requested() && !lifetime.stop_requested()) {
    try {
      refresh_failed = settings_failed;
      publish();
    } catch (const std::exception& error) {
      record_refresh_failure(classify(error));
    } catch (...) {
      record_refresh_failure(ErrorCode::internal_error);
    }
    const auto next = std::chrono::steady_clock::now() +
                      (market ? std::chrono::milliseconds(500) : std::chrono::milliseconds(2000));
    co_await PollUntil{[&] {
      return stop.stop_requested() || lifetime.stop_requested() ||
             std::chrono::steady_clock::now() >= next;
    }};
  }
}
PolledTask<void> Application::Impl::settings_loop(std::stop_token stop) {
  try {
    co_await initialized();
  } catch (const std::exception& error) {
    record_refresh_failure(classify(error));
    co_return;
  } catch (...) {
    record_refresh_failure(ErrorCode::internal_error);
    co_return;
  }
  while (!stop.stop_requested() && !lifetime.stop_requested()) {
    const auto next = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    co_await PollUntil{[&] {
      return stop.stop_requested() || lifetime.stop_requested() ||
             std::chrono::steady_clock::now() >= next;
    }};
    if (stop.stop_requested() || lifetime.stop_requested())
      co_return;
    if (settings_busy)
      continue;
    try {
      co_await settings<void>([] {});
      settings_failed = false;
    } catch (const std::exception& error) {
      settings_failed = true;
      record_refresh_failure(classify(error));
    } catch (...) {
      settings_failed = true;
      record_refresh_failure(ErrorCode::internal_error);
    }
  }
}
PolledTask<Application::Impl::Response> Application::Impl::dispatch(const json& request) {
  fields(request, {"version", "method", "params"});
  if (request.at("version") != 1 || !request.at("version").is_number_integer())
    throw std::invalid_argument("unsupported API version");
  const auto method = text(request, "method");
  const auto& params = request.at("params");
  if (method == "runtime.snapshot") {
    const bool incremental = params.is_object() && params.contains("market_rows");
    if (!params.is_object() ||
        (incremental ? params.size() != 4 || !params.contains("since") ||
                           !params.contains("market_set") || !params.contains("catalog")
                     : params.size() > 1 || (params.size() == 1 && !params.contains("since"))))
      throw std::invalid_argument("request fields do not match the current contract");
    for (const auto* name : {"since", "market_rows", "market_set", "catalog"})
      if (params.contains(name) && !params.at(name).is_number_unsigned())
        throw std::invalid_argument("snapshot revisions must be unsigned integers");
    if (params.contains("since") && cache)
      co_return read_published(params);
  }
  co_await initialized();
  co_return co_await invoke(method, method == "runtime.snapshot" ? json::object() : params);
}

const std::vector<DatasetSelection>& Application::Impl::selected() const {
  if (selections.empty())
    throw std::invalid_argument("select downloaded data and a contract specification first");
  return selections;
}
std::optional<std::size_t>
Application::Impl::series_of(const protocol::v1::Contract& contract) const {
  for (std::size_t i = 0; i < dataset_series.size(); ++i)
    if (dataset_series[i].summary.at("venue") == contract.venue() &&
        dataset_series[i].summary.at("product") == contract.product())
      return i;
  return std::nullopt;
}
std::vector<json> Application::Impl::selection_costs(const json& contracts) const {
  const auto& chosen = selected();
  if (!contracts.is_array() || contracts.size() != chosen.size())
    throw std::invalid_argument("give costs for every selected contract");
  std::vector<json> result;
  for (const auto& item : chosen) {
    const auto& spec = item.dataset.contract();
    const auto found = std::ranges::find_if(contracts, [&](const json& entry) {
      return entry.is_object() && entry.value("venue", "") == spec.venue() &&
             entry.value("symbol", "") == spec.symbol();
    });
    if (found == contracts.end())
      throw std::invalid_argument("give costs for every selected contract");
    require_fields(*found, {"venue", "symbol", "cost_schedule", "slippage_ticks"});
    const auto schedule = protocol::encode_cost_schedule(found->at("cost_schedule"));
    (void)costs_on(protocol::cost_schedule(schedule), item.dataset.bars(0).trading_day());
    result.push_back({{"cost_schedule", protocol::decode_cost_schedule(schedule)},
                      {"slippage_ticks", found->at("slippage_ticks")}});
  }
  return result;
}
Application::Application() : impl_(std::make_unique<Impl>()) {}
Application::~Application() = default;
json Application::dispatch(const json& request) {
  const auto trace = (request.contains("method") && request.at("method") == "runtime.snapshot")
                         ? std::string{}
                         : impl_->request_ids.next();
  TraceScope context(trace);
  return impl_->service_io
      .submit<json>(
          [this, request](std::stop_token) -> PolledTask<json> {
            auto response = co_await impl_->dispatch(request);
            co_return co_await impl_->service_io.read<json>(
                [&] { return std::move(response).render(); }, ServiceIo::ReadLane::response);
          },
          ServiceIo::Lane::application)
      .get();
}
std::future<void> Application::request(std::string wire, Completion complete) {
  const auto trace = impl_->request_ids.next();
  TraceScope context(trace);
  return impl_->service_io.submit<void>(
      [this, wire = std::move(wire),
       complete = std::move(complete)](std::stop_token) -> PolledTask<void> {
        Payload encoded;
        std::exception_ptr failure;
        try {
          std::optional<Impl::Response> response;
          std::exception_ptr command_error;
          auto budget = impl_->command_payloads;
          try {
            if (wire.size() > 65536 || wire.find('\0') != std::string::npos)
              throw std::invalid_argument("invalid native API request");
            auto input = parse_json(wire);
            if (input.is_object() && input.value("method", json{}) == "runtime.snapshot")
              budget = impl_->snapshot_payloads;
            response.emplace(co_await impl_->dispatch(input));
          } catch (...) {
            command_error = std::current_exception();
          }
          co_await impl_->service_io.read<void>(
              [&] {
                json envelope = json::object();
                try {
                  if (command_error)
                    std::rethrow_exception(command_error);
                  envelope["result"] = std::move(*response).render();
                } catch (const std::exception& error) {
                  envelope["error"] = {{"code", error_name(classify(error))},
                                       {"message", error.what()}};
                } catch (...) {
                  envelope["error"] = {{"code", "internal_error"},
                                       {"message", "native core call failed"}};
                }
                encoded = encode_response(envelope, budget);
              },
              ServiceIo::ReadLane::response);
        } catch (...) {
          failure = std::current_exception();
        }
        complete(std::move(encoded), failure);
      },
      ServiceIo::Lane::application);
}
} // namespace asterion::terminal
