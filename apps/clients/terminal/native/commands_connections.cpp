#include "application_impl.hpp"
namespace asterion::terminal {
DataConnection Application::Impl::resolve_data_connection(const std::string& id,
                                                          const std::string& revision,
                                                          const std::string& source) {
  if (!research)
    throw std::invalid_argument("connect research service first");
  auto connection = data_connections.get(id);
  const auto provider = research->source(source);
  if (connection.revision != revision)
    throw Error(ErrorCode::conflict, "data connection changed; inspect again");
  if (connection.source != source || connection.plugin_id != provider.plugin_id() ||
      !provider.has_connection())
    throw std::invalid_argument("data connection provider is unavailable");
  const auto& schema = provider.connection();
  if (connection.requests_per_minute > schema.requests_per_minute_max() ||
      connection.credential.size() > schema.credential_max_length() ||
      (schema.credential_required() && connection.credential.empty()) ||
      (connection.remember && !schema.remember_allowed()))
    throw std::invalid_argument("data connection requires configuration");
  return connection;
}
CtpConnection Application::Impl::market_ctp() const {
  auto connection = ctp_connections.market();
  if (!connection)
    throw std::invalid_argument("choose the CTP account for market data first");
  return std::move(*connection);
}
std::filesystem::path Application::Impl::ctp_record_directory(const std::string& account) {
  return ctp_account_directory(account, false);
}
json Application::Impl::ctp_accounts() const {
  auto accounts = ctp_connections.snapshot();
  for (auto& account : accounts)
    if (!account.contains("error"))
      account["trading_record"] = std::filesystem::exists(
          ctp_record_directory(account.at("id").get<std::string>()) / "journal.sqlite");
  return accounts;
}
void Application::Impl::register_connection_commands() {
  core.command("research.connections.save", [this](const json& params) {
    fields(params, {"id", "name", "source", "revision", "requests_per_minute", "remember",
                    "credential", "credential_action"});
    if (!research)
      throw std::invalid_argument("connect research service first");
    const auto source = research->source(text(params, "source"));
    if (!source.has_connection())
      throw std::invalid_argument("history connection configuration is unsupported");
    if (!params.at("requests_per_minute").is_number_unsigned() ||
        !params.at("remember").is_boolean())
      throw std::invalid_argument("invalid data connection settings");
    DataConnection connection{text(params, "id"),
                              text(params, "name"),
                              source.id(),
                              source.plugin_id(),
                              {},
                              params.at("requests_per_minute").get<unsigned>(),
                              params.at("remember").get<bool>(),
                              text(params, "credential", true)};
    data_connections.save(std::move(connection), text(params, "revision", true),
                          text(params, "credential_action"), source.connection());
    connection_verification = nullptr;
    return snapshot();
  });
  core.command("research.connections.remove", [this](const json& params) {
    fields(params, {"id", "revision"});
    data_connections.remove(text(params, "id"), text(params, "revision"));
    connection_verification = nullptr;
    return snapshot();
  });
  // Verifies a saved connection against its data source; the provider I/O
  // runs outside the lock.
  core.command("research.connections.verify", [this](const json& params) {
    fields(params, {"id", "revision", "source"});
    const auto connection = resolve_data_connection(text(params, "id"), text(params, "revision"),
                                                    text(params, "source"));
    const auto client = research;
    if (!client)
      throw std::invalid_argument("connect research service first");
    const auto verification = outside_lock(
        [&] { return client->verify_connection(connection.source, connection.credential); });
    if (research != client)
      throw Error(ErrorCode::conflict, "research connection changed; retry verification");
    (void)resolve_data_connection(text(params, "id"), text(params, "revision"),
                                  text(params, "source"));
    json checks = json::array();
    for (const auto& check : verification.checks())
      checks.push_back({{"scope", check.scope()}, {"state", check.state()}});
    connection_verification = {
        {"id", text(params, "id")}, {"revision", text(params, "revision")}, {"checks", checks}};
    return snapshot();
  });
  core.command("ctp.connections.save", [this](const json& params) {
    fields(params, {"id", "name", "revision", "broker_id", "user_id", "app_id", "trade_front",
                    "market_front"});
    // A trading record holds its own copy of the counter details; the account
    // it belongs to keeps them unchanged, so the two never disagree.
    if (std::filesystem::exists(ctp_record_directory(text(params, "id")) / "journal.sqlite")) {
      const auto current = ctp_connections.get(text(params, "id"));
      if (current.broker_id != text(params, "broker_id") ||
          current.user_id != text(params, "user_id") || current.app_id != text(params, "app_id") ||
          current.trade_front != text(params, "trade_front"))
        throw Error(ErrorCode::conflict,
                    "this account already trades; its counter details are fixed");
    }
    ctp_connections.save({text(params, "id"),
                          text(params, "name"),
                          text(params, "broker_id"),
                          text(params, "user_id"),
                          text(params, "app_id"),
                          text(params, "trade_front"),
                          text(params, "market_front"),
                          {}},
                         text(params, "revision", true));
    return snapshot();
  });
  // Market data follows one account; it changes only while market data is
  // disconnected. Trading accounts are not affected.
  core.command("ctp.connections.market", [this](const json& params) {
    fields(params, {"id"});
    if (market) {
      const auto phase = market->snapshot().at("phase").get<std::string>();
      if (phase != "disconnected" && phase != "error" && phase != "sdk_unavailable")
        throw Error(ErrorCode::conflict, "disconnect market data before changing its CTP account");
    }
    ctp_connections.select_market(text(params, "id"));
    return snapshot();
  });
  core.command("ctp.connections.remove", [this](const json& params) {
    fields(params, {"id", "revision"});
    if (live.contains(text(params, "id")))
      throw Error(ErrorCode::conflict, "close the trading account before removing it");
    ctp_connections.remove(text(params, "id"), text(params, "revision"));
    return snapshot();
  });
}
} // namespace asterion::terminal
