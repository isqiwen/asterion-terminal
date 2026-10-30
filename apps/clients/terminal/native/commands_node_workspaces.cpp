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
    // The one local Data and Research service; started when absent or stopped.
    command("node.data.local.open", [this](const json& p) -> PolledTask<Response> {
        fields(p, {});
        // One local Data. Opens that overlap adopt one client: a later one keeps
        // the connection the first adopted, so pages keyed to it do not reset.
        const auto generation = data_task_generation;
        const auto address = co_await PollFuture{services.start(node::proto::DATA)};
        auto client = co_await PollFuture{DataClient::open(service_io, address)};
        if (generation == data_task_generation)
            adopt_data(std::move(client));
        co_return snapshot();
    });
    command("node.research.local.open", [this](const json& p) -> PolledTask<Response> {
        fields(p, {});
        const auto generation = data_task_generation;
        const auto address = co_await PollFuture{services.start(node::proto::RESEARCH)};
        auto client = co_await PollFuture{ResearchClient::open(service_io, address)};
        if (generation == data_task_generation)
            adopt_research(std::move(client));
        co_return snapshot();
    });
}

} // namespace asterion::terminal
