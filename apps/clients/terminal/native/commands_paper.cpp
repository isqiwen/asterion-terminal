#include "application_impl.hpp"
#include <stdexcept>

namespace asterion::terminal {
// Paper trading sessions: create, connect, recover and act on an account.
void Application::Impl::register_paper_commands() {
  core.access().grant("terminal.local", "paper.manage");
  core.command("paper.create", "paper.manage", [this](const json& p) {
    const bool remote = paper && paper->connection().at("transport") == "tcp_tls";
    if (remote)
      fields_with_risk(p, {"deposit", "contracts"});
    else
      fields_with_risk(p, {"directory", "deposit", "contracts"});
    if (paper && !remote)
      throw std::invalid_argument("close the current paper session first");
    const auto costs = selection_costs(p.at("contracts"));
    // Validated before the braced initializer (GCC < 13 PR66139 leak).
    const auto risk = risk_parameters(p);
    const auto deposit = text(p, "deposit");
    std::size_t bars = 0;
    json contracts = json::array();
    for (std::size_t i = 0; i < selected().size(); ++i) {
      bars += static_cast<std::size_t>(selected()[i].dataset.bars_size());
      contracts.push_back(
          {{"dataset", protocol::decode_bar_dataset(selected()[i].dataset)}, {"costs", costs[i]}});
    }
    if (bars > protocol::max_session_bars)
      throw std::invalid_argument("paper sessions use at most 20000 bars; narrow the trading days");
    json manifest{{"version", 3},
                  {"type", "historical_paper"},
                  {"deposit", deposit},
                  {"risk", risk},
                  {"contracts", std::move(contracts)}};
    if (remote) {
      paper->create(manifest);
      return snapshot();
    }
    // Starting the session service waits for the Agent; other commands proceed.
    const auto directory = text(p, "directory");
    auto [node, next] = without_operations([&, existing = existing_local_node()] {
      auto node = local_node_client(existing);
      auto client = std::make_unique<TradingClient>(
          std::filesystem::path(std::u8string(directory.begin(), directory.end())),
          TradingMode::paper, manifest);
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
    const auto mode = text(p, "mode");
    if (mode != "paper" && mode != "live")
      throw std::invalid_argument("connection mode must be paper or live");
    auto& client = mode == "live" ? live : paper;
    if (client)
      throw std::invalid_argument("disconnect the current trading connection first");
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
    client = std::make_unique<TradingClient>(config, mode == "live" ? TradingMode::live
                                                                    : TradingMode::paper);
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
          std::filesystem::path(std::u8string(directory.begin(), directory.end())),
          TradingMode::paper);
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
