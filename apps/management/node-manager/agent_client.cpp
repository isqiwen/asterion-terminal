#include "sqlite_journal.hpp"
#include <asterion/kernel/native_plugin.hpp>
#include "agent_client.hpp"
#include <asterion/kernel/ipc/rpc_client.hpp>
#include <asterion/kernel/trace.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/node.pb.h>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <array>
#include <set>
#include <stdexcept>

namespace asterion::terminal {
namespace wire = asterion::node::proto;
using namespace std::chrono_literals;

struct AgentClient::Impl : std::enable_shared_from_this<Impl>
{
    ServiceIo& io;
    const NodeEndpoint endpoint;
    bool command_busy = false;
    std::array<std::unique_ptr<ipc::RpcClient>, 2> transports;
    std::stop_source lifetime;
    std::shared_ptr<const wire::Status> cached;
    bool connected = false, observing = false;
    std::string error;
    std::int64_t last_heartbeat = 0, latency = 0;
    std::chrono::steady_clock::time_point last_seen{};

    Impl(ServiceIo& owner, NodeEndpoint value)
        : io(owner),
          endpoint(std::move(value))
    {
        validate_id(endpoint.id);
        // TLS identity files are prepared before the connection is handed to I/O.
        for (auto& transport : transports)
            transport = endpoint.endpoint.empty()
                ? std::make_unique<ipc::RpcClient>(
                      io.reactor(),
                      endpoint.host,
                      endpoint.port,
                      endpoint.tls,
                      1,
                      io.payload_budget(ServiceIo::PayloadLane::administration),
                      2 * 1024 * 1024)
                : std::make_unique<ipc::RpcClient>(
                      io.reactor(),
                      endpoint.endpoint,
                      1,
                      io.payload_budget(ServiceIo::PayloadLane::administration),
                      2 * 1024 * 1024);
    }

    bool stopped(std::stop_token stop) const
    {
        return stop.stop_requested() || lifetime.stop_requested();
    }

    PolledTask<wire::Response> exchange(wire::Request request, std::stop_token stop)
    {
        if (stopped(stop))
            throw Error(ErrorCode::cancelled, "node connection closed");
        if (!request.has_status() && cached && cached->phase() == wire::Status::INITIALIZING)
            co_await ready(stop);
        request.set_version(1);
        request.set_correlation_id(next_correlation_id());
#ifdef ASTERION_SANITIZED
        const auto mutation = 60s;
#else
        const auto mutation = 10s;
#endif
        const auto timeout = request.has_status() ? 3s : request.has_firewall() ? 30s : mutation;
        std::string stage = "exchange";
        bool operation_rejected = false;
        const auto* operation =
            request.GetDescriptor()->FindFieldByNumber(request.operation_case());
        const auto operation_name = operation ? operation->name() : "missing";
        try {
            auto& transport = transports[request.has_status() ? 1 : 0];
            auto reply = transport->request(
                request.SerializeAsString(),
                timeout,
                std::chrono::steady_clock::time_point::max(),
                timeout);
            while (reply.wait_for(0ms) != std::future_status::ready) {
                if (stopped(stop))
                    transport.reset();
                else
                    transport->poll();
                co_await std::suspend_always{};
            }
            const auto payload = reply.get();
            stage = "validate";
            auto response = co_await io.read<wire::Response>(
                [&] {
                    wire::Response decoded;
                    if (!decoded.ParseFromString(*payload))
                        throw Error(ErrorCode::unavailable, "invalid node response");
                    protocol::validate_message(decoded);
                    return decoded;
                },
                ServiceIo::ReadLane::response);
            if (stopped(stop))
                throw Error(ErrorCode::cancelled, "node connection closed");
            if (response.version() != 1 || response.correlation_id() != request.correlation_id())
                throw Error(ErrorCode::unavailable, "node response identity mismatch");
            if (response.has_error()) {
                operation_rejected = !request.has_status();
                throw_remote_error(response.error().code(), response.error().message());
            }
            if (request.has_status()         ? !response.has_status()
                    : request.has_upgrade()  ? !response.has_upgrade()
                    : request.has_firewall() ? !response.has_firewall()
                                             : !response.has_accepted())
                throw Error(ErrorCode::unavailable, "unexpected node response");
            co_return response;
        } catch (const Error& e) {
            const auto diagnostic = "Agent " + operation_name + " " + stage + ": " + e.what();
            if (!operation_rejected) {
                connected = false;
                error = diagnostic;
            }
            throw Error(e.code(), diagnostic);
        }
    }

    PolledTask<void> observe(std::stop_token stop)
    {
        co_await PollUntil{[&] {
            return !observing || stopped(stop);
        }};
        if (stopped(stop))
            throw Error(ErrorCode::cancelled, "node connection closed");
        observing = true;

        struct Release
        {
            bool& observing;

            ~Release()
            {
                observing = false;
            }
        } release{observing};

        const auto start = std::chrono::steady_clock::now();
        wire::Request request;
        request.mutable_status();
        auto response = co_await exchange(std::move(request), stop);
        const auto& status = response.status();
        if (status.instance_id().empty() || status.os().empty() || status.arch().empty()
            || (status.phase() != wire::Status::INITIALIZING
                && status.phase() != wire::Status::READY
                && status.phase() != wire::Status::RECOVERY_REQUIRED))
            throw Error(ErrorCode::unavailable, "invalid node health");
        cached = std::make_shared<const wire::Status>(std::move(*response.mutable_status()));
        last_heartbeat = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
                             .count();
        last_seen = std::chrono::steady_clock::now();
        latency = std::chrono::duration_cast<std::chrono::milliseconds>(last_seen - start).count();
        connected = true;
        error.clear();
    }

    PolledTask<wire::Status> ready(std::stop_token stop)
    {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        for (;;) {
            co_await observe(stop);
            if (cached->phase() == wire::Status::READY)
                co_return *cached;
            if (cached->phase() == wire::Status::RECOVERY_REQUIRED)
                throw Error(ErrorCode::recovery_required, "Agent recovery is required");
            if (std::chrono::steady_clock::now() >= deadline)
                throw Error(ErrorCode::unavailable, "Agent is initializing");
            const auto next = std::chrono::steady_clock::now() + 20ms;
            co_await PollUntil{[&] {
                return stopped(stop) || std::chrono::steady_clock::now() >= next;
            }};
        }
    }

    PolledTask<void> refresh_after_acknowledgement(std::stop_token stop)
    {
        // An acknowledged mutation stays successful even when its observation fails.
        try {
            co_await observe(stop);
        } catch (const std::exception& failure) {
            connected = false;
            error = failure.what();
        }
    }

    NodeSnapshot snapshot() const
    {
        return {
            .id = endpoint.id,
            .host = endpoint.host,
            .port = endpoint.port,
            .online = connected && std::chrono::steady_clock::now() - last_seen <= 15s,
            .last_heartbeat_ms = last_heartbeat,
            .latency_ms = latency,
            .error = error,
            .health = cached
        };
    }

    PolledTask<void> monitor(std::stop_token stop)
    {
        while (!stopped(stop)) {
            const auto next = std::chrono::steady_clock::now() + 5s;
            co_await PollUntil{[&] {
                return stopped(stop) || std::chrono::steady_clock::now() >= next;
            }};
            if (stopped(stop))
                break;
            try {
                co_await observe(stop);
            } catch (const std::exception& e) {
                connected = false;
                error = e.what();
            }
        }
        for (auto& transport : transports)
            transport.reset();
    }

    template <class T>
    std::future<T> submit(std::function<PolledTask<T>(std::stop_token)> operation, bool exclusive)
    {
        return io.submit<T>(
            [state = shared_from_this(), operation = std::move(operation), exclusive](
                std::stop_token stop) -> PolledTask<T> {
                if (exclusive)
                    co_await PollUntil{[&] {
                        return !state->command_busy || state->stopped(stop);
                    }};
                if (state->stopped(stop))
                    throw Error(ErrorCode::cancelled, "node connection closed");
                if (exclusive)
                    state->command_busy = true;
                struct Release
                {
                    Impl& state;
                    bool exclusive;
                    ~Release()
                    {
                        if (exclusive)
                            state.command_busy = false;
                    }
                } release{*state, exclusive};
                if constexpr (std::is_void_v<T>)
                    co_await operation(stop);
                else
                    co_return co_await operation(stop);
            },
            exclusive ? ServiceIo::Lane::administration : ServiceIo::Lane::request);
    }

    PolledTask<std::string> upload_artifact(
        const std::filesystem::path& path,
        const std::string& os,
        const std::string& arch,
        std::stop_token stop,
        const std::string& expected_digest = {})
    {
        struct Artifact
        {
            std::string hash;
            std::uintmax_t size;
            std::unique_ptr<std::ifstream> input;
        };

        auto artifact = co_await io.admin<Artifact>([&] {
            const auto actual = artifact_platform(path);
            if (actual.os != os || actual.arch != arch)
                throw std::invalid_argument(
                    "program binary platform does not match selected target");
            const auto digest = sha256_file(path);
            if (!expected_digest.empty() && digest != expected_digest)
                throw std::invalid_argument("native plugin catalog changed; inspect again");
            return Artifact{
                digest,
                std::filesystem::file_size(path),
                std::make_unique<std::ifstream>(path, std::ios::binary)
            };
        });
        wire::Request begin;
        auto* upload = begin.mutable_upload();
        upload->set_sha256(artifact.hash);
        upload->set_size(artifact.size);
        upload->set_os(os);
        upload->set_arch(arch);
        co_await exchange(std::move(begin), stop);
        std::uint64_t offset = 0;
        for (;;) {
            auto bytes = co_await io.admin<std::string>([&] {
                std::string buffer(1024 * 1024, '\0');
                artifact.input->read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                buffer.resize(static_cast<std::size_t>(artifact.input->gcount()));
                if (buffer.empty() && (!artifact.input->eof() || offset != artifact.size))
                    throw std::runtime_error("artifact changed or failed while reading");
                return buffer;
            });
            if (bytes.empty())
                break;
            wire::Request chunk;
            auto* c = chunk.mutable_chunk();
            c->set_sha256(artifact.hash);
            c->set_offset(offset);
            offset += bytes.size();
            c->set_data(std::move(bytes));
            co_await exchange(std::move(chunk), stop);
        }
        co_await io.admin<void>([&] {
            artifact.input.reset();
        });
        wire::Request finish;
        finish.mutable_finish()->set_sha256(artifact.hash);
        co_await exchange(std::move(finish), stop);
        co_return artifact.hash;
    }

    PolledTask<NodeSnapshot> inspect_status(std::stop_token)
    {
        co_return snapshot();
    }

    PolledTask<std::vector<HistoryService>> history_inventory(std::stop_token stop)
    {
        const auto health = (co_await ready(stop));
        std::vector<HistoryService> result;
        const bool local = !endpoint.endpoint.empty();
        for (const auto& service : health.services()) {
            if (service.kind() != wire::RESEARCH && service.kind() != wire::DATA_SERVICE)
                continue;
            if (result.size() >= 1000)
                throw std::invalid_argument("historical service inspection limit exceeded");
            if (!local && service.port() > 65535)
                throw std::invalid_argument("invalid historical service address");
            ServiceEndpoint address{
                local ? "" : endpoint.host,
                service.id(),
                local ? std::uint16_t(0) : static_cast<std::uint16_t>(service.port()),
                local ? ipc::TlsIdentity{} : endpoint.tls,
                local ? service.endpoint() : ""
            };
            result.push_back(
                {service.kind(), service.directory(), service.state(), std::move(address)});
        }
        co_return result;
    }

    PolledTask<Json> coordinate_upgrade(
        const std::string& operation,
        const std::string& action,
        std::stop_token stop)
    {
        wire::Request request;
        if (snapshot().health->upgrade_protocol() != 1)
            throw Error(
                ErrorCode::unavailable,
                "installed Agent does not support coordinated upgrades");
        request.mutable_upgrade()->set_operation_id(operation);
        request.mutable_upgrade()->set_action(action);
        const auto response = (co_await exchange(std::move(request), stop));
        co_return Json{
            {"phase", response.upgrade().phase()},
            {"detail", response.upgrade().detail()}
        };
    }

    PolledTask<void> maintenance(
        bool enter,
        const std::string& operation,
        const std::string& instance,
        std::stop_token stop)
    {
        wire::Request request;
        auto* maintenance = request.mutable_maintenance();
        maintenance->set_enter(enter);
        maintenance->set_operation_id(operation);
        maintenance->set_instance_id(instance);
        co_await exchange(std::move(request), stop);
        co_await refresh_after_acknowledgement(stop);
    }

    PolledTask<ServiceEndpoint>
    service_endpoint(const std::string& service, wire::ServiceKind kind, std::stop_token stop)
    {
        const auto observed = (co_await ready(stop));
        for (const auto& s : observed.services())
            if (s.id() == service) {
                if (s.kind() != kind)
                    throw std::invalid_argument("service kind mismatch");
                if (s.state() != "running")
                    throw std::invalid_argument("service process is not running");
                co_return ServiceEndpoint{
                    endpoint.host,
                    service,
                    static_cast<std::uint16_t>(s.port()),
                    endpoint.tls,
                    s.endpoint(),
                    "local",
                    observed.node_id()
                };
            }
        throw std::invalid_argument("unknown managed service");
    }

    PolledTask<void> deploy(const ServiceDeployment& deployment, std::stop_token stop)
    {
        const auto& programs = deployment.programs;
        const auto& path = programs.executable;
        const auto& os = deployment.platform.os;
        const auto& arch = deployment.platform.arch;
        const auto& service = deployment.service;
        const auto kind = deployment.kind;
        if (!path.is_absolute())
            throw std::invalid_argument("artifact path must be absolute");
        const auto observed = (co_await ready(stop));
        if (observed.os() != os || observed.arch() != arch)
            throw std::invalid_argument("selected artifact platform does not match node");
        for (const auto& existing : observed.services())
            if (existing.id() == service)
                throw std::invalid_argument(
                    "service already exists; its program and ledger are not replaced");
        if (kind != wire::MARKET_DATA && kind != wire::RESEARCH && kind != wire::LIVE_TRADING
            && kind != wire::DATA_SERVICE && kind != wire::INTELLIGENCE)
            throw std::invalid_argument("invalid service kind");
        const auto hash = (co_await upload_artifact(path, os, arch, stop));
        const auto library = programs.provider.empty()
            ? std::string{}
            : (co_await upload_artifact(programs.provider, os, arch, stop));
        wire::Request deploy;
        auto* d = deploy.mutable_deploy();
        d->set_service_id(service);
        d->set_sha256(hash);
        d->set_port(deployment.port);
        d->set_directory(deployment.directory);
        d->set_kind(kind);
        d->set_provider_artifact(library);
        d->set_catalog_artifact(
            programs.catalog.empty()
                ? std::string{}
                : (co_await upload_artifact(programs.catalog, os, arch, stop)));
        const bool risk = kind == wire::LIVE_TRADING;
        if ((kind == wire::RESEARCH || kind == wire::DATA_SERVICE || risk) && deployment.plugins) {
            for (const auto& artifact : *deployment.plugins) {
                d->add_plugin_artifacts(
                    co_await upload_artifact(artifact.path, os, arch, stop, artifact.sha256));
            }
        } else if (kind == wire::RESEARCH || kind == wire::DATA_SERVICE || risk) {
            const auto paths = co_await io.admin<std::vector<std::filesystem::path>>([&] {
                auto folder = path.parent_path() / "plugins";
                if (os == current_platform().os && arch == current_platform().arch)
                    folder = native_plugin_directory();
                if (!std::filesystem::is_directory(folder))
                    throw std::invalid_argument(
                        "native plugin directory is missing from service bundle");
                std::vector<std::filesystem::path> result;
                for (const auto& entry : std::filesystem::directory_iterator(folder))
                    if (entry.path().extension() == (os == "macos" ? ".dylib" : ".so")
                        && (!risk || entry.path().stem() == "asterion_order_limits"))
                        result.push_back(entry.path());
                return result;
            });
            for (const auto& candidate : paths)
                d->add_plugin_artifacts(co_await upload_artifact(candidate, os, arch, stop));
            if (risk && d->plugin_artifacts_size() != 1)
                throw std::invalid_argument("pre-trade risk plugin is unavailable");
            std::sort(d->mutable_plugin_artifacts()->begin(), d->mutable_plugin_artifacts()->end());
        }
        (co_await exchange(std::move(deploy), stop));
        co_await refresh_after_acknowledgement(stop);
    }

    PolledTask<void> update(const ServiceUpdate& update, std::stop_token stop)
    {
        const auto& service = update.service;
        const auto& revision = update.expected_revision;
        const auto& os = update.platform.os;
        const auto& arch = update.platform.arch;
        const auto observed = (co_await ready(stop));
        std::vector<std::string> plugins;
        bool found = false;
        for (const auto& s : observed.services())
            if (s.id() == service) {
                found = true;
                plugins = std::vector<std::string>(
                    s.plugin_artifacts().begin(),
                    s.plugin_artifacts().end());
                if (s.desired_running() != false || s.state() != "stopped"
                    || s.revision() != revision)
                    throw std::invalid_argument(
                        "stop and inspect the current service before updating");
            }
        if (!found || observed.os() != os || observed.arch() != arch)
            throw std::invalid_argument("update target mismatch");
        wire::Request request;
        auto* u = request.mutable_update();
        u->set_service_id(service);
        u->set_expected_revision(revision);
        u->set_artifact(
            (update.programs.executable.empty()
                    ? std::string{}
                    : co_await upload_artifact(update.programs.executable, os, arch, stop)));
        u->set_provider_artifact(
            (update.programs.provider.empty()
                    ? std::string{}
                    : co_await upload_artifact(update.programs.provider, os, arch, stop)));
        u->set_catalog_artifact(
            (update.programs.catalog.empty()
                    ? std::string{}
                    : co_await upload_artifact(update.programs.catalog, os, arch, stop)));
        for (const auto& hash : plugins)
            u->add_plugin_artifacts(hash);
        (co_await exchange(request, stop));
        co_await refresh_after_acknowledgement(stop);
    }

    PolledTask<void> configure_plugins(
        const std::string& service,
        const std::string& revision,
        const std::vector<std::string>& hashes,
        std::stop_token stop)
    {
        if (endpoint.endpoint.empty())
            throw std::invalid_argument("native plugin selection requires the local node");
        const auto observed = (co_await ready(stop));
        const wire::Service* current = nullptr;
        for (const auto& item : observed.services())
            if (item.id() == service)
                current = &item;
        if (!current
            || (current->kind() != wire::RESEARCH && current->kind() != wire::DATA_SERVICE))
            throw std::invalid_argument("service does not support managed native plugins");
        if (current->desired_running() || current->state() != "stopped")
            throw std::invalid_argument("stop the service before configuring plugins");
        if (current->revision() != revision)
            throw std::invalid_argument("service configuration changed; inspect again");
        const std::vector<std::string> installed(
            current->plugin_artifacts().begin(),
            current->plugin_artifacts().end());
        const auto selection = co_await io.admin<PluginSelection>([&] {
            return local_plugin_catalog().select_data_task_plugins(hashes, installed);
        });
        const auto platform = current_platform();
        for (const auto& artifact : selection.uploads)
            co_await upload_artifact(
                artifact.path,
                platform.os,
                platform.arch,
                stop,
                artifact.sha256);
        wire::Request request;
        auto* configuration = request.mutable_configure_plugins();
        configuration->set_service_id(service);
        configuration->set_expected_revision(revision);
        for (const auto& hash : selection.hashes)
            configuration->add_artifacts(hash);
        (co_await exchange(request, stop));
        co_await refresh_after_acknowledgement(stop);
    }

    PolledTask<ServiceEndpoint>
    local_session(const std::filesystem::path& directory, std::stop_token stop)
    {
        const auto kind = wire::LIVE_TRADING;
        if (endpoint.endpoint.empty())
            throw std::invalid_argument("local Agent required");
        const auto value = co_await io.admin<std::string>([&] {
            if (!directory.is_absolute() || !std::filesystem::is_directory(directory)
                || std::filesystem::is_symlink(directory))
                throw std::invalid_argument("choose an existing absolute trading record directory");
            check_journal_directory(directory, {"plugins", "ctp-flow", "archives", "facts"});
            const auto path = std::filesystem::canonical(directory).u8string();
            return std::string(path.begin(), path.end());
        });
        std::string service;
        {
            const auto observed = (co_await ready(stop));
            for (const auto& s : observed.services())
                if (s.directory() == value) {
                    if (s.kind() != kind)
                        throw std::invalid_argument("this directory belongs to another service");
                    service = s.id();
                    break;
                }
        }
        if (service.empty()) {
            service = "live-" + unique_process_id();
            (co_await deploy(
                {.service = service,
                    .kind = kind,
                    .platform = current_platform(),
                    .programs = co_await io.admin<ServicePrograms>([&] {
                        return local_service_programs(kind);
                    }),
                    .directory = value},
                stop));
        } else
            (co_await action(service, "start", stop));
        co_return co_await wait_trading(service, stop);
    }

    PolledTask<ServiceEndpoint> trading_session(const std::string& service, std::stop_token stop)
    {
        const auto observed = co_await ready(stop);
        const auto found = std::ranges::find(observed.services(), service, &wire::Service::id);
        if (found == observed.services().end() || found->kind() != wire::LIVE_TRADING)
            throw std::invalid_argument("choose a deployed trading service instance");
        co_await action(service, "start", stop);
        co_return co_await wait_trading(service, stop);
    }

    PolledTask<ServiceEndpoint> wait_trading(const std::string& service, std::stop_token stop)
    {
        // Process creation is not listener readiness. The Agent's first health
        // response confirms this process generation has bound its service channels.
        // Wait here, before any account creation or trading command is submitted.
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        for (;;) {
            const auto address = (co_await service_endpoint(service, wire::LIVE_TRADING, stop));
            const auto status = snapshot();
            for (const auto& item : status.health->services())
                if (item.id() == service && item.last_heartbeat_ms()
                    && item.health() != "unresponsive")
                    co_return address;
            if (std::chrono::steady_clock::now() >= deadline)
                throw Error(ErrorCode::unavailable, "trading service did not become reachable");
            const auto next = std::chrono::steady_clock::now() + 100ms;
            co_await PollUntil{[&] {
                return stopped(stop) || std::chrono::steady_clock::now() >= next;
            }};
        }
    }

    PolledTask<ServiceEndpoint> local_market(std::stop_token stop)
    {
        if (endpoint.endpoint.empty())
            throw std::invalid_argument("local Agent required");
        const std::string service = "market-data";
        bool exists = false;
        {
            const auto observed = (co_await ready(stop));
            for (const auto& s : observed.services())
                if (s.id() == service)
                    exists = true;
        }
        if (!exists) {
            auto programs = co_await io.admin<ServicePrograms>([] {
                auto programs = local_service_programs(wire::MARKET_DATA);
                // The optional CTP libraries need not be installed when the service is created.
                if (!std::filesystem::exists(programs.provider))
                    programs.provider.clear();
                if (!std::filesystem::exists(programs.catalog))
                    programs.catalog.clear();
                return programs;
            });
            (co_await deploy(
                {.service = service,
                    .kind = wire::MARKET_DATA,
                    .platform = current_platform(),
                    .programs = programs},
                stop));
        } else
            (co_await action(service, "start", stop));
        co_return (co_await service_endpoint(service, wire::MARKET_DATA, stop));
    }

    PolledTask<ServiceEndpoint> local_data(std::stop_token stop)
    {
        if (endpoint.endpoint.empty())
            throw std::invalid_argument("local Agent required");
        const std::string service = "historical-data";
        const auto observed = co_await ready(stop);
        const auto found = std::ranges::find(observed.services(), service, &wire::Service::id);
        if (found == observed.services().end()) {
            const auto programs = co_await io.admin<ServicePrograms>([] {
                return local_service_programs(wire::DATA_SERVICE);
            });
            co_await deploy(
                {.service = service,
                    .kind = wire::DATA_SERVICE,
                    .platform = current_platform(),
                    .programs = programs},
                stop);
        } else {
            if (found->kind() != wire::DATA_SERVICE)
                throw std::invalid_argument("service kind mismatch");
            co_await action(service, "start", stop);
        }
        co_return co_await service_endpoint(service, wire::DATA_SERVICE, stop);
    }

    PolledTask<ServiceEndpoint> local_intelligence(std::stop_token stop)
    {
        if (endpoint.endpoint.empty())
            throw std::invalid_argument("local Agent required");
        const std::string service = "intelligence";
        const auto observed = co_await ready(stop);
        const auto found = std::ranges::find(observed.services(), service, &wire::Service::id);
        if (found == observed.services().end()) {
            const auto programs = co_await io.admin<ServicePrograms>([] {
                return local_service_programs(wire::INTELLIGENCE);
            });
            co_await deploy(
                {.service = service,
                    .kind = wire::INTELLIGENCE,
                    .platform = current_platform(),
                    .programs = programs},
                stop);
        } else {
            if (found->kind() != wire::INTELLIGENCE)
                throw std::invalid_argument("service kind mismatch");
            co_await action(service, "start", stop);
        }
        co_return co_await service_endpoint(service, wire::INTELLIGENCE, stop);
    }

    PolledTask<ServiceEndpoint> local_research(
        const std::optional<std::vector<std::string>>& selected_plugins,
        std::stop_token stop)
    {
        if (endpoint.endpoint.empty())
            throw std::invalid_argument("local Agent required");
        const std::string service = "research";
        bool exists = false;
        {
            const auto observed = (co_await ready(stop));
            for (const auto& s : observed.services()) {
                if (s.id() == service) {
                    if (s.kind() != wire::RESEARCH)
                        throw std::invalid_argument("local research service kind mismatch");
                    exists = true;
                }
            }
        }
        if (exists && selected_plugins)
            throw std::invalid_argument(
                "local research service already exists; configure its plugins");
        std::optional<std::vector<PluginArtifact>> selection;
        if (selected_plugins) {
            selection = co_await io.admin<std::vector<PluginArtifact>>([&] {
                return local_plugin_catalog().select_data_task_plugins(*selected_plugins).uploads;
            });
        }
        if (!exists) {
            (co_await deploy(
                {.service = service,
                    .kind = wire::RESEARCH,
                    .platform = current_platform(),
                    .programs = co_await io.admin<ServicePrograms>([] {
                        return local_service_programs(wire::RESEARCH);
                    }),
                    .plugins = selection},
                stop));
        } else
            (co_await action(service, "start", stop));
        co_return (co_await service_endpoint(service, wire::RESEARCH, stop));
    }

    PolledTask<Json> firewall(
        const std::string& service,
        const std::string& action,
        const std::string& token,
        std::stop_token stop)
    {
        wire::Request request;
        auto* operation = request.mutable_firewall();
        operation->set_service_id(service);
        operation->set_action(action);
        operation->set_token(token);
        const auto response = (co_await exchange(std::move(request), stop));
        const auto& plan = response.firewall();
        Json result{
            {"id", endpoint.id},
            {"host", endpoint.host},
            {"service", service},
            {"transport", "agent"},
            {"token", plan.token()},
            {"source", plan.source()},
            {"port", plan.port()},
            {"backend", plan.backend()},
            {"state", plan.state()},
            {"can_apply", plan.can_apply()},
            {"rule", plan.rule()},
            {"action", plan.action()},
            {"verification", plan.verification()}
        };
        if (plan.state() == "applied") {
            const auto reachable = co_await io.admin<bool>([&] {
                try {
                    auto channel = ipc::TlsChannel::connect(
                        endpoint.host,
                        static_cast<unsigned short>(plan.port()),
                        endpoint.tls,
                        3s);
                    return true;
                } catch (const std::exception&) {
                    return false;
                }
            });
            result["verification"] = reachable ? "tls_reachable" : "unreachable";
        }
        co_return result;
    }

    PolledTask<void>
    action(const std::string& service, const std::string& operation, std::stop_token stop)
    {
        wire::Request request;
        auto* action = request.mutable_action();
        action->set_service_id(service);
        if (operation == "start")
            action->set_kind(wire::Action::START);
        else if (operation == "stop")
            action->set_kind(wire::Action::STOP);
        else if (operation == "restart")
            action->set_kind(wire::Action::RESTART);
        else
            throw std::invalid_argument("unsupported service action");
        (co_await exchange(std::move(request), stop));
        co_await refresh_after_acknowledgement(stop);
    }
};

AgentClient::AgentClient(ServiceIo& io, NodeEndpoint endpoint)
    : impl_(std::make_shared<Impl>(io, std::move(endpoint)))
{}

std::future<std::shared_ptr<AgentClient>>
AgentClient::open(ServiceIo& io, NodeEndpoint endpoint, bool observe)
{
    return io.submit<std::shared_ptr<AgentClient>>(
        [&io, endpoint = std::move(endpoint), observe](
            std::stop_token stop) mutable -> PolledTask<std::shared_ptr<AgentClient>> {
            auto client = co_await io.admin<std::shared_ptr<AgentClient>>([&] {
                return std::shared_ptr<AgentClient>(new AgentClient(io, std::move(endpoint)));
            });
            if (observe)
                co_await client->impl_->observe(stop);
            (void)client->impl_->io.submit<void>(
                [state = client->impl_](std::stop_token stop) {
                    return state->monitor(stop);
                },
                ServiceIo::Lane::observation);
            co_return client;
        });
}

AgentClient::~AgentClient()
{
    impl_->lifetime.request_stop();
}

NodeSnapshot AgentClient::owner_view() const
{
    return impl_->snapshot();
}

std::future<void> AgentClient::refresh()
{
    return impl_->submit<void>(
        [state = impl_](std::stop_token stop) {
            return state->observe(stop);
        },
        false);
}

std::future<Json> AgentClient::status() const
{
    return impl_->submit<Json>(
        [state = impl_](std::stop_token) -> PolledTask<Json> {
            auto view = state->snapshot();
            co_return co_await state->io.read<Json>(
                [view = std::move(view)] {
                    return node_snapshot_json(view);
                },
                ServiceIo::ReadLane::response);
        },
        false);
}

std::future<NodeSnapshot> AgentClient::inspect_status() const
{
    return impl_->submit<NodeSnapshot>(
        [state = impl_](std::stop_token stop) {
            return state->inspect_status(stop);
        },
        false);
}

std::future<std::vector<HistoryService>> AgentClient::history_inventory()
{
    return impl_->submit<std::vector<HistoryService>>(
        [state = impl_](std::stop_token stop) {
            return state->history_inventory(stop);
        },
        false);
}

std::future<Json>
AgentClient::coordinate_upgrade(const std::string& operation, const std::string& action)
{
    return impl_->submit<Json>(
        [state = impl_, operation, action](std::stop_token stop) {
            return state->coordinate_upgrade(operation, action, stop);
        },
        true);
}

std::future<void>
AgentClient::maintenance(bool enter, const std::string& operation, const std::string& instance)
{
    return impl_->submit<void>(
        [state = impl_, enter, operation, instance](std::stop_token stop) {
            return state->maintenance(enter, operation, instance, stop);
        },
        true);
}

std::future<ServiceEndpoint>
AgentClient::service_endpoint(const std::string& service, wire::ServiceKind kind)
{
    return impl_->submit<ServiceEndpoint>(
        [state = impl_, service, kind](std::stop_token stop) {
            return state->service_endpoint(service, kind, stop);
        },
        false);
}

std::future<void> AgentClient::deploy(const ServiceDeployment& deployment)
{
    return impl_->submit<void>(
        [state = impl_, deployment](std::stop_token stop) {
            return state->deploy(deployment, stop);
        },
        true);
}

std::future<void> AgentClient::update(const ServiceUpdate& update)
{
    return impl_->submit<void>(
        [state = impl_, update](std::stop_token stop) {
            return state->update(update, stop);
        },
        true);
}

std::future<void> AgentClient::configure_plugins(
    const std::string& service,
    const std::string& revision,
    const std::vector<std::string>& hashes)
{
    return impl_->submit<void>(
        [state = impl_, service, revision, hashes](std::stop_token stop) {
            return state->configure_plugins(service, revision, hashes, stop);
        },
        true);
}

std::future<ServiceEndpoint> AgentClient::local_session(const std::filesystem::path& directory)
{
    return impl_->submit<ServiceEndpoint>(
        [state = impl_, directory](std::stop_token stop) {
            return state->local_session(directory, stop);
        },
        true);
}

std::future<ServiceEndpoint> AgentClient::trading_session(const std::string& service)
{
    return impl_->submit<ServiceEndpoint>(
        [state = impl_, service](std::stop_token stop) {
            return state->trading_session(service, stop);
        },
        true);
}

std::future<ServiceEndpoint> AgentClient::local_market()
{
    return impl_->submit<ServiceEndpoint>(
        [state = impl_](std::stop_token stop) {
            return state->local_market(stop);
        },
        true);
}

std::future<ServiceEndpoint> AgentClient::local_data()
{
    return impl_->submit<ServiceEndpoint>(
        [state = impl_](std::stop_token stop) {
            return state->local_data(stop);
        },
        false);
}

std::future<ServiceEndpoint> AgentClient::local_intelligence()
{
    return impl_->submit<ServiceEndpoint>(
        [state = impl_](std::stop_token stop) {
            return state->local_intelligence(stop);
        },
        false);
}

std::future<ServiceEndpoint> AgentClient::local_research(
    const std::optional<std::vector<std::string>>& selected_plugins)
{
    return impl_->submit<ServiceEndpoint>(
        [state = impl_, selected_plugins](std::stop_token stop) {
            return state->local_research(selected_plugins, stop);
        },
        true);
}

std::future<Json> AgentClient::firewall(
    const std::string& service,
    const std::string& action,
    const std::string& token)
{
    return impl_->submit<Json>(
        [state = impl_, service, action, token](std::stop_token stop) {
            return state->firewall(service, action, token, stop);
        },
        true);
}

std::future<void> AgentClient::action(const std::string& service, const std::string& operation)
{
    return impl_->submit<void>(
        [state = impl_, service, operation](std::stop_token stop) {
            return state->action(service, operation, stop);
        },
        true);
}

} // namespace asterion::terminal
