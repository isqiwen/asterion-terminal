#include "manager.hpp"
#include <asterion/foundation/serialization.hpp>
#include "agent_client.hpp"
#include "node_enrollment.hpp"
#include "manager_wire.hpp"
#include <asterion/kernel/process/owner.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/trace.hpp>
#include <asterion/protocol/rpc_diagnostics.hpp>
#include <asterion/protocol/trading.hpp>
#include <algorithm>

namespace asterion::terminal {

using namespace std::chrono_literals;
namespace wire = manager::proto;

namespace {

// Deploy business services independently; inputs select their source owners per operation.
PolledTask<void>
deploy_service(ServiceIo& io, AgentClient& node, bool local, const manager::proto::Deployment& p)
{
    const auto status = co_await PollFuture{node.inspect_status()};
    if (!status.online || !status.health)
        throw std::invalid_argument("connect the node before deployment");
    const auto kind = p.kind();
    if (local
        && (p.port() != 0
            || (kind != node::proto::RESEARCH && kind != node::proto::DATA
                && kind != node::proto::TRADING)))
        throw std::invalid_argument("local deployment requires a business service and port zero");
    if (!local
        && (status.health->os() != "linux" || status.health->version() != ASTERION_PRODUCT_VERSION))
        throw std::invalid_argument("remote node requires the current Linux service programs");
    const auto programs = co_await io.admin<ServicePrograms>([&] {
        return local ? local_service_programs(kind)
                     : bundled_service_programs(status.health->arch(), kind);
    });
    const ServiceDeployment deployment{
        .service = p.service(),
        .kind = kind,
        .platform = {status.health->os(), status.health->arch()},
        .programs = programs,
        .port = local ? std::uint16_t{0} : static_cast<std::uint16_t>(p.port())
    };
    co_await PollFuture{node.deploy(deployment)};
}

// Replaces a stopped service's programs with this Terminal's build.
PolledTask<void>
update_service(ServiceIo& io, AgentClient& node, bool local, const manager::proto::Update& p)
{
    const auto service = p.service();
    const auto state = (co_await PollFuture{node.inspect_status()});
    if (!state.online || !state.health)
        throw std::invalid_argument("connect the node before updating");
    const auto& health = *state.health;
    auto kind = node::proto::UNSPECIFIED_SERVICE;
    for (const auto& service_status : health.services())
        if (service_status.id() == service)
            kind = service_status.kind();
    auto programs = co_await io.admin<ServicePrograms>([&] {
        return local ? local_service_programs(kind) : bundled_service_programs(health.arch(), kind);
    });
    const ServiceUpdate update{
        .service = service,
        .expected_revision = p.revision(),
        .platform = {.os = health.os(), .arch = health.arch()},
        .programs = std::move(programs)
    };
    co_await PollFuture{node.update(update)};
}

Json ssh_parameters(const wire::Ssh& p)
{
    return {
        {"id", p.id()},
        {"host", p.host()},
        {"ssh_port", p.ssh_port()},
        {"username", p.username()},
        {"key_source", p.key_source()},
        {"private_key", p.private_key()},
        {"known_hosts", p.known_hosts()},
        {"agent_port", p.agent_port()},
        {"firewall_port", p.firewall_port()},
        {"firewall_action", p.firewall_action()}
    };
}

} // namespace

struct NodeManager::Impl
{
    ServiceIo io;
    std::map<std::string, std::shared_ptr<AgentClient>> nodes;
    Json firewall_plan = nullptr, firewall_parameters = nullptr;
    std::chrono::steady_clock::time_point firewall_expiry{};
    bool busy = false;
    std::uint64_t snapshots = 0;
    std::shared_future<void> initialization;

    ~Impl()
    {
        if (initialization.valid())
            initialization.wait();
    }

    PolledTask<void> restore()
    {
        const auto local = co_await io.admin<std::optional<NodeEndpoint>>([] {
            return enrolled_local_node();
        });
        if (local) {
            auto client = co_await PollFuture{AgentClient::open(io, *local, false)};
            nodes.emplace("local", std::move(client));
        }
        auto inventory = co_await io.admin<Json>([] {
            return registered_node_inventory();
        });
        if (remote_maintenance_pending())
            co_return;
        for (const auto& name : inventory.at("names")) {
            const auto id = name.get<std::string>();
            try {
                auto endpoint = co_await io.admin<NodeEndpoint>([&] {
                    return enrolled_node(id);
                });
                auto client =
                    co_await PollFuture{AgentClient::open(io, std::move(endpoint), false)};
                nodes.emplace(id, std::move(client));
            } catch (const std::exception& error) {
                log::warn("node {} could not be restored: {}", id, error.what());
            }
        }
    }

    wire::Snapshot snapshot()
    {
        wire::Snapshot result;
        result.set_pid(current_process_id());
        result.set_sequence(++snapshots);
        for (const auto& [id, node] : nodes)
            encode(*result.add_nodes(), node->owner_view());
        return result;
    }

    PolledTask<std::shared_ptr<AgentClient>> connect(const std::string& id)
    {
        if (auto found = nodes.find(id);
            found != nodes.end() && (id != "local" || found->second->owner_view().online)) {
            if (id != "local")
                co_await PollFuture{found->second->refresh()};
            co_return found->second;
        }
        auto endpoint = co_await io.admin<NodeEndpoint>([&] {
            return id == "local" ? local_node(io) : enrolled_node(id);
        });
        auto client = co_await PollFuture{AgentClient::open(io, std::move(endpoint))};
        nodes.insert_or_assign(id, client);
        co_return client;
    }

    PolledTask<void> node_command(const wire::NodeCommand& p, wire::Response& result)
    {
        if (remote_maintenance_pending())
            throw Error(
                ErrorCode::conflict,
                "remote maintenance is pending; continue in Installer");
        if (p.has_disconnect()) {
            if (p.id() == "local")
                throw std::invalid_argument("the local node monitor cannot be removed");
            nodes.erase(p.id());
            result.mutable_accepted();
            co_return;
        }
        std::shared_ptr<AgentClient> node;
        if (p.has_connect() || p.has_trading_session() || p.has_local_session()
            || p.has_local_market() || p.has_local_data() || p.has_local_research()
            || p.has_create_research())
            node = co_await connect(p.id());
        else {
            const auto found = nodes.find(p.id());
            if (found == nodes.end())
                throw Error(ErrorCode::not_found, "node is not connected");
            node = found->second;
        }
        switch (p.operation_case()) {
            case wire::NodeCommand::kConnect:
                result.mutable_accepted();
                break;
            case wire::NodeCommand::kHistory: {
                auto inventory = co_await PollFuture{node->history_inventory()};
                auto& history = *result.mutable_history();
                for (const auto& service : inventory) {
                    auto& out = *history.add_services();
                    out.set_kind(service.kind);
                    out.set_directory(service.directory);
                    out.set_state(service.state);
                    encode(*out.mutable_address(), service.address);
                }
                break;
            }
            case wire::NodeCommand::kEndpoint:
                encode(
                    *result.mutable_endpoint(),
                    co_await PollFuture{
                        node->service_endpoint(p.endpoint().name(), p.endpoint().kind())
                    });
                break;
            case wire::NodeCommand::kTradingSession: {
                const auto address =
                    co_await PollFuture{node->trading_session(p.trading_session())};
                encode(*result.mutable_endpoint(), address);
                break;
            }
            case wire::NodeCommand::kLocalSession:
            case wire::NodeCommand::kLocalMarket:
            case wire::NodeCommand::kLocalData:
            case wire::NodeCommand::kLocalResearch:
            case wire::NodeCommand::kCreateResearch: {
                if (p.id() != "local")
                    throw std::invalid_argument("local service operation requires the local node");
                ServiceEndpoint address;
                if (p.has_local_session())
                    address = co_await PollFuture{node->local_session(p.local_session())};
                else if (p.has_local_market())
                    address = co_await PollFuture{node->local_market()};
                else if (p.has_local_data())
                    address = co_await PollFuture{node->local_data()};
                else {
                    std::optional<std::vector<std::string>> plugins;
                    if (p.has_create_research())
                        plugins = std::vector<std::string>{
                            p.create_research().hashes().begin(),
                            p.create_research().hashes().end()
                        };
                    address = co_await PollFuture{node->local_research(plugins)};
                }
                encode(*result.mutable_endpoint(), address);
                break;
            }
            case wire::NodeCommand::kConfigurePlugins: {
                const auto& c = p.configure_plugins();
                const std::vector<std::string> hashes{
                    c.plugins().hashes().begin(),
                    c.plugins().hashes().end()
                };
                co_await PollFuture{node->configure_plugins(c.service(), c.revision(), hashes)};
                result.mutable_accepted();
                break;
            }
            case wire::NodeCommand::kDeploy:
                if (p.deploy().port() > 65535)
                    throw std::invalid_argument("invalid service port");
                co_await deploy_service(io, *node, p.id() == "local", p.deploy());
                result.mutable_accepted();
                break;
            case wire::NodeCommand::kUpdate:
                co_await update_service(io, *node, p.id() == "local", p.update());
                result.mutable_accepted();
                break;
            case wire::NodeCommand::kAction:
                co_await PollFuture{node->action(p.action().service(), p.action().action())};
                result.mutable_accepted();
                break;
            case wire::NodeCommand::kFirewall: {
                firewall_plan = nullptr;
                firewall_parameters = nullptr;
                auto plan = co_await PollFuture{node->firewall(
                    p.firewall().service(),
                    p.firewall().action(),
                    p.firewall().token())};
                result.set_presentation(plan.dump());
                break;
            }
            default:
                throw std::invalid_argument("invalid node management request");
        }
    }

    PolledTask<void> ssh(const wire::Ssh& p, wire::Response& result)
    {
        if (remote_maintenance_pending())
            throw Error(
                ErrorCode::conflict,
                "remote maintenance is pending; continue in Installer");
        if (p.operation() == wire::Ssh::PREPARE_KEY) {
            auto key = co_await io.admin<Json>([&] {
                return prepare_ssh_key(p.id());
            });
            result.set_presentation(key.dump());
            co_return;
        }
        if (p.operation() == wire::Ssh::ENROLL || p.operation() == wire::Ssh::REPLACE_IDENTITY) {
            auto params = ssh_parameters(p);
            auto endpoint = co_await io.admin<NodeEndpoint>([&] {
                return p.operation() == wire::Ssh::ENROLL ? enroll_node(io, params)
                                                          : replace_node_identity(io, params);
            });
            auto client = co_await PollFuture{AgentClient::open(io, std::move(endpoint))};
            nodes.insert_or_assign(p.id(), std::move(client));
            result.mutable_accepted();
            co_return;
        }
        if (p.operation() == wire::Ssh::INSPECT_FIREWALL) {
            firewall_plan = nullptr;
            firewall_parameters = nullptr;
            auto params = ssh_parameters(p);
            auto plan = co_await io.admin<Json>([&] {
                return inspect_node_firewall(params);
            });
            plan["transport"] = "ssh";
            plan["token"] = unique_process_id();
            params.erase("private_key");
            firewall_parameters = std::move(params);
            firewall_plan = std::move(plan);
            firewall_expiry = std::chrono::steady_clock::now() + 5min;
            result.set_presentation(firewall_plan.dump());
            co_return;
        }
        if (p.operation() == wire::Ssh::APPLY_FIREWALL) {
            if (firewall_plan.is_null() || firewall_parameters.is_null()
                || p.token() != firewall_plan.at("token").get<std::string>()
                || std::chrono::steady_clock::now() > firewall_expiry)
                throw std::invalid_argument("firewall confirmation expired; inspect again");
            auto params = std::move(firewall_parameters);
            firewall_parameters = nullptr;
            params["private_key"] = p.private_key();
            auto plan = std::move(firewall_plan);
            firewall_plan = nullptr;
            auto changed = co_await io.admin<Json>([&] {
                return change_node_firewall(params, plan);
            });
            result.set_presentation(changed.dump());
            co_return;
        }
        throw std::invalid_argument("invalid node management request");
    }

    PolledTask<wire::Response> dispatch(wire::Request request)
    {
        wire::Response result;
        result.set_version(1);
        result.set_correlation_id(request.correlation_id());
        const auto finish = [&] {
            protocol::log_rpc_result("manager", request, result, request.has_snapshot());
            return std::move(result);
        };
        try {
            protocol::validate_message(request);
            if (request.version() != 1 || request.correlation_id().empty())
                throw std::invalid_argument("invalid node management request");
            if (request.has_snapshot()) {
                if (initialization.valid()
                    && initialization.wait_for(0ms) == std::future_status::ready)
                    initialization.get();
                *result.mutable_snapshot() = snapshot();
                co_return finish();
            }
            if (request.has_maintenance()
                && request.maintenance().action() == wire::Maintenance::INSPECT) {
                const auto view = co_await io.read<Json>(
                    [] {
                        return remote_maintenance_view();
                    },
                    ServiceIo::ReadLane::response);
                result.set_presentation(view.dump());
                co_return finish();
            }
            if (request.has_inventory()) {
                auto inventory = co_await io.admin<Json>([] {
                    return registered_node_inventory();
                });
                result.set_presentation(inventory.dump());
                co_return finish();
            }
            if (initialization.valid())
                co_await PollUntil{[&] {
                    return initialization.wait_for(0ms) == std::future_status::ready;
                }};
            if (initialization.valid())
                initialization.get();
            if (request.has_node()
                && (request.node().has_history() || request.node().has_endpoint()
                    || request.node().has_connect())
                && nodes.contains(request.node().id())
                && (request.node().id() != "local" || nodes.at("local")->owner_view().online)) {
                co_await node_command(request.node(), result);
                *result.mutable_view() = snapshot();
                co_return finish();
            }
            if (busy)
                throw Error(
                    ErrorCode::conflict,
                    "another node operation is in progress; retry after it completes");
            busy = true;

            struct Release
            {
                bool& flag;

                ~Release()
                {
                    flag = false;
                }
            } release{busy};

            switch (request.operation_case()) {
                case wire::Request::kNode:
                    co_await node_command(request.node(), result);
                    break;
                case wire::Request::kMaintenance: {
                    const auto& p = request.maintenance();
                    std::string action;
                    switch (p.action()) {
                        case wire::Maintenance::INSPECT:
                            action = "inspect";
                            break;
                        case wire::Maintenance::PREPARE:
                            action = "prepare";
                            break;
                        case wire::Maintenance::FINISH:
                            action = "finish";
                            break;
                        case wire::Maintenance::UNINSTALL:
                            action = "uninstall";
                            break;
                        case wire::Maintenance::CONFIGURE:
                            action = "configure";
                            break;
                        default:
                            throw std::invalid_argument("invalid remote maintenance action");
                    }
                    std::vector<Json> connections;
                    for (const auto& connection : p.connections())
                        connections.push_back(ssh_parameters(connection));
                    std::vector<std::string> selected(p.nodes().begin(), p.nodes().end());
                    const auto report = co_await io.admin<Json>([&] {
                        return maintain_remote_nodes(
                            io,
                            action,
                            p.resources(),
                            p.target_version(),
                            selected,
                            p.remove_data(),
                            connections);
                    });
                    if (action == "prepare" || action == "finish") {
                        for (auto it = nodes.begin(); it != nodes.end();) {
                            if (it->first != "local")
                                it = nodes.erase(it);
                            else
                                ++it;
                        }
                    } else if (action == "uninstall") {
                        for (const auto& node : report.at("nodes"))
                            if (!node.at("installed").get<bool>())
                                nodes.erase(node.at("id").get<std::string>());
                    }
                    result.set_presentation(report.dump());
                    if (action == "finish")
                        co_await restore();
                    break;
                }
                case wire::Request::kSsh:
                    co_await ssh(request.ssh(), result);
                    break;
                case wire::Request::kShutdownDevelopment:
                    co_await io.admin<void>([&] {
                        shutdown_development_node(io);
                    });
                    nodes.erase("local");
                    result.mutable_accepted();
                    break;
                default:
                    throw std::invalid_argument("invalid node management request");
            }
        } catch (const std::exception& error) {
            result.mutable_error()->set_code(std::string(error_name(classify(error))));
            result.mutable_error()->set_message(error.what());
        }
        *result.mutable_view() = snapshot();
        co_return finish();
    }
};

NodeManager::NodeManager()
    : impl_(std::make_unique<Impl>())
{}

NodeManager::~NodeManager() = default;

void NodeManager::open()
{
    impl_->initialization = impl_->io
                                .submit<void>([state = impl_.get()](std::stop_token) {
                                    return state->restore();
                                })
                                .share();
}

service::RpcHost::Reply NodeManager::accept(std::string frame)
{
    wire::Request request;
    if (!request.ParseFromString(frame)) {
        wire::Response response;
        response.set_version(1);
        response.mutable_error()->set_code("invalid_request");
        response.mutable_error()->set_message("invalid node management request");
        return [payload = response.SerializeAsString()] {
            return service::RpcHost::Message{payload};
        };
    }
    TraceScope trace(request.correlation_id());
    auto pending = std::make_shared<std::future<wire::Response>>(impl_->io.submit<wire::Response>(
        [state = impl_.get(), request = std::move(request)](std::stop_token) mutable {
            return state->dispatch(std::move(request));
        }));
    return [pending]() -> std::optional<service::RpcHost::Message> {
        if (pending->wait_for(0ms) != std::future_status::ready)
            return std::nullopt;
        return service::RpcHost::Message{pending->get().SerializeAsString()};
    };
}

} // namespace asterion::terminal
