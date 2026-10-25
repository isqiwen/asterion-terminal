#include "application_impl.hpp"

namespace asterion::terminal {

void Application::Impl::adopt_research(std::shared_ptr<ResearchClient> next)
{
    research_client = std::move(next);
    task_result = nullptr;
    ++data_task_generation;
}

void Application::Impl::adopt_data(std::shared_ptr<DataClient> next)
{
    if (!data_client || !same_service_endpoint(data_client->endpoint(), next->endpoint())) {
        selections.clear();
        dataset_series.clear();
        ++dataset_selection_generation;
    }
    data_client = std::move(next);
    task_result = nullptr;
    ++data_task_generation;
}

void Application::Impl::register_data_task_connection_commands()
{
    command("node.data.local.open", [this](const json& p) -> PolledTask<Response> {
        fields(p, {});
        const auto generation = data_task_generation;
        const auto node = co_await local_node_client();
        ServiceEndpoint address;
        if (data_client && data_client->endpoint().node_id == "local") {
            const auto service = data_client->endpoint().session;
            co_await PollFuture{node->action(service, "start")};
            address = co_await PollFuture{node->service_endpoint(service, node::proto::DATA)};
        } else
            address = co_await PollFuture{node->local_data()};
        auto client = co_await PollFuture{DataClient::open(service_io, address)};
        if (generation != data_task_generation)
            throw Error(
                ErrorCode::conflict,
                "task owner selection changed; inspect the original service");
        adopt_data(std::move(client));
        co_return snapshot();
    });
    command("node.data.attach", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"id", "service"});
        const auto generation = data_task_generation;
        const auto node = manager.node(text(p, "id"));
        const auto address =
            co_await PollFuture{node->service_endpoint(text(p, "service"), node::proto::DATA)};
        auto client = co_await PollFuture{DataClient::open(service_io, address)};
        if (generation != data_task_generation)
            throw Error(
                ErrorCode::conflict,
                "task owner selection changed; inspect the original service");
        adopt_data(std::move(client));
        co_return snapshot();
    });
    command("node.research.local.open", [this](const json& p) -> PolledTask<Response> {
        fields(p, {});
        const auto generation = data_task_generation;
        const auto node = co_await local_node_client();
        ServiceEndpoint address;
        if (research_client && research_client->endpoint().node_id == "local") {
            const auto service = research_client->endpoint().session;
            co_await PollFuture{node->action(service, "start")};
            address = co_await PollFuture{node->service_endpoint(service, node::proto::RESEARCH)};
        } else
            address = co_await PollFuture{node->local_research()};
        auto client = co_await PollFuture{ResearchClient::open(service_io, address)};
        if (generation != data_task_generation)
            throw Error(ErrorCode::conflict, "research owner selection changed");
        adopt_research(std::move(client));
        co_return snapshot();
    });
    command("node.research.attach", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"id", "service"});
        const auto generation = data_task_generation;
        const auto node = manager.node(text(p, "id"));
        const auto address =
            co_await PollFuture{node->service_endpoint(text(p, "service"), node::proto::RESEARCH)};
        auto client = co_await PollFuture{ResearchClient::open(service_io, address)};
        if (generation != data_task_generation)
            throw Error(ErrorCode::conflict, "research owner selection changed");
        adopt_research(std::move(client));
        co_return snapshot();
    });
}

} // namespace asterion::terminal
