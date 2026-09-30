#include "application_impl.hpp"
#include <stdexcept>

namespace asterion::terminal {
// Paper trading sessions: create, connect, recover and act on an account.
void Application::Impl::register_paper_commands() {
  core.access().grant("terminal.local", "paper.manage");
  core.command("paper.create", "paper.manage", [this](const json& p) {
    const bool remote = paper && paper->connection().at("transport") == "tcp_tls";
    if (remote)
      fields_with_costs(
          p, {"deposit", "max_order_quantity", "max_gross_quantity", "max_working_orders"});
    else
      fields_with_costs(p, {"directory", "deposit", "max_order_quantity", "max_gross_quantity",
                            "max_working_orders"});
    if (paper && !remote)
      throw std::invalid_argument("close the current paper session first");
    const auto costs = cost_parameters(p);
    // Validated before the braced initializer (GCC < 13 PR66139 leak).
    const auto risk = risk_parameters(p);
    const auto deposit = text(p, "deposit");
    if (static_cast<std::size_t>(selected().dataset.bars_size()) > protocol::max_session_bars)
      throw std::invalid_argument("paper sessions use at most 20000 bars; narrow the trading days");
    auto dataset = protocol::decode_bar_dataset(selected().dataset);
    json manifest{{"version", 2}, {"type", "historical_paper"}, {"costs", costs},
                  {"risk", risk}, {"deposit", deposit},         {"dataset", std::move(dataset)}};
    // Every dataset carries data-source settlement prices, so each trading
    // day ends with its bound settlement instead of a typed price.
    const json schedule{{"request_id", "replay-days"}, {"action", "replay_days"}};
    if (remote) {
      paper->create(manifest);
      paper->execute(schedule);
      return snapshot();
    }
    // Starting the session service waits for the Agent; other commands proceed.
    const auto directory = text(p, "directory");
    auto [node, next] = without_operations([&, existing = existing_local_node()] {
      auto node = local_node_client(existing);
      auto client = std::make_unique<TradingClient>(
          std::filesystem::path(std::u8string(directory.begin(), directory.end())), manifest);
      client->execute(schedule);
      return std::pair{std::move(node), std::move(client)};
    });
    nodes.try_emplace("local", std::move(node));
    if (paper)
      throw Error(ErrorCode::conflict, "another window opened a paper session meanwhile; "
                                       "recover this directory after closing it");
    paper = std::move(next);
    return snapshot();
  });
  core.command("paper.connect", "paper.manage", [this](const json& p) {
    fields(p,
           {"host", "port", "session", "mode", "ca_file", "certificate_file", "private_key_file"});
    if (paper)
      throw std::invalid_argument("disconnect the current trading connection first");
    if (text(p, "mode") != "paper")
      throw std::invalid_argument("live trading is not available");
    const auto port_text = text(p, "port");
    unsigned int port = 0;
    const auto [end, ec] =
        std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
    if (ec != std::errc{} || end != port_text.data() + port_text.size() || !port || port > 65535)
      throw std::invalid_argument("port must be between 1 and 65535");
    ServiceEndpoint config{
        text(p, "host"),
        text(p, "session"),
        static_cast<std::uint16_t>(port),
        {text(p, "ca_file"), text(p, "certificate_file"), text(p, "private_key_file")}};
    paper = std::make_unique<TradingClient>(config);
    return snapshot();
  });
  core.command("paper.reconnect", "paper.manage", [this](const json& p) {
    fields(p, {});
    if (!paper)
      throw std::invalid_argument("choose a connection profile first");
    paper->reconnect();
    return snapshot();
  });
  core.command("paper.open", "paper.manage", [this](const json& p) {
    fields(p, {"directory"});
    if (paper)
      throw std::invalid_argument("close the current paper session first");
    const auto directory = text(p, "directory");
    auto [node, next] = without_operations([&, existing = existing_local_node()] {
      auto node = local_node_client(existing);
      auto client = std::make_unique<TradingClient>(
          std::filesystem::path(std::u8string(directory.begin(), directory.end())));
      return std::pair{std::move(node), std::move(client)};
    });
    nodes.try_emplace("local", std::move(node));
    if (paper)
      throw Error(ErrorCode::conflict, "another window opened a paper session meanwhile; "
                                       "recover this directory after closing it");
    paper = std::move(next);
    return snapshot();
  });
  core.command("paper.close", "paper.manage", [this](const json& p) {
    fields(p, {});
    paper.reset();
    return snapshot();
  });
  core.command("paper.act", "paper.manage", [this](const json& p) {
    if (!paper)
      throw std::invalid_argument("create or recover a paper session first");
    paper->execute(p);
    return snapshot();
  });
}
} // namespace asterion::terminal
