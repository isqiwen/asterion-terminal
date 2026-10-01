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
// Live CTP sessions: create, open, connect with credentials, authorize and
// trade. Every order passes the service's authorization, allowlist and risk.
void Application::Impl::register_live_commands() {
  core.access().grant("terminal.local", "live.manage");
  core.command("live.create", "live.manage", [this](const json& p) {
    fields_with_risk(p, {"directory", "front", "broker_id", "user_id", "app_id", "contracts"});
    if (live)
      throw std::invalid_argument("close the current live session first");
    // Validated before the braced initializer (GCC < 13 PR66139 leak).
    const auto risk = risk_parameters(p);
    auto contracts = catalog_terms(market ? market->snapshot() : json(nullptr), p.at("contracts"));
    json manifest{{"version", 1},
                  {"type", "live_ctp"},
                  {"broker",
                   {{"front", text(p, "front")},
                    {"broker_id", text(p, "broker_id")},
                    {"user_id", text(p, "user_id")},
                    {"app_id", text(p, "app_id")}}},
                  {"risk", risk},
                  {"contracts", std::move(contracts)}};
    (void)protocol::encode_live_input(manifest);
    const auto directory = text(p, "directory");
    auto [node, next] = without_operations([&, existing = existing_local_node()] {
      auto node = local_node_client(existing);
      auto client = std::make_unique<TradingClient>(
          std::filesystem::path(std::u8string(directory.begin(), directory.end())),
          TradingMode::live, manifest);
      return std::pair{std::move(node), std::move(client)};
    });
    nodes.try_emplace("local", std::move(node));
    if (live)
      throw Error(ErrorCode::conflict, "another window opened a live session meanwhile; "
                                       "recover this directory after closing it");
    live = std::move(next);
    return snapshot();
  });
  core.command("live.open", "live.manage", [this](const json& p) {
    fields(p, {"directory"});
    if (live)
      throw std::invalid_argument("close the current live session first");
    const auto directory = text(p, "directory");
    auto [node, next] = without_operations([&, existing = existing_local_node()] {
      auto node = local_node_client(existing);
      auto client = std::make_unique<TradingClient>(
          std::filesystem::path(std::u8string(directory.begin(), directory.end())),
          TradingMode::live);
      return std::pair{std::move(node), std::move(client)};
    });
    nodes.try_emplace("local", std::move(node));
    if (live)
      throw Error(ErrorCode::conflict, "another window opened a live session meanwhile; "
                                       "recover this directory after closing it");
    live = std::move(next);
    return snapshot();
  });
  // Credentials go to the session service and are not kept by the Terminal.
  core.command("live.connect", "live.manage", [this](const json& p) {
    fields(p, {"password", "auth_code"});
    if (!live)
      throw std::invalid_argument("create or recover a live session first");
    live->connect_broker(text(p, "password"), text(p, "auth_code"));
    return snapshot();
  });
  core.command("live.disconnect", "live.manage", [this](const json& p) {
    fields(p, {});
    if (!live)
      throw std::invalid_argument("create or recover a live session first");
    live->disconnect_broker();
    return snapshot();
  });
  core.command("live.act", "live.manage", [this](const json& p) {
    if (!live)
      throw std::invalid_argument("create or recover a live session first");
    live->execute(p);
    return snapshot();
  });
  core.command("live.close", "live.manage", [this](const json& p) {
    fields(p, {});
    live.reset();
    return snapshot();
  });
}
} // namespace asterion::terminal
