#include "session.hpp"
#include "sqlite_journal.hpp"
#include "moving_average.hpp"
#include <asterion/domain/futures.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/strategy.hpp>
#include <stdexcept>
namespace asterion::strategy {
namespace {
Instrument instrument(const protocol::v1::Contract& c) {
  if (!c.has_price_increment() || !c.has_quantity_increment() || !c.has_multiplier())
    throw std::invalid_argument("incomplete strategy contract");
  Instrument value{{c.venue(), c.symbol()},
                   AssetClass::futures,
                   c.currency(),
                   Decimal::from_raw(c.price_increment().units()),
                   Decimal::from_raw(c.quantity_increment().units()),
                   Decimal::from_raw(c.multiplier().units())};
  FuturesContract{value, c.product(), c.delivery_month()}.validate();
  return value;
}
Json config_json(const v1::Config& config) {
  protocol::validate_message(config);
  validate_id(config.session_id());
  validate_id(config.stream_id());
  if (config.version() != 1 || !config.has_contract() || !config.has_quantity() ||
      config.plugin_id() != "asterion.strategy.cta.sma-long-flat")
    throw std::invalid_argument("unsupported strategy configuration");
  // Plugin owns the algorithm's parameter and lot-size constraints.
  MovingAverage validation(instrument(config.contract()), config.fast(), config.slow(),
                           Decimal::from_raw(config.quantity().units()));
  Json result = {{"version", 1},
                 {"session_id", config.session_id()},
                 {"stream_id", config.stream_id()},
                 {"contract", protocol::decode_contract(config.contract())},
                 {"plugin_id", config.plugin_id()},
                 {"fast", config.fast()},
                 {"slow", config.slow()},
                 {"quantity", config.quantity().units()}};
  if (config.has_replay()) {
    result["replay"] = protocol::decode_replay_plan(config.replay());
    if (protocol::decode_contract(config.contract()) !=
        protocol::decode_contract(config.replay().dataset().contract()))
      throw std::invalid_argument("strategy replay contract mismatch");
  }
  return result;
}
v1::Config parse_config(const Json& j) {
  if (j.contains("replay"))
    require_fields(j, {"version", "session_id", "stream_id", "contract", "plugin_id", "fast",
                       "slow", "quantity", "replay"});
  else
    require_fields(j, {"version", "session_id", "stream_id", "contract", "plugin_id", "fast",
                       "slow", "quantity"});
  for (const auto* field : {"version", "fast", "slow", "quantity"})
    if (!j.at(field).is_number_integer())
      throw std::invalid_argument("invalid strategy configuration number");
  if (j.at("version") != 1 || j.at("fast") < 1 || j.at("fast") > 10000 || j.at("slow") < 2 ||
      j.at("slow") > 10000)
    throw std::invalid_argument("invalid strategy configuration range");
  v1::Config c;
  c.set_version(1);
  c.set_session_id(j.at("session_id").get<std::string>());
  c.set_stream_id(j.at("stream_id").get<std::string>());
  c.set_plugin_id(j.at("plugin_id").get<std::string>());
  c.set_fast(j.at("fast").get<std::uint32_t>());
  c.set_slow(j.at("slow").get<std::uint32_t>());
  // Canonical round-trip below also rejects integer conversion wraparound.
  c.mutable_quantity()->set_units(j.at("quantity").get<std::int64_t>());
  *c.mutable_contract() = protocol::encode_contract(j.at("contract"));
  if (j.contains("replay"))
    *c.mutable_replay() = protocol::encode_replay_plan(j.at("replay"));
  if (config_json(c) != j)
    throw std::invalid_argument("noncanonical strategy configuration");
  return c;
}
Json event_json(const v1::Event& event) {
  protocol::validate_message(event);
  if (!event.has_bar() || !event.sequence() || event.sequence() > protocol::max_dataset_bars)
    throw std::invalid_argument("strategy event requires a bar and sequence in 1..20000");
  const auto bar = protocol::market_bar(event.bar());
  return {{"stream_id", event.stream_id()},
          {"sequence", event.sequence()},
          {"bar",
           {{"trading_day", bar.trading_day},
            {"timestamp_ns", std::to_string(bar.timestamp_ns)},
            {"open", bar.open.str()},
            {"high", bar.high.str()},
            {"low", bar.low.str()},
            {"close", bar.close.str()},
            {"volume", bar.volume.str()}}}};
}
Json receipt_json(const v1::Receipt& receipt) {
  Json result{{"sequence", receipt.sequence()}, {"intent", nullptr}};
  if (receipt.has_intent()) {
    const auto& i = receipt.intent();
    result["intent"] = {{"id", i.id()},
                        {"sequence", i.sequence()},
                        {"timestamp_ns", i.timestamp_ns()},
                        {"target_quantity", i.target_quantity().units()}};
  }
  return result;
}
} // namespace
struct Session::Impl {
  SqliteJournal journal;
  v1::Config config;
  Json intent_scope;
  std::unique_ptr<MovingAverage> plugin;
  std::vector<Json> events;
  std::vector<v1::Receipt> receipts;
  bool poisoned = false;
  explicit Impl(const std::filesystem::path& directory) : journal(directory) {}
  std::pair<std::unique_ptr<MovingAverage>, v1::Receipt> prepare(const v1::Event& event) {
    auto candidate = std::make_unique<MovingAverage>(*plugin);
    const auto bar = protocol::market_bar(event.bar());
    const auto target = candidate->on_bar(bar);
    v1::Receipt receipt;
    receipt.set_sequence(event.sequence());
    if (target) {
      auto* intent = receipt.mutable_intent();
      intent->set_sequence(event.sequence());
      intent->set_timestamp_ns(bar.timestamp_ns);
      intent->mutable_target_quantity()->set_units(target->raw());
      // Bind identity to the immutable config, full source event and output.
      intent->set_id(sha256_bytes(Json{{"config", intent_scope},
                                       {"event", event_json(event)},
                                       {"target_quantity", target->raw()}}
                                      .dump()));
    }
    return {std::move(candidate), std::move(receipt)};
  }
};
Session::Session(const std::filesystem::path& directory, const std::string& session_id,
                 const v1::Config* create)
    : impl_(std::make_unique<Impl>(directory)) {
  validate_id(session_id);
  if (!directory.is_absolute() || !std::filesystem::is_directory(directory) ||
      std::filesystem::is_symlink(directory))
    throw std::invalid_argument("strategy directory must be absolute and existing");
  if (create) {
    static_cast<void>(config_json(*create));
    if (create->session_id() != session_id)
      throw std::invalid_argument("strategy session mismatch");
  }
  impl_->journal.start();
  auto records = impl_->journal.read();
  if (records.empty()) {
    if (!create)
      throw std::invalid_argument("strategy session is not initialized");
    impl_->journal.append(
        {{"version", 1}, {"type", "strategy.config"}, {"config", config_json(*create)}});
    records = impl_->journal.read();
  }
  require_fields(records.front(), {"version", "type", "config"});
  if (!records.front().at("version").is_number_integer() || records.front().at("version") != 1 ||
      records.front().at("type") != "strategy.config" || records.size() > 10001)
    throw std::invalid_argument("unsupported strategy journal");
  impl_->config = parse_config(records.front().at("config"));
  if (impl_->config.session_id() != session_id)
    throw std::invalid_argument("strategy journal session mismatch");
  if (create)
    verify_config(*create);
  impl_->intent_scope = config_json(impl_->config);
  if (impl_->config.has_replay())
    impl_->intent_scope = {{"configuration_sha256", sha256_bytes(impl_->intent_scope.dump())}};
  impl_->plugin = std::make_unique<MovingAverage>(
      instrument(impl_->config.contract()), impl_->config.fast(), impl_->config.slow(),
      Decimal::from_raw(impl_->config.quantity().units()));
  impl_->plugin->start();
  impl_->events.reserve(10000);
  impl_->receipts.reserve(10000);
  for (std::size_t index = 1; index < records.size(); ++index) {
    const auto& r = records[index];
    require_fields(r, {"version", "type", "event", "receipt"});
    // Version 2: bar events. Earlier tick journals are refused, not converted.
    if (!r.at("version").is_number_integer() || r.at("version") != 2 ||
        r.at("type") != "strategy.event")
      throw std::invalid_argument("unsupported strategy event record");
    const auto& j = r.at("event");
    require_fields(j, {"stream_id", "sequence", "bar"});
    if (!j.at("sequence").is_number_integer() || j.at("sequence") != index ||
        j.at("stream_id") != impl_->config.stream_id())
      throw std::invalid_argument("strategy journal stream or sequence mismatch");
    v1::Event event;
    event.set_stream_id(impl_->config.stream_id());
    event.set_sequence(index);
    const auto& b = j.at("bar");
    require_fields(b, {"trading_day", "timestamp_ns", "open", "high", "low", "close", "volume"});
    const auto time = b.at("timestamp_ns").get<std::string>();
    *event.mutable_bar() =
        protocol::encode_bar({b.at("trading_day").get<std::string>(), std::stoll(time),
                              Decimal::parse(b.at("open").get<std::string>()),
                              Decimal::parse(b.at("high").get<std::string>()),
                              Decimal::parse(b.at("low").get<std::string>()),
                              Decimal::parse(b.at("close").get<std::string>()),
                              Decimal::parse(b.at("volume").get<std::string>())});
    if (event_json(event) != j)
      throw std::invalid_argument("noncanonical strategy event");
    auto [candidate, receipt] = impl_->prepare(event);
    if (receipt_json(receipt) != r.at("receipt"))
      throw std::invalid_argument("strategy intent replay mismatch");
    impl_->plugin = std::move(candidate);
    impl_->events.push_back(j);
    impl_->receipts.push_back(std::move(receipt));
  }
}
Session::~Session() = default;
const v1::Config& Session::config() const noexcept {
  return impl_->config;
}
std::uint64_t Session::processed() const noexcept {
  return impl_->events.size();
}
bool Session::recovery_required() const noexcept {
  return impl_->poisoned;
}
void Session::verify_config(const v1::Config& config) const {
  if (config_json(config) != config_json(impl_->config))
    throw std::invalid_argument("strategy configuration conflict");
}
v1::Receipt Session::apply(const v1::Event& event) {
  if (impl_->poisoned)
    throw std::runtime_error("strategy journal requires inspection and recovery");
  const auto payload = event_json(event);
  if (event.stream_id() != impl_->config.stream_id())
    throw std::invalid_argument("strategy stream mismatch");
  if (event.sequence() <= impl_->events.size()) {
    const auto index = static_cast<std::size_t>(event.sequence() - 1);
    if (impl_->events[index] != payload)
      throw std::invalid_argument("strategy event sequence conflict");
    return impl_->receipts[index];
  }
  if (event.sequence() != impl_->events.size() + 1)
    throw std::invalid_argument("strategy event sequence gap");
  auto [candidate, receipt] = impl_->prepare(event);
  const Json record{{"version", 2},
                    {"type", "strategy.event"},
                    {"event", payload},
                    {"receipt", receipt_json(receipt)}};
  try {
    impl_->journal.append(record);
    impl_->events.push_back(payload);
    impl_->receipts.push_back(receipt);
    impl_->plugin = std::move(candidate);
  } catch (...) {
    impl_->poisoned = true;
    throw;
  }
  return receipt;
}
v1::Snapshot Session::snapshot() const {
  v1::Snapshot value;
  *value.mutable_config() = impl_->config;
  value.set_processed(impl_->events.size());
  value.set_recovery_required(impl_->poisoned);
  if (!impl_->receipts.empty() && impl_->receipts.back().has_intent())
    *value.mutable_latest_intent() = impl_->receipts.back().intent();
  return value;
}
} // namespace asterion::strategy
