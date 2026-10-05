#include "application_impl.hpp"
namespace asterion::terminal {
PolledTask<std::string> Application::Impl::source_credential(const std::string& source,
                                                             std::string typed) {
  if (!data_client)
    throw std::invalid_argument("connect the data and task services first");
  if (!typed.empty())
    co_return typed;
  const auto provider = (co_await PollFuture{data_client->source(source)}).plugin_id();
  const auto saved = co_await settings<std::optional<DataCredential>>(
      [&] { return data_credentials.find(provider); });
  co_return saved ? saved->credential : std::string();
}
PolledTask<CtpConnection> Application::Impl::market_ctp() {
  auto connection =
      (co_await settings<std::optional<CtpConnection>>([&] { return ctp_connections.market(); }));
  if (!connection)
    throw std::invalid_argument("choose the CTP account for market data first");
  co_return std::move(*connection);
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
Application::Impl::Settings Application::Impl::inspect_settings(const Settings& previous) const {
  const auto selected = ctp_connections.market();
  SettingsView next{data_credentials.snapshot(), ctp_accounts(),
                    selected ? json(selected->id) : json(nullptr)};
  return next == *previous ? previous : std::make_shared<const SettingsView>(std::move(next));
}
void Application::Impl::register_connection_commands() {
  command("data.download.budget.configure", [this](const json& params) -> PolledTask<Response> {
    fields(params, {"source", "token", "requests_per_minute"});
    const auto client = data_client;
    if (!client)
      throw std::invalid_argument("connect data service first");
    const auto& rate = params.at("requests_per_minute");
    if (!rate.is_number_integer() || rate < 1 || rate > 500)
      throw std::invalid_argument("invalid download request budget");
    data::v1::DownloadBudgetConfiguration policy;
    policy.set_source(text(params, "source"));
    policy.set_credential(
        (co_await source_credential(policy.source(), text(params, "token", true))));
    policy.set_requests_per_minute(rate.get<unsigned>());
    (co_await PollFuture{client->configure_download_budget(policy)});
    if (data_client != client)
      throw Error(ErrorCode::conflict, "data service changed during budget configuration");
    co_return snapshot();
  });
  command("data.credentials.save", [this](const json& params) -> PolledTask<Response> {
    fields(params, {"provider", "credential", "remember", "requests_per_minute"});
    if (!data_client)
      throw std::invalid_argument("connect the data and task services first");
    const auto sources =
        (co_await PollFuture{data_client->provider_sources(text(params, "provider"))});
    if (sources.empty())
      throw std::invalid_argument("data source is unavailable");
    if (!params.at("requests_per_minute").is_number_unsigned() ||
        !params.at("remember").is_boolean())
      throw std::invalid_argument("invalid data source credential settings");
    // One credential serves every source of the provider, so it has to fit
    // the strictest of them.
    DataCredentialLimits limits{false, 256, true, 500};
    for (const auto& source : sources) {
      const auto& schema = source.connection();
      limits.credential_required |= schema.credential_required();
      limits.credential_max_length =
          std::min<std::size_t>(limits.credential_max_length, schema.credential_max_length());
      limits.remember_allowed &= schema.remember_allowed();
      limits.requests_per_minute_max =
          std::min(limits.requests_per_minute_max, schema.requests_per_minute_max());
    }
    (co_await settings<void>([&] {
      return data_credentials.save(
          {text(params, "provider"), params.at("requests_per_minute").get<unsigned>(),
           params.at("remember").get<bool>(), text(params, "credential", true)},
          limits);
    }));
    credential_verification = nullptr;
    co_return snapshot();
  });
  command("data.credentials.clear", [this](const json& params) -> PolledTask<Response> {
    fields(params, {"provider"});
    (co_await settings<void>([&] { return data_credentials.clear(text(params, "provider")); }));
    credential_verification = nullptr;
    co_return snapshot();
  });
  // Checks the saved credential against every source of the provider; the
  // provider I/O suspends without occupying the state owner.
  command("data.credentials.verify", [this](const json& params) -> PolledTask<Response> {
    fields(params, {"provider"});
    const auto client = data_client;
    if (!client)
      throw std::invalid_argument("connect the data and task services first");
    const auto provider = text(params, "provider");
    const auto sources = (co_await PollFuture{client->provider_sources(provider)});
    if (sources.empty())
      throw std::invalid_argument("data source is unavailable");
    const auto saved = (co_await settings<std::optional<DataCredential>>(
        [&] { return data_credentials.find(provider); }));
    if (!saved)
      throw std::invalid_argument("data source credential is required");
    json checks = json::array();
    (co_await run<void>([&]() -> PolledTask<void> {
      for (const auto& source : sources) {
        const auto verification =
            (co_await PollFuture{client->verify_connection(source.id(), saved->credential)});
        for (const auto& check : verification.checks())
          checks.push_back(
              {{"source", source.id()}, {"scope", check.scope()}, {"state", check.state()}});
      }
    }));
    if (data_client != client)
      throw Error(ErrorCode::conflict, "data connection changed; retry verification");
    credential_verification = {{"provider", provider}, {"checks", checks}};
    co_return snapshot();
  });
  command("ctp.connections.save", [this](const json& params) -> PolledTask<Response> {
    fields(params, {"id", "name", "revision", "broker_id", "user_id", "app_id", "trade_front",
                    "market_front"});
    // A trading record holds its own copy of the counter details; the account
    // it belongs to keeps them unchanged, so the two never disagree.
    co_await settings<void>([&] {
      if (std::filesystem::exists(ctp_record_directory(text(params, "id")) / "journal.sqlite")) {
        const auto current = ctp_connections.get(text(params, "id"));
        if (current.broker_id != text(params, "broker_id") ||
            current.user_id != text(params, "user_id") ||
            current.app_id != text(params, "app_id") ||
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
    });
    co_return snapshot();
  });
  // Market data follows one account; it changes only while market data is
  // disconnected. Trading accounts are not affected.
  command("ctp.connections.market", [this](const json& params) -> PolledTask<Response> {
    fields(params, {"id"});
    if (market) {
      const auto phase = market->owner_read().header.at("phase").get<std::string>();
      if (phase != "disconnected" && phase != "error" && phase != "sdk_unavailable")
        throw Error(ErrorCode::conflict, "disconnect market data before changing its CTP account");
    }
    (co_await settings<void>([&] { return ctp_connections.select_market(text(params, "id")); }));
    co_return snapshot();
  });
  command("ctp.connections.remove", [this](const json& params) -> PolledTask<Response> {
    fields(params, {"id", "revision"});
    if (live.contains(text(params, "id")))
      throw Error(ErrorCode::conflict, "close the trading account before removing it");
    (co_await settings<void>(
        [&] { return ctp_connections.remove(text(params, "id"), text(params, "revision")); }));
    co_return snapshot();
  });
}
} // namespace asterion::terminal
