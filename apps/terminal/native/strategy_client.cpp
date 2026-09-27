#include "strategy_client.hpp"
#include <asterion/foundation/decimal.hpp>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/process/child.hpp>
#include <thread>
using namespace std::chrono_literals;
namespace asterion::terminal {
StrategyClient::StrategyClient(ServiceEndpoint endpoint) : endpoint_(std::move(endpoint)) {
  validate_id(endpoint_.session);
}
strategy::v1::Response StrategyClient::call(strategy::v1::Request request, bool wait_for_start) {
  request.set_version(1);
  request.set_session_id(endpoint_.session);
  request.set_correlation_id(unique_process_id());
  auto exchange = [&](auto& channel) {
    channel.send(request.SerializeAsString(), 10s);
    strategy::v1::Response response;
    if (!response.ParseFromString(channel.receive(10s)))
      throw std::runtime_error("invalid strategy response");
    protocol::validate_message(response);
    if (response.version() != 1 || response.session_id() != endpoint_.session ||
        response.correlation_id() != request.correlation_id())
      throw std::runtime_error("strategy response identity mismatch");
    if (response.has_error())
      throw std::invalid_argument(response.error().message());
    if (!response.has_snapshot() && !response.has_uninitialized())
      throw std::runtime_error("missing strategy snapshot");
    return response;
  };
  if (endpoint_.endpoint.empty()) {
    auto channel = ipc::TlsChannel::connect(endpoint_.host, endpoint_.port, endpoint_.tls, 5s);
    return exchange(channel);
  }
  ipc::Channel channel;
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  for (;;) {
    try {
      channel = ipc::Channel::connect(endpoint_.endpoint, 200ms);
      break;
    } catch (const Error&) {
      if (!wait_for_start || std::chrono::steady_clock::now() >= deadline)
        throw;
      std::this_thread::sleep_for(50ms);
    }
  }
  return exchange(channel); // Do not resend a request after delivery begins.
}
void StrategyClient::observe(const strategy::v1::Response& response) {
  if (response.has_uninitialized()) {
    config_.Clear();
    last_ = {{"id", endpoint_.session}, {"state", "connected"}, {"phase", "uninitialized"},
             {"processed", 0},          {"total", 0},           {"error", ""}};
    return;
  }
  const auto& snapshot = response.snapshot();
  if (snapshot.config().session_id() != endpoint_.session || snapshot.config().version() != 1)
    throw std::runtime_error("invalid strategy configuration identity");
  if (snapshot.config().has_replay()) {
    static_cast<void>(protocol::decode_replay_plan(snapshot.config().replay()));
    const auto& phase = snapshot.replay().phase();
    if (snapshot.processed() >
            static_cast<std::uint64_t>(snapshot.config().replay().dataset().ticks_size()) ||
        (phase != "waiting" && phase != "running" && phase != "completed" && phase != "blocked"))
      throw std::runtime_error("invalid strategy replay status");
  }
  config_ = snapshot.config();
  last_ = {{"id", endpoint_.session},
           {"state", "connected"},
           {"phase", config_.has_replay() ? snapshot.replay().phase() : "manual"},
           {"processed", snapshot.processed()},
           {"total", config_.has_replay() ? config_.replay().dataset().ticks_size() : 0},
           {"fast", config_.fast()},
           {"slow", config_.slow()},
           {"quantity", Decimal::from_raw(config_.quantity().units()).str()},
           {"symbol", config_.contract().symbol()},
           {"error", snapshot.replay().error()}};
  if (config_.has_replay()) {
    last_["account"] = config_.replay().trading_session();
    last_["grant_id"] = config_.replay().grant_id();
    last_["revision"] = config_.replay().dataset().revision();
  }
  if (snapshot.recovery_required())
    last_["phase"] = "blocked";
}
void StrategyClient::create(const strategy::v1::Config& config) {
  strategy::v1::Request r;
  *r.mutable_create() = config;
  observe(call(r, true));
}
Json StrategyClient::status() {
  try {
    strategy::v1::Request r;
    r.mutable_snapshot();
    observe(call(r));
    return last_;
  } catch (const std::exception& error) {
    auto result =
        last_.is_null()
            ? Json{{"id", endpoint_.session}, {"phase", "unknown"}, {"processed", 0}, {"total", 0}}
            : last_;
    result["state"] = "disconnected";
    result["error"] = error.what();
    return result;
  }
}
const strategy::v1::Config& StrategyClient::config() const {
  if (!config_.has_replay())
    throw std::invalid_argument("attach an initialized automatic strategy first");
  return config_;
}
} // namespace asterion::terminal
