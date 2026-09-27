#include <asterion/foundation/id.hpp>
#include <asterion/protocol/strategy.hpp>
namespace asterion::protocol {
Json decode_replay_plan(const strategy::v1::ReplayPlan& p) {
  validate_message(p);
  validate_id(p.trading_session());
  validate_id(p.grant_id());
  if (p.version() != 2 || !p.has_dataset())
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
  Json calendar = nullptr;
  if (p.has_calendar_publication()) {
    calendar = decode_calendar_publication(p.calendar_publication());
    if (calendar.at("calendar").at("contract") != decode_contract(p.dataset().contract()))
      throw std::invalid_argument("strategy calendar contract does not match dataset");
  }
  return {{"version", 2},
          {"calendar_publication", calendar},
          {"dataset", decode_dataset(p.dataset())},
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
  require_fields(j, {"version", "dataset", "trading_session", "grant_id", "agent_endpoint", "host",
                     "port", "tls_ca", "tls_cert", "tls_key", "calendar_publication"});
  if (!j.at("version").is_number_integer() || j.at("version") != 2 ||
      !j.at("port").is_number_integer() || j.at("port") < 0 || j.at("port") > 65535)
    throw std::invalid_argument("invalid replay plan version or port");
  strategy::v1::ReplayPlan p;
  p.set_version(2);
  if (!j.at("calendar_publication").is_null())
    *p.mutable_calendar_publication() = encode_calendar_publication(j.at("calendar_publication"));
  *p.mutable_dataset() = encode_dataset(j.at("dataset"));
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
