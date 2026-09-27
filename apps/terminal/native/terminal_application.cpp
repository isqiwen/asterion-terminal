#include "application_impl.hpp"

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
std::string next_runtime_scope() {
  // Distinct runtime scope per Application instance within this process.
  static IdSequence scopes{"terminal"};
  return scopes.next();
}
Application::Impl::Impl() {
  scope.publish("preview", std::make_shared<PreviewState>());
  auto positive_limit = [](const json& value) {
    return value.is_number_integer() && value > 0 && value <= 32 * 1024 * 1024;
  };
  core.configuration().declare("preview.max_bytes", 32 * 1024 * 1024, positive_limit);
  core.configuration().declare("preview.max_rows", 250000, positive_limit);
  core.access().grant("terminal.local", "runtime.read");
  core.access().grant("terminal.local", "data.inspect");
  // Only reached before the first publication; later reads never lock.
  core.command("runtime.snapshot", "runtime.read", [this](const json&) { return snapshot(); });
  core.command("futures.inspect_csv", "data.inspect",
               [this](const json& params) { return inspect(params); });
  register_paper_commands();
  register_node_commands();
  register_research_commands();
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
  const auto preview = core.resources().resolve<PreviewState>("terminal", "preview").lock();
  const auto metrics = core.observations().metrics();
  return {{"research", parts.research},
          {"research_result", research_result},
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
          {"dataset", preview->dataset},
          {"diagnostics",
           {{"succeeded", metrics.succeeded},
            {"failed", metrics.failed},
            {"trading_process_id", parts.process}}},
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
json Application::Impl::read_published(const json& params) {
  if (!params.is_object() || params.size() > 1 || (params.size() == 1 && !params.contains("since")))
    throw std::invalid_argument("request fields do not match the current contract");
  std::optional<std::uint64_t> since;
  if (params.contains("since")) {
    if (!params.at("since").is_number_unsigned())
      throw std::invalid_argument("since must be a snapshot revision");
    since = params.at("since").get<std::uint64_t>();
  }
  core.access().require("terminal.local", "runtime.read");
  std::lock_guard lock(cache_mutex);
  if (cache.is_null())
    return nullptr;
  json result = since == revision ? json{{"unchanged", true}} : cache;
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
      json next;
      {
        std::lock_guard lock(operations);
        // A command ran meanwhile and already published fresher state.
        if (mutations == before)
          next = compose(parts);
      }
      if (!next.is_null())
        publish(std::move(next));
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
  const auto& params = request.at("params");
  // Polls pass `since` and always read the published snapshot. A plain read is
  // an explicit probe: fresh when idle, otherwise the published snapshot
  // marked stale. Neither ever waits behind a command or a service RPC.
  std::unique_lock operation(operations, std::defer_lock);
  if (method == "runtime.snapshot") {
    const bool poll = params.is_object() && params.contains("since");
    if (poll || !operation.try_lock()) {
      if (auto published = read_published(params); !published.is_null()) {
        if (!poll)
          published["stale"] = true;
        return published;
      }
      if (!operation.owns_lock())
        operation.lock();
    }
  } else {
    operation.lock();
    ++mutations;
  }
  auto result = core.dispatch("terminal.local", method,
                              method == "runtime.snapshot" ? json::object() : params);
  if (result.is_object() && result.contains("protocol")) {
    publish(result);
    std::lock_guard cached(cache_mutex);
    result["revision"] = revision;
    result["refreshed_at_ms"] = refreshed_at_ms;
  }
  return result;
}
json Application::Impl::inspect(const json& params) {
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
Application::Application() : impl_(std::make_unique<Impl>()) {}
Application::~Application() = default;
json Application::dispatch(const json& request) {
  return impl_->dispatch(request);
}
} // namespace asterion::terminal
