#include "application_impl.hpp"
#include <stdexcept>

namespace asterion::terminal {
// Read-only live market service.
void Application::Impl::register_market_commands() {
  command("market.credentials.save", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"account", "password", "auth_code"});
    co_await settings<void>([&] {
      ctp_connections.remember_market_credentials(text(p, "account"),
                                                  {text(p, "password"), text(p, "auth_code")});
    });
    co_return snapshot();
  });
  command("market.credentials.clear", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"account"});
    co_await settings<void>([&] { ctp_connections.forget_market_credentials(text(p, "account")); });
    co_return snapshot();
  });
  command("market.local", [this](const json& p) -> PolledTask<Response> {
    fields(p, {});
    if (market && nodes.contains("local"))
      co_return snapshot();
    auto [node, next] = (co_await manage<
                         std::pair<std::shared_ptr<NodeClient>, std::shared_ptr<MarketClient>>>(
        [this, existing = existing_local_node()]()
            -> PolledTask<std::pair<std::shared_ptr<NodeClient>, std::shared_ptr<MarketClient>>> {
          auto node = (co_await local_node_client(existing));
          auto client = (co_await PollFuture{
              MarketClient::open(service_io, (co_await PollFuture{node->local_market()}))});
          co_return std::pair{std::move(node), std::move(client)};
        }));
    nodes.try_emplace("local", std::move(node));
    if (!market)
      market = std::move(next);
    co_return snapshot();
  });
  command("market.attach", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"id", "service"});
    auto next = (co_await PollFuture{MarketClient::open(
        service_io,
        (co_await PollFuture{
            nodes.at(text(p, "id"))
                ->service_endpoint(text(p, "service"), asterion::node::v1::MARKET_DATA)}))});
    market = std::move(next);
    co_return snapshot();
  });
  command("market.connect", [this](const json& p) -> PolledTask<Response> {
    const auto client = market;
    fields(p, {"password", "instruments"});
    if (!client)
      throw std::invalid_argument("start or select a market service first");
    const auto connection = (co_await market_ctp());
    auto password = text(p, "password", true);
    if (password.empty())
      password = (co_await settings<MarketCredentials>([&] {
                   return ctp_connections.market_credentials(connection);
                 })).password;
    const json request{{"front", connection.market_front},
                       {"broker", connection.broker_id},
                       {"user", connection.user_id},
                       {"password", password},
                       {"instruments", p.at("instruments")}};
    co_await PollFuture{client->connect(request)};
    if (market != client)
      throw Error(ErrorCode::conflict, "market service selection changed during operation; inspect "
                                       "the original service before retrying");
    co_return snapshot();
  });
  command("market.subscribe", [this](const json& p) -> PolledTask<Response> {
    const auto client = market;
    fields(p, {"instruments"});
    if (!client)
      throw std::invalid_argument("select a market service first");
    (co_await PollFuture{client->subscribe(p.at("instruments"))});
    if (market != client)
      throw Error(ErrorCode::conflict, "market service selection changed during operation; inspect "
                                       "the original service before retrying");
    co_return snapshot();
  });
  command("market.catalog", [this](const json& p) -> PolledTask<Response> {
    // The contract catalog is read through the trade front of the named account.
    const auto client = market;
    fields(p, {"account", "password", "auth_code"});
    if (!client)
      throw std::invalid_argument("select a market service first");
    const auto connection =
        (co_await settings<CtpConnection>([&] { return ctp_connections.get(text(p, "account")); }));
    MarketCredentials credentials{text(p, "password", true), text(p, "auth_code", true)};
    if (credentials.password.empty() || credentials.auth_code.empty()) {
      const auto saved = co_await settings<MarketCredentials>(
          [&] { return ctp_connections.market_credentials(connection); });
      if (credentials.password.empty())
        credentials.password = saved.password;
      if (credentials.auth_code.empty())
        credentials.auth_code = saved.auth_code;
    }
    const json request{{"front", connection.trade_front}, {"broker", connection.broker_id},
                       {"user", connection.user_id},      {"password", credentials.password},
                       {"app_id", connection.app_id},     {"auth_code", credentials.auth_code}};
    co_await PollFuture{client->catalog(request)};
    if (market != client)
      throw Error(ErrorCode::conflict, "market service selection changed during operation; inspect "
                                       "the original service before retrying");
    co_return snapshot();
  });
  // Read-only: retain the selected client while its service request is suspended.
  command("market.minutes", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"venue", "symbol"});
    InstrumentId{text(p, "venue"), text(p, "symbol")}.validate();
    if (!market)
      throw std::invalid_argument("select a market service first");
    const auto reader = market;
    auto intraday = (co_await PollFuture{reader->minutes(text(p, "venue"), text(p, "symbol"))});
    auto result = read_published(json::object());
    result.values["intraday"] = std::move(intraday);
    co_return result;
  });
  command("market.disconnect", [this](const json& p) -> PolledTask<Response> {
    const auto client = market;
    fields(p, {});
    if (client)
      (co_await PollFuture{client->disconnect()});
    if (market != client)
      throw Error(ErrorCode::conflict, "market service selection changed during operation; inspect "
                                       "the original service before retrying");
    co_return snapshot();
  });
}
} // namespace asterion::terminal
