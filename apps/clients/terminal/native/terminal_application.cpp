#include "application_impl.hpp"
#include <array>
#include <stdexcept>

namespace asterion::terminal {
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
namespace {
constexpr std::array<const char*, 8> cost_keys{
    "margin_per_lot", "open_fee",      "close_today_fee",      "close_yesterday_fee",
    "margin_rate",    "open_fee_rate", "close_today_fee_rate", "close_yesterday_fee_rate"};
} // namespace
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
std::string next_runtime_scope() {
  // Distinct runtime scope per Application instance within this process.
  static IdSequence scopes{"terminal"};
  return scopes.next();
}
Application::Impl::Impl() {
  core.access().grant("terminal.local", "runtime.read");
  // Only reached before the first publication; later reads never lock.
  core.command("runtime.snapshot", "runtime.read", [this](const json&) { return snapshot(); });
  register_paper_commands();
  register_node_commands();
  register_research_commands();
  register_connection_commands();
  register_strategy_commands();
  register_market_commands();
  core.start();
  refresher = std::jthread([this](std::stop_token stop) { refresh_loop(stop); });
}
Application::Impl::~Impl() {
  refresher.request_stop();
  refresh_wake.notify_all();
  refresher = {};
}
Application::Impl::Parts Application::Impl::gather_parts(bool hold_between_calls) {
  // With hold_between_calls the caller already owns `operations`. Otherwise
  // each client call takes it briefly, so a command waits for at most one RPC.
  Parts parts;
  auto step = [&](auto&& read) {
    if (hold_between_calls) {
      read();
      return;
    }
    std::lock_guard lock(operations);
    read();
  };
  step([&] {
    parts.paper = paper ? paper->snapshot() : json(nullptr);
    parts.connection = paper ? paper->connection() : json(nullptr);
    parts.process = paper ? json(paper->process_id()) : json(nullptr);
  });
  step([&] { parts.research = research ? research->status() : json(nullptr); });
  step([&] { parts.strategy = strategy ? strategy->status() : json(nullptr); });
  step([&] { parts.market = market ? market->snapshot() : json(nullptr); });
  step([&] {
    for (const auto& [id, node] : nodes) {
      (void)id;
      parts.nodes.push_back(node->status());
    }
  });
  return parts;
}
json Application::Impl::compose(const Parts& parts) {
  const auto metrics = core.observations().metrics();
  json catalog = json::array();
  for (const auto& item : history_contracts)
    catalog.push_back(
        {{"code", item.identity.key()},
         {"name", item.name},
         {"list_date", item.list_date},
         {"delist_date", item.delist_date},
         {"multiplier", item.multiplier ? json(item.multiplier->str()) : json(nullptr)},
         {"per_unit", item.per_unit ? json(item.per_unit->str()) : json(nullptr)},
         {"trade_unit", item.trade_unit ? json(*item.trade_unit) : json(nullptr)},
         {"quote_unit", item.quote_unit ? json(*item.quote_unit) : json(nullptr)}});
  return {{"history_contracts",
           {{"source", history_source},
            {"connection", history_connection},
            {"connection_revision", history_connection_revision},
            {"exchange", history_exchange},
            {"product", history_product},
            {"cutoff_ns", std::to_string(history_cutoff)},
            {"items", catalog}}},
          {"research", parts.research},
          {"data_connections", data_connections.snapshot()},
          {"connection_verification", connection_verification},
          {"research_result", research_result},
          {"history_page", nullptr},
          {"daily_page", nullptr},
          {"strategy", parts.strategy},
          {"market", parts.market},
          {"ssh_key", ssh_key},
          {"agent_program", agent_program},
          {"firewall_plan", firewall_plan},
          {"nodes", parts.nodes},
          {"connection", parts.connection},
          {"protocol", 1},
          {"product", "Asterion Terminal"},
          {"core", "C++20"},
          {"phase", "ready"},
          {"asset", "futures"},
          {"paper", parts.paper},
          {"datasets",
           [&] {
             json summaries = json::array();
             for (const auto& item : selections)
               summaries.push_back(item.summary);
             return summaries;
           }()},
          {"diagnostics",
           {{"succeeded", metrics.succeeded},
            {"failed", metrics.failed},
            {"trading_process_id", parts.process}}},
          {"native_plugins", native_plugins},
          {"plugins",
           json::array(
               {{{"id", "asterion.data.ctp"}, {"kind", "data"}, {"state", "available"}},
                {{"id", "asterion.execution.paper"}, {"kind", "execution"}, {"state", "available"}},
                {{"id", "asterion.storage.filesystem-journal"},
                 {"kind", "storage"},
                 {"state", "available"}}})},
          {"live_market", "not_connected"},
          {"execution", "paper_only"}};
}
json Application::Impl::snapshot() {
  return compose(gather_parts(true));
}
void Application::Impl::publish(json next) {
  // One-shot payloads belong to the requesting call, not to later reads.
  next.erase("initializer");
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  std::lock_guard lock(cache_mutex);
  if (next != cache) {
    cache = std::move(next);
    ++revision;
  }
  refreshed_at_ms = now;
}
// A poll that states the market revisions it holds receives only quote rows
// changed after them; an unchanged catalog is sent without its contracts.
// Any mismatch in subscription set sends every row.
void Application::Impl::trim_market(json& result, const json& params) {
  if (!params.contains("market_rows") || !result.contains("market") ||
      !result.at("market").is_object())
    return;
  auto& market = result["market"];
  if (market.value("subscription_set", std::uint64_t{0}) ==
      params.at("market_set").get<std::uint64_t>()) {
    const auto held = params.at("market_rows").get<std::uint64_t>();
    json changed = json::array();
    for (const auto& row : market.at("subscriptions"))
      if (row.value("revision", std::uint64_t{0}) > held)
        changed.push_back(row);
    market["subscriptions"] = std::move(changed);
    market["delta"] = true;
  }
  auto& catalog = market["catalog"];
  if (catalog.value("revision", std::uint64_t{0}) == params.at("catalog").get<std::uint64_t>()) {
    catalog["contracts"] = json::array();
    catalog["omitted"] = true;
  }
}
json Application::Impl::read_published(const json& params) {
  std::optional<std::uint64_t> since;
  if (params.contains("since"))
    since = params.at("since").get<std::uint64_t>();
  core.access().require("terminal.local", "runtime.read");
  std::lock_guard lock(cache_mutex);
  if (cache.is_null())
    return nullptr;
  json result = since == revision ? json{{"unchanged", true}} : cache;
  if (since != revision)
    trim_market(result, params);
  result["revision"] = revision;
  result["refreshed_at_ms"] = refreshed_at_ms;
  return result;
}
void Application::Impl::refresh_loop(std::stop_token stop) {
  while (!stop.stop_requested()) {
    bool live = false;
    try {
      std::uint64_t before = 0;
      {
        std::lock_guard lock(operations);
        before = mutations;
      }
      auto parts = gather_parts(false);
      live = !parts.market.is_null();
      {
        std::lock_guard lock(operations);
        // Check and publish under the same lock as commands. Releasing it
        // between these steps would let an old refresh overwrite a new command.
        if (mutations == before)
          publish(compose(parts));
      }
    } catch (const std::exception&) {
      // Client failures surface through their own status fields next cycle.
    }
    std::unique_lock lock(cache_mutex);
    refresh_wake.wait_for(lock, stop,
                          live ? std::chrono::milliseconds(500) : std::chrono::seconds(2),
                          [] { return false; });
  }
}
json Application::Impl::dispatch(const json& request) {
  fields(request, {"version", "method", "params"});
  if (request.at("version") != 1 || !request.at("version").is_number_integer())
    throw std::invalid_argument("unsupported API version");
  const auto method = text(request, "method");
  const bool market_query = method == "market.minutes";
  const bool history_query = market_query || method == "research.minutes.page" ||
                             method == "research.daily.page" || method == "research.datasets" ||
                             method == "research.coverage";
  const bool research_io = method == "research.daily-factor.submit" || method == "research.result";
  const auto& params = request.at("params");
  // Polls pass `since` and always read the published snapshot. A plain read is
  // an explicit probe: fresh, except while a command runs, when it returns the
  // published snapshot marked stale. A probe may wait for one background
  // refresher step (a single client call) but never behind a command.
  // Provider I/O happens before taking the client-operation lock. Other windows
  // can continue fresh status reads and service actions during catalog lookup.
  std::optional<std::vector<HistoryListing>> catalog;
  std::shared_ptr<ResearchClient> catalog_client;
  if (method == "research.contracts.load") {
    fields(params, {"source", "exchange", "product", "token", "connection", "connection_revision"});
    core.access().require("terminal.local", "node.manage");
    if (text(params, "connection", true).empty() &&
        !text(params, "connection_revision", true).empty())
      throw std::invalid_argument("invalid data connection identity");
    auto& client = catalog_client;
    auto credential = text(params, "token", true);
    {
      std::lock_guard lock(operations);
      client = research;
      if (params.contains("connection") && !text(params, "connection", true).empty()) {
        if (!credential.empty())
          throw std::invalid_argument("choose a saved connection or a temporary credential");
        credential =
            resolve_data_connection(text(params, "connection"), text(params, "connection_revision"),
                                    text(params, "source"))
                .credential;
      }
    }
    if (!client)
      throw std::invalid_argument("connect research service first");
    catalog = client->catalog(text(params, "source"), credential, text(params, "exchange"),
                              text(params, "product"));
  }
  std::optional<data::v1::HistoryConnectionVerification> verification;
  std::shared_ptr<ResearchClient> verifying_client;
  if (method == "research.connections.verify") {
    fields(params, {"id", "revision", "source"});
    core.access().require("terminal.local", "node.manage");
    DataConnection connection;
    {
      std::lock_guard lock(operations);
      connection = resolve_data_connection(text(params, "id"), text(params, "revision"),
                                           text(params, "source"));
      verifying_client = research;
    }
    verification = verifying_client->verify_connection(connection.source, connection.credential);
  }
  std::unique_lock operation(operations, std::defer_lock);
  if (method == "runtime.snapshot") {
    // Validate before choosing a fresh or cached read. Invalid input must not
    // become accepted merely because no command currently owns the lock.
    const bool incremental = params.is_object() && params.contains("market_rows");
    if (!params.is_object() ||
        (incremental ? params.size() != 4 || !params.contains("since") ||
                           !params.contains("market_set") || !params.contains("catalog")
                     : params.size() > 1 || (params.size() == 1 && !params.contains("since"))))
      throw std::invalid_argument("request fields do not match the current contract");
    const bool poll = params.contains("since");
    for (const auto* name : {"since", "market_rows", "market_set", "catalog"})
      if (params.contains(name) && !params.at(name).is_number_unsigned())
        throw std::invalid_argument("snapshot revisions must be unsigned integers");
    auto published_read = [&]() -> json {
      auto published = read_published(params);
      if (!published.is_null() && !poll)
        published["stale"] = true;
      return published;
    };
    if (poll)
      if (auto published = published_read(); !published.is_null())
        return published;
    while (!operation.try_lock()) {
      if (command_running.load())
        if (auto published = published_read(); !published.is_null())
          return published;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  } else {
    while (!operation.try_lock()) {
      if (command_running.load())
        throw Error(ErrorCode::conflict,
                    "another Terminal operation is in progress; retry after it completes");
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!history_query)
      ++mutations;
    command_running = !history_query;
  }
  struct Clear {
    std::atomic<bool>& flag;
    bool active;
    ~Clear() {
      if (active)
        flag = false;
    }
  } clear{command_running, method != "runtime.snapshot" && !history_query};
  if (verification) {
    if (research != verifying_client)
      throw Error(ErrorCode::conflict, "research connection changed; retry verification");
    (void)resolve_data_connection(text(params, "id"), text(params, "revision"),
                                  text(params, "source"));
    json checks = json::array();
    for (const auto& check : verification->checks())
      checks.push_back({{"scope", check.scope()}, {"state", check.state()}});
    connection_verification = {
        {"id", text(params, "id")}, {"revision", text(params, "revision")}, {"checks", checks}};
  }
  if (catalog) {
    if (research != catalog_client)
      throw Error(ErrorCode::conflict, "research connection changed; reload catalog");
    if (!text(params, "connection", true).empty())
      (void)resolve_data_connection(text(params, "connection"), text(params, "connection_revision"),
                                    text(params, "source"));
    history_connection = text(params, "connection", true);
    history_connection_revision = text(params, "connection_revision", true);
    history_source = text(params, "source");
    history_contracts = std::move(*catalog);
    history_exchange = text(params, "exchange");
    history_product = text(params, "product");
    history_cutoff = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
  }
  struct Active {
    std::unique_lock<std::mutex>*& slot;
    ~Active() { slot = nullptr; }
  } active{operation_lock};
  operation_lock = &operation;
  auto result = core.dispatch("terminal.local", method,
                              method == "runtime.snapshot" ? json::object() : params);
  if (!history_query && !research_io && result.is_object() && result.contains("protocol")) {
    publish(result);
    std::lock_guard cached(cache_mutex);
    result["revision"] = revision;
    result["refreshed_at_ms"] = refreshed_at_ms;
  }
  if (research_io) {
    auto client = research;
    const auto generation = research_generation.load();
    auto definition = params;
    definition.erase("id");
    const auto input = method == "research.daily-factor.submit"
                           ? protocol::encode_daily_factor_request(definition)
                           : research::v1::DailyFactorRequest{};
    clear.active = false;
    command_running = false;
    operation.unlock();
    json evidence;
    if (method == "research.daily-factor.submit")
      client->submit(text(params, "id"), input);
    else
      evidence = client->result(text(params, "id"));
    operation.lock();
    if (generation != research_generation.load())
      throw Error(ErrorCode::conflict, "research service changed during operation; inspect the "
                                       "original service before retrying");
    if (method == "research.result")
      research_result = std::move(evidence);
    result = snapshot();
    publish(result);
    std::lock_guard cached(cache_mutex);
    result["revision"] = revision;
    result["refreshed_at_ms"] = refreshed_at_ms;
  }
  if (method == "research.datasets" || method == "research.coverage") {
    auto reader = research;
    const auto generation = research_generation.load();
    data::v1::HistoryFilter filter;
    filter.set_venue(params.value("venue", ""));
    filter.set_product(params.value("product", ""));
    filter.set_contract_id(params.value("contract_id", ""));
    filter.set_source(params.value("source", ""));
    clear.active = false;
    command_running = false;
    operation.unlock();
    auto rows = method == "research.coverage" ? reader->coverage(filter) : reader->datasets(filter);
    if (generation != research_generation.load())
      throw Error(ErrorCode::conflict, "research service changed during archive query");
    result[method == "research.coverage" ? "history_coverage" : "history_datasets"] =
        std::move(rows);
  } else if (market_query) {
    auto reader = market;
    clear.active = false;
    command_running = false;
    operation.unlock();
    result["intraday"] = reader->minutes(text(params, "venue"), text(params, "symbol"));
  } else if (history_query) {
    // Keep the selected client alive, but let unrelated commands and chart readers proceed.
    // Runtime dispatch itself stays serialized; only immutable service I/O runs outside it.
    auto reader = research;
    const auto generation = research_generation.load();
    const bool daily = method == "research.daily.page";
    const auto minute_query = daily ? data::v1::MinutePageQuery{} : minute_page_query(params);
    const auto daily_query = daily ? daily_page_query(params) : data::v1::DailyPageQuery{};
    clear.active = false;
    command_running = false;
    operation.unlock();
    auto page = daily ? reader->daily_page(daily_query) : reader->minute_page(minute_query);
    if (generation != research_generation.load())
      throw Error(ErrorCode::conflict, daily ? "research service changed during daily query"
                                             : "research service changed during minute query");
    result[daily ? "daily_page" : "history_page"] = std::move(page);
  }
  return result;
}
const std::vector<DatasetSelection>& Application::Impl::selected() const {
  if (selections.empty())
    throw std::invalid_argument("select downloaded data and a contract specification first");
  return selections;
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
    if (found->size() != cost_keys.size() + 2)
      throw std::invalid_argument("request fields do not match the current contract");
    json costs = json::object();
    for (const auto* key : cost_keys)
      costs[key] = text(*found, key);
    result.push_back(std::move(costs));
  }
  return result;
}
Application::Application() : impl_(std::make_unique<Impl>()) {}
Application::~Application() = default;
json Application::dispatch(const json& request) {
  return impl_->dispatch(request);
}
} // namespace asterion::terminal
