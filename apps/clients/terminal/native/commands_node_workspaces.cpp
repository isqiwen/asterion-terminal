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
        // One local Data: a concurrent open adopts a client of the same service.
        const auto address = co_await PollFuture{services.start(node::proto::DATA)};
        adopt_data(co_await PollFuture{DataClient::open(service_io, address)});
        co_return snapshot();
    });
    command("node.research.local.open", [this](const json& p) -> PolledTask<Response> {
        fields(p, {});
        const auto address = co_await PollFuture{services.start(node::proto::RESEARCH)};
        adopt_research(co_await PollFuture{ResearchClient::open(service_io, address)});
        co_return snapshot();
    });
}

} // namespace asterion::terminal
