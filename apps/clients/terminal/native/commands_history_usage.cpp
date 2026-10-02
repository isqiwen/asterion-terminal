#include "application_impl.hpp"
#include "task_store.hpp"

namespace asterion::terminal {
// References to one historical dataset version: the current research service,
// other research services (running, or stopped local ledgers read in place)
// and this window's selection. Read-only; the service I/O runs outside the
// operation lock and its inputs are checked unchanged afterwards.
json Application::Impl::history_usage(const json& params) {
  const auto client = research;
  const auto generation = research_generation.load();
  const auto selection_generation = dataset_selection_generation;
  const auto id = text(params, "id");
  bool market_reference = false, settlement_reference = false;
  for (const auto& selection : selections) {
    const auto& data = selection.request;
    market_reference |=
        std::ranges::find(data.source_dataset_ids(), id) != data.source_dataset_ids().end();
    settlement_reference |=
        std::ranges::find(data.settlement_dataset_ids(), id) != data.settlement_dataset_ids().end();
  }
  const auto research_address = client->endpoint();
  const auto inspected_nodes = nodes;
  auto report = outside_lock([&] {
    const auto registered = registered_node_inventory();
    auto usage = client->history_usage(id);
    usage["disconnected_nodes"] = registered;
    auto& disconnected = usage["disconnected_nodes"]["names"];
    std::erase_if(disconnected.get_ref<json::array_t&>(), [&](const auto& name) {
      return inspected_nodes.contains(name.template get<std::string>());
    });
    usage["other_research"] = json::array();
    std::vector<ServiceEndpoint> inspected_research{research_address};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    for (const auto& [name, node] : inspected_nodes) {
      const bool local = name == "local";
      try {
        if (std::chrono::steady_clock::now() >= deadline)
          throw std::runtime_error("historical service inspection timed out");
        const auto inventory = node->history_inventory();
        for (const auto& service : inventory) {
          if (std::ranges::any_of(inspected_research, [&](const auto& address) {
                return same_service_endpoint(address, service.address);
              }))
            continue;
          inspected_research.push_back(service.address);
          json group{{"node", name},
                     {"service", service.address.session},
                     {"checked", false},
                     {"references", json::array()}};
          try {
            if (local && service.state == "stopped") {
              const auto expected =
                  local_node_directory() / "services" / service.address.session / "ledger";
              // Only the current environment's managed layout is eligible for disk inspection.
              for (const auto& path : {local_node_directory(), expected.parent_path().parent_path(),
                                       expected.parent_path(), expected})
                if (std::filesystem::is_symlink(path))
                  throw std::invalid_argument("invalid local research ledger directory");
              if (std::filesystem::path(service.directory).lexically_normal() !=
                  expected.lexically_normal())
                throw std::invalid_argument("invalid local research ledger directory");
              const auto result = tasks::Store::inspect_history_usage(expected, id);
              group["references"] = protocol::decode_history_usage(result).at("references");
              group["checked"] = true;
              group["stopped"] = true;
              usage["other_research"].push_back(std::move(group));
              continue;
            }
            if (service.state != "running")
              throw std::runtime_error(
                  "research service is not running; references were not inspected");
            // A local inventory must never become a TCP connection through an empty socket.
            if (local && service.address.endpoint.empty())
              throw std::invalid_argument("invalid historical service address");
            const auto result = ResearchClient::inspect_history_usage(
                service.address, id,
                std::min(deadline, std::chrono::steady_clock::now() + std::chrono::seconds(5)));
            group["references"] = result.at("references");
            group["checked"] = true;
          } catch (const std::exception& e) {
            group["error"] = e.what();
          }
          usage["other_research"].push_back(std::move(group));
        }
      } catch (const std::exception& e) {
        usage["other_research"].push_back({{"node", name},
                                           {"service", ""},
                                           {"checked", false},
                                           {"references", json::array()},
                                           {"error", e.what()}});
      }
    }
    if (registered_node_inventory() != registered)
      throw Error(ErrorCode::conflict, "registered nodes changed during archive query");
    return usage;
  });
  if (nodes != inspected_nodes)
    throw Error(ErrorCode::conflict, "node connections changed during archive query");
  if (generation != research_generation.load())
    throw Error(ErrorCode::conflict, "research service changed during archive query");
  if (selection_generation != dataset_selection_generation)
    throw Error(ErrorCode::conflict, "dataset selection changed during resolution");
  report["selected_roles"] = json::array();
  if (market_reference)
    report["selected_roles"].push_back("market");
  if (settlement_reference)
    report["selected_roles"].push_back("settlement");
  auto result = snapshot();
  result["history_usage"] = std::move(report);
  return result;
}
} // namespace asterion::terminal
