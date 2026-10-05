#include "application_impl.hpp"
#include "task_store.hpp"

namespace asterion::terminal {
// References to one historical dataset version: the selected Data/Task services,
// other Data/Task services (running, or stopped local task ledgers read in place)
// and this window's selection. Read-only; requests suspend and the owner checks
// their captured identities before applying the report.
PolledTask<Application::Impl::Response> Application::Impl::history_usage(const json& params) {
  const auto client = task_client;
  const auto warehouse = data_client;
  const auto generation = data_task_generation;
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
  const auto task_address = client->endpoint();
  const auto inspected_nodes = nodes;
  auto report = (co_await run<json>([&]() -> PolledTask<json> {
    const auto registered =
        (co_await service_io.admin<json>([&] { return registered_node_inventory(); }));
    auto usage = (co_await PollFuture{client->history_usage(id)});
    const auto data_usage = (co_await PollFuture{warehouse->history_usage(id)});
    for (const auto& reference : data_usage.at("references"))
      usage["references"].push_back(reference);
    usage["disconnected_nodes"] = registered;
    auto& disconnected = usage["disconnected_nodes"]["names"];
    std::erase_if(disconnected.get_ref<json::array_t&>(), [&](const auto& name) {
      return inspected_nodes.contains(name.template get<std::string>());
    });
    usage["other_data_services"] = json::array();
    std::vector<ServiceEndpoint> inspected_services{task_address, warehouse->endpoint()};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    for (const auto& [name, node] : inspected_nodes) {
      const bool local = name == "local";
      try {
        if (std::chrono::steady_clock::now() >= deadline)
          throw std::runtime_error("historical service inspection timed out");
        const auto inventory = (co_await PollFuture{node->history_inventory()});
        for (const auto& service : inventory) {
          if (std::ranges::any_of(inspected_services, [&](const auto& address) {
                return same_service_endpoint(address, service.address);
              }))
            continue;
          inspected_services.push_back(service.address);
          json group{{"node", name},
                     {"service", service.address.session},
                     {"checked", false},
                     {"references", json::array()}};
          try {
            if (local && service.kind == node::v1::TASK_SERVICE && service.state == "stopped") {
              group["references"] = co_await service_io.read<json>([&] {
                const auto expected =
                    local_node_directory() / "services" / service.address.session / "ledger";
                // Only the current environment's managed layout is eligible for disk inspection.
                for (const auto& path :
                     {local_node_directory(), expected.parent_path().parent_path(),
                      expected.parent_path(), expected})
                  if (std::filesystem::is_symlink(path))
                    throw std::invalid_argument("invalid local task ledger directory");
                if (std::filesystem::path(service.directory).lexically_normal() !=
                    expected.lexically_normal())
                  throw std::invalid_argument("invalid local task ledger directory");
                const auto result = tasks::Store::inspect_history_usage(
                    expected, {service.address.session, service.data_service}, id);
                return protocol::decode_history_usage(result).at("references");
              });
              group["checked"] = true;
              group["stopped"] = true;
              usage["other_data_services"].push_back(std::move(group));
              continue;
            }
            if (service.state != "running")
              throw std::runtime_error(
                  "data/task service is not running; references were not inspected");
            // A local inventory must never become a TCP connection through an empty socket.
            if (local && service.address.endpoint.empty())
              throw std::invalid_argument("invalid historical service address");
            const auto until =
                std::min(deadline, std::chrono::steady_clock::now() + std::chrono::seconds(5));
            const auto result = service.kind == node::v1::DATA_SERVICE
                                    ? (co_await PollFuture{DataClient::inspect_history_usage(
                                          service_io, service.address, id, until)})
                                    : (co_await PollFuture{TaskClient::inspect_history_usage(
                                          service_io, service.address, id, until)});
            group["references"] = result.at("references");
            group["checked"] = true;
          } catch (const std::exception& e) {
            group["error"] = e.what();
          }
          usage["other_data_services"].push_back(std::move(group));
        }
      } catch (const std::exception& e) {
        usage["other_data_services"].push_back({{"node", name},
                                                {"service", ""},
                                                {"checked", false},
                                                {"references", json::array()},
                                                {"error", e.what()}});
      }
    }
    if ((co_await service_io.admin<json>([&] { return registered_node_inventory(); })) !=
        registered)
      throw Error(ErrorCode::conflict, "registered nodes changed during archive query");
    co_return usage;
  }));
  if (nodes != inspected_nodes)
    throw Error(ErrorCode::conflict, "node connections changed during archive query");
  if (generation != data_task_generation)
    throw Error(ErrorCode::conflict, "data/task service selection changed during archive query");
  if (selection_generation != dataset_selection_generation)
    throw Error(ErrorCode::conflict, "dataset selection changed during resolution");
  report["selected_roles"] = json::array();
  if (market_reference)
    report["selected_roles"].push_back("market");
  if (settlement_reference)
    report["selected_roles"].push_back("settlement");
  auto result = snapshot();
  result.values["history_usage"] = std::move(report);
  co_return result;
}
} // namespace asterion::terminal
