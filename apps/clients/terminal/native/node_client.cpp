#include "node_client.hpp"
#include "local_manager.hpp"
#include "node_service.hpp"
#include "application_environment.hpp"
#include "manager_wire.hpp"
#include <asterion/kernel/trace.hpp>
#include <asterion/protocol/trading.hpp>

namespace asterion::terminal {
namespace wire = manager::proto;
using namespace std::chrono_literals;

struct ManagerClient::Impl : std::enable_shared_from_this<Impl>
{
    ServiceIo& io;
    std::unique_ptr<ipc::RpcClient> transport;
    std::string endpoint;
    std::vector<NodeSnapshot> nodes;
    std::uint64_t pid = 0;
    // The latest view applied; an older one arriving late is ignored.
    std::uint64_t sequence = 0;
    std::stop_source lifetime;
    bool starting = false;

    explicit Impl(ServiceIo& owner)
        : io(owner)
    {}

    PolledTask<wire::Response> exchange(wire::Request request, std::stop_token stop)
    {
        request.set_version(1);
        request.set_correlation_id(next_correlation_id());
        auto pending =
            transport->request(request.SerializeAsString(), request.has_snapshot() ? 5s : 120s);
        while (pending.wait_for(0ms) != std::future_status::ready) {
            if (stop.stop_requested() || lifetime.stop_requested())
                throw Error(ErrorCode::cancelled, "node manager connection closed");
            transport->poll();
            co_await std::suspend_always{};
        }
        wire::Response response;
        if (!response.ParseFromString(*pending.get()) || response.version() != 1
            || response.correlation_id() != request.correlation_id())
            throw Error(ErrorCode::unavailable, "invalid node manager response");
        if (response.has_snapshot() || response.has_view()) {
            const auto& snapshot = response.has_snapshot() ? response.snapshot() : response.view();
            if (snapshot.pid() != pid || snapshot.sequence() > sequence) {
                pid = snapshot.pid();
                sequence = snapshot.sequence();
                nodes.clear();
                for (const auto& node : snapshot.nodes())
                    nodes.push_back(decode(node));
            }
        }
        if (response.has_error())
            throw_remote_error(response.error().code(), response.error().message());
        co_return response;
    }

    PolledTask<void> monitor(std::stop_token stop)
    {
        while (!stop.stop_requested() && !lifetime.stop_requested()) {
            const auto next = std::chrono::steady_clock::now() + 2s;
            co_await PollUntil{[&] {
                return stop.stop_requested() || lifetime.stop_requested()
                    || std::chrono::steady_clock::now() >= next;
            }};
            if (stop.stop_requested() || lifetime.stop_requested())
                break;
            try {
                wire::Request request;
                request.mutable_snapshot();
                co_await exchange(std::move(request), stop);
            } catch (const std::exception& error) {
                for (auto& node : nodes) {
                    node.online = false;
                    node.error = error.what();
                }
            }
        }
    }

    PolledTask<wire::Response> call(wire::Request request, std::stop_token stop)
    {
        co_await PollUntil{[&] {
            return !starting || stop.stop_requested();
        }};
        if (!transport) {
            starting = true;

            struct Done
            {
                bool& flag;

                ~Done()
                {
                    flag = false;
                }
            } done{starting};

            endpoint = co_await io.admin<std::string>([] {
                return local_manager();
            });
            transport = std::make_unique<ipc::RpcClient>(
                io.reactor(),
                endpoint,
                8,
                io.payload_budget(ServiceIo::PayloadLane::administration),
                4 * 1024 * 1024);
            (void)io.submit<void>(
                [state = shared_from_this()](std::stop_token token) {
                    return state->monitor(token);
                },
                ServiceIo::Lane::observation);
        }
        co_return co_await exchange(std::move(request), stop);
    }
};

ManagerClient::ManagerClient(ServiceIo& io)
    : io_(io),
      impl_(std::make_shared<Impl>(io))
{}

ManagerClient::~ManagerClient()
{
    impl_->lifetime.request_stop();
}

std::future<wire::Response> ManagerClient::request(wire::Request request)
{
    return io_.submit<wire::Response>(
        [state = impl_, request = std::move(request)](std::stop_token stop) mutable {
            return state->call(std::move(request), stop);
        });
}

std::shared_ptr<NodeClient> ManagerClient::node(std::string id)
{
    return std::make_shared<NodeClient>(*this, std::move(id));
}

const std::vector<NodeSnapshot>& ManagerClient::view() const
{
    return impl_->nodes;
}

std::future<Json> ManagerClient::inventory()
{
    return io_.submit<Json>([state = impl_](std::stop_token stop) -> PolledTask<Json> {
        wire::Request request;
        request.mutable_inventory();
        auto response = co_await state->call(std::move(request), stop);
        co_return Json::parse(response.presentation());
    });
}

std::future<void> ManagerClient::shutdown(bool recover)
{
    return io_.submit<void>([state = impl_, recover](std::stop_token stop) -> PolledTask<void> {
        if (!development_environment() || environment_variable("ASTERION_ROOT"))
            throw Error(
                ErrorCode::permission_denied,
                "automatic shutdown requires the managed development environment");
        const auto registered = co_await state->io.admin<bool>([] {
            return std::filesystem::exists(node_service_definition("me.asterion.node-manager.dev"))
                || std::filesystem::exists(node_service_definition("me.asterion.node-agent.dev"));
        });
        if (!registered)
            co_return;
        wire::Request request;
        request.set_shutdown_development(recover);
        co_await state->call(std::move(request), stop);
        co_await state->io.admin<void>([&] {
            stop_development_manager(state->endpoint, state->pid);
        });
        state->lifetime.request_stop();
    });
}

std::future<ServiceEndpoint> NodeClient::endpoint(wire::NodeCommand command)
{
    command.set_id(id_);
    wire::Request request;
    *request.mutable_node() = std::move(command);
    return manager_.io_.submit<ServiceEndpoint>(
        [state = manager_.impl_, node_id = id_, request = std::move(request)](
            std::stop_token stop) mutable -> PolledTask<ServiceEndpoint> {
            auto response = co_await state->call(std::move(request), stop);
            if (!response.has_endpoint())
                throw Error(ErrorCode::unavailable, "invalid node manager response");
            auto address = decode(response.endpoint());
            address.node_id = node_id;
            co_return address;
        });
}

std::future<ServiceEndpoint>
NodeClient::service_endpoint(const std::string& name, node::proto::ServiceKind kind)
{
    wire::NodeCommand command;
    command.mutable_endpoint()->set_name(name);
    command.mutable_endpoint()->set_kind(kind);
    return endpoint(std::move(command));
}

std::future<ServiceEndpoint> NodeClient::local_session(const std::filesystem::path& directory)
{
    wire::NodeCommand command;
    command.set_local_session(directory.string());
    return endpoint(std::move(command));
}

std::future<ServiceEndpoint> NodeClient::trading_session(const std::string& service)
{
    wire::NodeCommand command;
    command.set_trading_session(service);
    return endpoint(std::move(command));
}

std::future<ServiceEndpoint> NodeClient::local_market()
{
    wire::NodeCommand command;
    command.mutable_local_market();
    return endpoint(std::move(command));
}

std::future<ServiceEndpoint> NodeClient::local_data()
{
    wire::NodeCommand command;
    command.mutable_local_data();
    return endpoint(std::move(command));
}

std::future<ServiceEndpoint> NodeClient::local_research(
    const std::optional<std::vector<std::string>>& plugins)
{
    wire::NodeCommand command;
    if (plugins) {
        auto& selection = *command.mutable_create_research();
        for (const auto& hash : *plugins)
            selection.add_hashes(hash);
    } else
        command.mutable_local_research();
    return endpoint(std::move(command));
}

std::future<std::vector<HistoryService>> NodeClient::history_inventory()
{
    wire::Request request;
    request.mutable_node()->set_id(id_);
    request.mutable_node()->mutable_history();
    return manager_.io_.submit<std::vector<HistoryService>>(
        [state = manager_.impl_, node_id = id_, request = std::move(request)](
            std::stop_token stop) mutable -> PolledTask<std::vector<HistoryService>> {
            auto response = co_await state->call(std::move(request), stop);
            if (!response.has_history())
                throw Error(ErrorCode::unavailable, "invalid node manager response");
            std::vector<HistoryService> result;
            for (const auto& service : response.history().services())
                result.push_back(
                    {service.kind(),
                        service.directory(),
                        service.state(),
                        decode(service.address())});
            co_return result;
        });
}

std::future<void> NodeClient::action(const std::string& service, const std::string& operation)
{
    wire::Request request;
    request.mutable_node()->set_id(id_);
    request.mutable_node()->mutable_action()->set_service(service);
    request.mutable_node()->mutable_action()->set_action(operation);
    return manager_.io_.submit<void>(
        [state = manager_.impl_, node_id = id_, request = std::move(request)](
            std::stop_token stop) mutable -> PolledTask<void> {
            co_await state->call(std::move(request), stop);
        });
}

} // namespace asterion::terminal
