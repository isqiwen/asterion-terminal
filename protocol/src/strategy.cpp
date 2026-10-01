#include <asterion/foundation/id.hpp>
#include <asterion/domain/account.hpp>
#include <asterion/protocol/strategy.hpp>
#include <stdexcept>
namespace asterion::protocol {
Json decode_replay_plan(const strategy::v1::ReplayPlan& p) {
  validate_message(p);
  validate_id(p.trading_session());
  validate_id(p.grant_id());
  if (p.version() != 4 || p.datasets().empty() ||
      static_cast<std::size_t>(p.datasets_size()) > max_portfolio_contracts)
    throw std::invalid_argument("unsupported strategy replay plan");
  for (const auto* value :
       {&p.agent_endpoint(), &p.host(), &p.tls_ca(), &p.tls_cert(), &p.tls_key()})
    if (value->size() > 4096 || value->find('\0') != std::string::npos)
      throw std::invalid_argument("invalid replay connection field");
  const bool local = !p.agent_endpoint().empty();
  if (local ? (!p.host().empty() || p.port() || !p.tls_ca().empty() || !p.tls_cert().empty() ||
               !p.tls_key().empty())
            : (p.host().empty() || !p.port() || p.port() > 65535 || p.tls_ca().empty() ||
               p.tls_cert().empty() || p.tls_key().empty()))
    throw std::invalid_argument(
        "replay requires a local Agent OR complete remote TCP/mTLS identity");
  // Evaluated before the braced initializer: GCC < 13 leaks already-built
  // initializer_list elements when a later element throws (PR66139).
  Json datasets = Json::array();
  for (const auto& dataset : p.datasets())
    datasets.push_back(decode_bar_dataset(dataset));
  return {{"version", 4},
          {"datasets", std::move(datasets)},
          {"trading_session", p.trading_session()},
          {"grant_id", p.grant_id()},
          {"agent_endpoint", p.agent_endpoint()},
          {"host", p.host()},
          {"port", p.port()},
          {"tls_ca", p.tls_ca()},
          {"tls_cert", p.tls_cert()},
          {"tls_key", p.tls_key()}};
}
strategy::v1::ReplayPlan encode_replay_plan(const Json& j) {
  require_fields(j, {"version", "datasets", "trading_session", "grant_id", "agent_endpoint", "host",
                     "port", "tls_ca", "tls_cert", "tls_key"});
  if (!j.at("version").is_number_integer() || j.at("version") != 4 ||
      !j.at("datasets").is_array() || !j.at("port").is_number_integer() || j.at("port") < 0 ||
      j.at("port") > 65535)
    throw std::invalid_argument("invalid replay plan version or port");
  strategy::v1::ReplayPlan p;
  p.set_version(4);
  for (const auto& dataset : j.at("datasets"))
    *p.add_datasets() = encode_bar_dataset(dataset);
  p.set_trading_session(j.at("trading_session").get<std::string>());
  p.set_grant_id(j.at("grant_id").get<std::string>());
  p.set_agent_endpoint(j.at("agent_endpoint").get<std::string>());
  p.set_host(j.at("host").get<std::string>());
  p.set_port(j.at("port").get<std::uint32_t>());
  p.set_tls_ca(j.at("tls_ca").get<std::string>());
  p.set_tls_cert(j.at("tls_cert").get<std::string>());
  p.set_tls_key(j.at("tls_key").get<std::string>());
  static_cast<void>(decode_replay_plan(p));
  return p;
}
} // namespace asterion::protocol
