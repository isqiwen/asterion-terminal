#include "application_impl.hpp"
#include <stdexcept>

namespace asterion::terminal {
namespace {
// Exchange terms of the allowed contracts from the CTP contract catalog the
// market service loaded; domestic CTP futures trade whole lots in CNY.
json catalog_terms(const json& market, const json& requested) {
  if (!requested.is_array() || requested.empty() || requested.size() > max_portfolio_contracts)
    throw std::invalid_argument("choose 1 to 20 contracts for the live session");
  if (market.is_null() || market.at("catalog").at("phase") != "ready")
    throw std::invalid_argument("load the CTP contract catalog in the market workspace first");
  json contracts = json::array();
  for (const auto& item : requested) {
    fields(item, {"venue", "symbol"});
    const auto& rows = market.at("catalog").at("contracts");
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
TradingClient& Application::Impl::live_account(const json& params) {
  const auto found = live.find(text(params, "account"));
  if (found == live.end())
    throw std::invalid_argument("open this CTP account first");
  return *found->second;
}
// CTP trading accounts: each account has one record and one service. Several
// may be open at once; every command names its account, and every order
// passes that account's authorization, allowlist and risk.
void Application::Impl::register_live_commands() {
  core.command("live.create", [this](const json& p) {
    fields_with_risk(p, {"account", "max_price_deviation", "contracts"});
    const auto account = text(p, "account");
    if (live.contains(account))
      throw std::invalid_argument("this CTP account is already open");
    // Validated before the braced initializer (GCC < 13 PR66139 leak).
    const auto risk = risk_parameters(p);
    const auto connection = ctp_connections.get(account);
    if (connection.trade_front.empty())
      throw std::invalid_argument("CTP connection has no trade front");
    if (std::filesystem::exists(ctp_record_directory(account) / "journal.sqlite"))
      throw Error(ErrorCode::conflict, "this CTP account already has a trading record; open it");
    auto contracts = catalog_terms(market ? market->snapshot() : json(nullptr), p.at("contracts"));
    json manifest{{"version", 2},
                  {"type", "live_ctp"},
                  {"broker",
                   {{"front", connection.trade_front},
                    {"broker_id", connection.broker_id},
                    {"user_id", connection.user_id},
                    {"app_id", connection.app_id}}},
                  {"risk", risk},
                  {"max_price_deviation", text(p, "max_price_deviation")},
                  {"contracts", std::move(contracts)}};
    (void)protocol::encode_live_input(manifest);
    auto [node, next] = without_operations([&, existing = existing_local_node()] {
      auto node = local_node_client(existing);
      auto client = std::make_unique<TradingClient>(ctp_account_directory(account, true), manifest);
      return std::pair{std::move(node), std::move(client)};
    });
    nodes.try_emplace("local", std::move(node));
    if (!live.try_emplace(account, std::move(next)).second)
      throw Error(ErrorCode::conflict, "another window opened this CTP account meanwhile");
    return snapshot();
  });
  core.command("live.open", [this](const json& p) {
    fields(p, {"account"});
    const auto account = text(p, "account");
    if (live.contains(account))
      return snapshot();
    static_cast<void>(ctp_connections.get(account));
    const auto directory = ctp_record_directory(account);
    if (!std::filesystem::exists(directory / "journal.sqlite"))
      throw std::invalid_argument("this CTP account has no trading record yet");
    auto [node, next] = without_operations([&, existing = existing_local_node()] {
      auto node = local_node_client(existing);
      auto client = std::make_unique<TradingClient>(directory);
      return std::pair{std::move(node), std::move(client)};
    });
    nodes.try_emplace("local", std::move(node));
    live.try_emplace(account, std::move(next));
    return snapshot();
  });
  // Credentials go to the account's service and are not kept by the Terminal.
  core.command("live.connect", [this](const json& p) {
    fields(p, {"account", "password", "auth_code"});
    live_account(p).connect_broker(text(p, "password"), text(p, "auth_code"));
    return snapshot();
  });
  core.command("live.disconnect", [this](const json& p) {
    fields(p, {"account"});
    live_account(p).disconnect_broker();
    return snapshot();
  });
  // The account's margin and commission rates from the broker; observations
  // only, never recorded as trading commands.
  core.command("live.costs", [this](const json& p) {
    fields(p, {"account"});
    live_account(p).query_costs();
    return snapshot();
  });
  // The account is part of the request, never implied: an order cannot reach
  // another account because a different one happens to be selected.
  core.command("live.act", [this](const json& p) {
    auto& account = live_account(p);
    auto command = p;
    command.erase("account");
    account.execute(command);
    return snapshot();
  });
  core.command("live.close", [this](const json& p) {
    fields(p, {"account"});
    live.erase(text(p, "account"));
    return snapshot();
  });
}
} // namespace asterion::terminal
