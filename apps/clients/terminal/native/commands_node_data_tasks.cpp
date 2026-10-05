#include "application_impl.hpp"

namespace asterion::terminal {
void Application::Impl::adopt_data_tasks(std::shared_ptr<TaskClient> next,
                                         std::shared_ptr<DataClient> next_data) {
  if (!data_client || !same_service_endpoint(data_client->endpoint(), next_data->endpoint())) {
    selections.clear();
    dataset_series.clear();
    ++dataset_selection_generation;
    history_contracts = std::make_shared<const std::vector<HistoryListing>>();
    history_source.clear();
    history_exchange.clear();
    history_product.clear();
    history_cutoff = 0;
  }
  task_client = std::move(next);
  data_client = std::move(next_data);
  task_result = nullptr;
  ++data_task_generation;
}
void Application::Impl::register_data_task_connection_commands() {
  command("node.data_tasks.local.open", [this](const json& p) -> PolledTask<Response> {
    fields(p, {});
    const auto generation = data_task_generation;
    const auto current = task_client ? task_client->endpoint() : ServiceEndpoint{};
    const auto service = current.endpoint.empty() ? std::string{} : current.session;
    auto [node, next, next_data] =
        (co_await manage<std::tuple<std::shared_ptr<NodeClient>, std::shared_ptr<TaskClient>,
                                    std::shared_ptr<DataClient>>>(
            [this, existing = existing_local_node(), service]()
                -> PolledTask<std::tuple<std::shared_ptr<NodeClient>, std::shared_ptr<TaskClient>,
                                         std::shared_ptr<DataClient>>> {
              auto node = (co_await local_node_client(existing));
              ServiceEndpoint address;
              if (service.empty()) {
                address = co_await PollFuture{node->local_data_tasks()};
              } else {
                const auto selected = co_await PollFuture{node->data_task_endpoints(service)};
                co_await PollFuture{node->action(selected.data.session, "start")};
                co_await PollFuture{node->action(selected.task.session, "start")};
                address = selected.task;
              }
              const auto pair = (co_await PollFuture{node->data_task_endpoints(address.session)});
              auto client = (co_await PollFuture{TaskClient::open(service_io, pair.task)});
              auto data = (co_await PollFuture{DataClient::open(service_io, pair.data)});
              co_return std::tuple{std::move(node), std::move(client), std::move(data)};
            }));
    if (generation != data_task_generation)
      throw Error(ErrorCode::conflict,
                  "data/task service selection changed during operation; inspect the "
                  "original services before retrying");
    nodes.try_emplace("local", std::move(node));
    adopt_data_tasks(std::move(next), std::move(next_data));
    co_return snapshot();
  });
  command("node.data_tasks.attach", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"id", "service"});
    const auto generation = data_task_generation;
    const auto node = nodes.at(text(p, "id"));
    const auto service = text(p, "service");
    auto [next, next_data] =
        (co_await manage<std::pair<std::shared_ptr<TaskClient>, std::shared_ptr<DataClient>>>(
            [&]()
                -> PolledTask<std::pair<std::shared_ptr<TaskClient>, std::shared_ptr<DataClient>>> {
              const auto pair = (co_await PollFuture{node->data_task_endpoints(service)});
              co_return std::pair{(co_await PollFuture{TaskClient::open(service_io, pair.task)}),
                                  (co_await PollFuture{DataClient::open(service_io, pair.data)})};
            }));
    if (generation != data_task_generation)
      throw Error(ErrorCode::conflict,
                  "data/task service selection changed during operation; inspect the "
                  "original services before retrying");
    adopt_data_tasks(std::move(next), std::move(next_data));
    co_return snapshot();
  });
}
} // namespace asterion::terminal
