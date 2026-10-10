#include "application_impl.hpp"
#include <stdexcept>

namespace asterion::terminal {
namespace {
// Exchange terms of the allowed contracts from the CTP contract catalog the
// market service loaded; domestic CTP futures trade whole lots in CNY.
json policy_contracts(const std::shared_ptr<const json>& catalog, const json& requested,
                      const json& retained = json::array()) {
  if (!requested.is_array() || requested.empty() || requested.size() > max_portfolio_contracts)
    throw std::invalid_argument("choose 1 to 20 contracts for the live session");
  json contracts = json::array();
  for (const auto& item : requested) {
    fields(item, {"venue", "symbol"});
    const auto known = std::ranges::find_if(retained, [&](const json& candidate) {
      return candidate.at("venue") == item.at("venue") &&
             candidate.at("symbol") == item.at("symbol");
    });
    if (known != retained.end()) {
      contracts.push_back(*known);
      continue;
    }
    if (!catalog || catalog->at("phase") != "ready")
      throw std::invalid_argument("load the CTP contract catalog in the market workspace first");
    const auto& rows = catalog->at("contracts");
    const auto row = std::ranges::find_if(rows, [&](const json& candidate) {
      return candidate.at("venue") == item.at("venue") &&
             candidate.at("symbol") == item.at("symbol");
    });
    if (row == rows.end())
      throw std::invalid_argument("contract is not in the CTP contract catalog");
    const auto identity = HistoryIdentity::parse(row->at("contract_id").get<std::string>());
    contracts.push_back(
        {{"venue", row->at("venue")},
         {"symbol", row->at("symbol")},
         {"currency", "CNY"},
         {"price_increment", Decimal::parse(row->at("price_tick").get<std::string>()).str()},
         {"quantity_increment", "1"},
         {"multiplier", std::to_string(row->at("multiplier").get<int>())},
         {"product", identity.product},
         {"delivery_month", identity.delivery_month}});
  }
  return contracts;
}
} // namespace
std::shared_ptr<Application::Impl::LiveAccount>
Application::Impl::live_account(const json& params) {
  const auto found = live.find(text(params, "account"));
  if (found == live.end())
    throw std::invalid_argument("open this CTP account first");
  return found->second;
}
// CTP trading accounts: each account has one record and one service. Several
// may be open at once; every command names its account, and every order
// passes that account's authorization, allowlist and risk.
void Application::Impl::register_live_commands() {
  command("live.create", [this](const json& p) -> PolledTask<Response> {
    fields_with_risk(p, {"account", "max_price_deviation", "contracts"});
    const auto account = text(p, "account");
    if (live.contains(account))
      throw std::invalid_argument("this CTP account is already open");
    // Validated before the braced initializer (GCC < 13 PR66139 leak).
    const auto risk = risk_parameters(p);
    const auto connection =
        (co_await settings<CtpConnection>([&] { return ctp_connections.get(account); }));
    if ((co_await service_io.admin<bool>([&] {
          return std::filesystem::exists(ctp_record_directory(account) / "journal.sqlite");
        })))
      throw Error(ErrorCode::conflict, "this CTP account already has a trading record; open it");
    auto contracts =
        policy_contracts(market ? market->owner_read().catalog : nullptr, p.at("contracts"));
    json manifest{{"version", 5},
                  {"account_id", account},
                  {"type", "live_ctp"},
                  {"broker",
                   {{"front", connection.trade_front},
                    {"broker_id", connection.broker_id},
                    {"user_id", connection.user_id},
                    {"app_id", connection.app_id}}},
                  {"policy",
                   {{"risk", risk},
                    {"max_price_deviation", text(p, "max_price_deviation")},
                    {"contracts", std::move(contracts)}}}};
    (void)protocol::encode_live_input(manifest);
    auto [node, next] = (co_await manage<
                         std::pair<std::shared_ptr<NodeClient>, std::shared_ptr<LiveAccount>>>(
        [&, existing = existing_local_node()]()
            -> PolledTask<std::pair<std::shared_ptr<NodeClient>, std::shared_ptr<LiveAccount>>> {
          auto node = (co_await local_node_client(existing));
          auto client = std::make_shared<LiveAccount>((co_await PollFuture{
              TradingClient::open(service_io,
                                  (co_await PollFuture{node->local_session(
                                      co_await service_io.admin<std::filesystem::path>(
                                          [&] { return ctp_account_directory(account, true); }))}),
                                  manifest)}));
          co_return std::pair{std::move(node), std::move(client)};
        }));
    nodes.try_emplace("local", std::move(node));
    if (!live.try_emplace(account, std::move(next)).second)
      throw Error(ErrorCode::conflict, "another window opened this CTP account meanwhile");
    co_return snapshot();
  });
  command("live.open", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"account"});
    const auto account = text(p, "account");
    if (live.contains(account))
      co_return snapshot();
    static_cast<void>(
        (co_await settings<CtpConnection>([&] { return ctp_connections.get(account); })));
    const auto directory = ctp_record_directory(account);
    if (!(co_await service_io.admin<bool>(
            [&] { return std::filesystem::exists(directory / "journal.sqlite"); })))
      throw std::invalid_argument("this CTP account has no trading record yet");
    auto [node, next] = (co_await manage<
                         std::pair<std::shared_ptr<NodeClient>, std::shared_ptr<LiveAccount>>>(
        [&, existing = existing_local_node()]()
            -> PolledTask<std::pair<std::shared_ptr<NodeClient>, std::shared_ptr<LiveAccount>>> {
          auto node = (co_await local_node_client(existing));
          auto client = std::make_shared<LiveAccount>((co_await PollFuture{TradingClient::open(
              service_io, (co_await PollFuture{node->local_session(directory)}))}));
          co_return std::pair{std::move(node), std::move(client)};
        }));
    nodes.try_emplace("local", std::move(node));
    live.try_emplace(account, std::move(next));
    co_return snapshot();
  });
  // Credentials go to the account's service and are not kept by the Terminal.
  command("live.connect", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"account", "password", "auth_code"});
    co_await with_live_account(p, [&](TradingClient& client) -> PolledTask<void> {
      (co_await PollFuture{client.connect_broker(text(p, "password"), text(p, "auth_code"))});
    });
    co_return snapshot();
  });
  command("live.disconnect", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"account"});
    co_await with_live_account(p, [](TradingClient& client) -> PolledTask<void> {
      (co_await PollFuture{client.disconnect_broker()});
    });
    co_return snapshot();
  });
  // The account's margin and commission rates from the broker; observations
  // only, never recorded as trading commands.
  command("live.costs", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"account"});
    co_await with_live_account(p, [](TradingClient& client) -> PolledTask<void> {
      (co_await PollFuture{client.query_costs()});
    });
    co_return snapshot();
  });
  // The account is part of the request, never implied: an order cannot reach
  // another account because a different one happens to be selected.
  command("live.act", [this](const json& p) -> PolledTask<Response> {
    auto command = p;
    command.erase("account");
    const auto execute = [&](TradingClient& client) -> PolledTask<void> {
      (co_await PollFuture{client.execute(command)});
    };
    // A cancel or a strategy stop must not wait for, or be refused because
    // of, an order in flight.
    if (const auto action = command.value("action", "");
        action == "cancel" || action == "strategy_stop")
      co_await cancel_on_live_account(p, execute);
    else
      co_await with_live_account(p, execute);
    co_return snapshot();
  });
  // The run reads minute bars from the market service on this machine; its
  // address comes from the attached service, never from the page.
  command("live.strategy.start", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"account", "request_id", "account_id", "policy_revision", "venue", "symbol",
               "strategy"});
    const auto source = market ? market->endpoint() : ServiceEndpoint{};
    if (source.endpoint.empty())
      throw Error(ErrorCode::unavailable,
                  "a strategy reads the market service on this machine; attach it first");
    auto command = p;
    command.erase("account");
    command["action"] = "strategy_start";
    command["market_endpoint"] = source.endpoint;
    command["market_service"] = source.session;
    co_await with_live_account(p, [&](TradingClient& client) -> PolledTask<void> {
      (co_await PollFuture{client.execute(command)});
    });
    co_return snapshot();
  });
  command("live.policy.configure", [this](const json& p) -> PolledTask<Response> {
    fields_with_risk(p, {"account", "request_id", "account_id", "policy_revision", "risk_artifact",
                         "max_price_deviation", "contracts"});
    const auto risk = risk_parameters(p);
    const auto catalog = market ? market->owner_read().catalog : nullptr;
    co_await with_live_account(p, [&](TradingClient& client) -> PolledTask<void> {
      const auto current = client.owner_view().session;
      const auto contracts = policy_contracts(catalog, p.at("contracts"), current->at("contracts"));
      const json command{{"account_id", text(p, "account_id")},
                         {"policy_revision", text(p, "policy_revision")},
                         {"request_id", text(p, "request_id")},
                         {"action", "live_policy"},
                         {"risk_artifact", text(p, "risk_artifact")},
                         {"policy",
                          {{"risk", risk},
                           {"contracts", contracts},
                           {"max_price_deviation", text(p, "max_price_deviation")}}}};
      co_await PollFuture{client.execute(command)};
    });
    co_return snapshot();
  });
  command("live.close", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"account"});
    const auto id = text(p, "account");
    const auto found = live.find(id);
    if (found == live.end())
      co_return snapshot();
    auto account = found->second;
    if (account->busy || account->cancels)
      throw Error(
          ErrorCode::conflict,
          "another operation for this CTP account is in progress; retry after it completes");
    live.erase(found);
    account->client.reset();
    co_return snapshot();
  });
}
} // namespace asterion::terminal
