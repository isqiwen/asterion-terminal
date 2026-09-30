#include "application_impl.hpp"
#include <stdexcept>

namespace asterion::terminal {
// Read-only live market service.
void Application::Impl::register_market_commands() {
  core.command("market.local", "node.manage", [this](const json& p) {
    fields(p, {});
    if (!nodes.contains("local"))
      nodes.emplace("local", std::make_unique<NodeClient>(local_node()));
    if (!market)
      market = std::make_unique<MarketClient>(nodes.at("local")->local_market());
    return snapshot();
  });
  core.command("market.attach", "node.manage", [this](const json& p) {
    fields(p, {"id", "service"});
    auto next = std::make_unique<MarketClient>(
        nodes.at(text(p, "id"))
            ->service_endpoint(text(p, "service"), asterion::node::v1::MARKET_DATA));
    market = std::move(next);
    return snapshot();
  });
  core.command("market.connect", "node.manage", [this](const json& p) {
    fields(p, {"front", "broker", "user", "password", "instruments"});
    if (!market)
      throw std::invalid_argument("start or select a market service first");
    market->connect(p);
    return snapshot();
  });
  core.command("market.subscribe", "node.manage", [this](const json& p) {
    fields(p, {"instruments"});
    if (!market)
      throw std::invalid_argument("select a market service first");
    market->subscribe(p.at("instruments"));
    return snapshot();
  });
  core.command("market.catalog", "node.manage", [this](const json& p) {
    fields(p, {"front", "broker", "user", "password", "app_id", "auth_code"});
    if (!market)
      throw std::invalid_argument("select a market service first");
    market->catalog(p);
    return snapshot();
  });
  // Read-only: the market service I/O runs outside the operation lock in dispatch.
  core.command("market.minutes", "node.manage", [this](const json& p) {
    fields(p, {"venue", "symbol"});
    InstrumentId{text(p, "venue"), text(p, "symbol")}.validate();
    if (!market)
      throw std::invalid_argument("select a market service first");
    auto published = read_published(json::object());
    return published.is_null() ? snapshot() : published;
  });
  core.command("market.disconnect", "node.manage", [this](const json& p) {
    fields(p, {});
    if (market)
      market->disconnect();
    return snapshot();
  });
}
} // namespace asterion::terminal
