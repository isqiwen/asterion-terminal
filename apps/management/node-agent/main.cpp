#include "node_storage_identity.hpp"
#include <asterion/research.pb.h>
#include <asterion/intelligence.pb.h>
#include "program_preparation.hpp"
#include <asterion/protocol/health.hpp>
#include "plugin_artifacts.hpp"
#include "service_configuration.hpp"
#include "supervision_rpc.hpp"
#include "upgrade_plan.hpp"
#include "artifact_uploads.hpp"
#include "firewall_control.hpp"
#include <asterion/data_service.pb.h>
#include "managed_paths.hpp"
#include "firewall.hpp"
#include <asterion/kernel/environment.hpp>
#include <asterion/kernel/rpc_host.hpp>
#include <asterion/kernel/polled_task.hpp>
#include <asterion/kernel/ipc/rpc_client.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <CLI/CLI.hpp>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/ipc/tls_channel.hpp>
#include <asterion/kernel/logger.hpp>
#include <asterion/kernel/trace.hpp>
#include <asterion/protocol/rpc_diagnostics.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/file_lock.hpp>
#include <asterion/kernel/process/owner.hpp>
#include <asterion/kernel/process/resources.hpp>
#include <asterion/kernel/thread_pool.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/market.pb.h>
#include <asterion/node.pb.h>
#include <asterion/task.pb.h>
#include <atomic>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <stdexcept>
namespace fs = std::filesystem;
namespace wire = asterion::node::proto;
using namespace asterion;
using namespace std::chrono_literals;

namespace {

using namespace asterion::agent;

std::string utf8(const fs::path& p)
{
    const auto s = p.u8string();
    return {s.begin(), s.end()};
}

struct Service
{
    ServiceConfiguration configuration;
    std::string error, health_endpoint;
    protocol::ServiceHealth health = protocol::ServiceHealth::starting;
    std::optional<runtime::proto::ExecutionHealth> execution;
    std::chrono::steady_clock::time_point execution_observed{};
    std::int64_t last_heartbeat = 0;
    unsigned failures = 0;
    std::chrono::steady_clock::time_point probe{};
    // When the current process was started; probes repeat quickly until the
    // first heartbeat so a healthy service is reported within moments.
    std::chrono::steady_clock::time_point started{};
    unsigned int restarts = 0;
    std::uint64_t generation = 0, retiring_pid = 0;
    bool starting = false;
    std::unique_ptr<ChildProcess> process;
    std::chrono::steady_clock::time_point retry{};
};

class Agent
{
    fs::path root_;
    PluginArtifacts plugins_{root_, current_executable()};
    ipc::TlsIdentity tls_;
    std::string bind_;
    fs::path sockets_;
    bool local_;
    unsigned short control_port_;
    std::map<std::string, Service> services_;
    std::string maintenance_;
    std::optional<agent::UpgradePlan> upgrade_;
    std::string upgrade_error_;
    wire::Status::Phase phase_ = wire::Status::INITIALIZING;
    wire::Error failure_;
    Progress& io_progress_;
    Progress state_progress_, initialization_progress_, persistence_progress_;
    bool recovering_drain_ = false;

    bool upgrade_active() const
    {
        return upgrade_ && upgrade_->active();
    }

    // Requests wait on these pools; finished work prompts the I/O owner.
    ThreadPool journal_{1, 8, service::wake_io_owner}, operations_{2, 8, service::wake_io_owner};
    bool mutation_active_ = false, node_mutation_ = false, stopping_ = false;
    std::size_t admitted_mutations_ = 0;
    std::string last_supervised_;
    std::string mutating_service_;
    HostResourceMonitor resource_monitor_;
    std::optional<wire::HostResources> host_resources_;
    std::optional<PolledTask<void>> sampling_;
    std::chrono::steady_clock::time_point next_sample_{};
    std::map<std::string, PolledTask<void>> observations_;
    std::optional<PolledTask<void>> upgrading_, initializing_;
    std::chrono::steady_clock::time_point next_supervision_{};
    std::size_t preparing_ = 0;

    template <class F>
    PolledTask<void> prepare(F work)
    {
        co_await PollUntil{[&] {
            return preparing_ < 8;
        }};

        struct Slot
        {
            std::size_t& count;

            explicit Slot(std::size_t& value)
                : count(value)
            {
                ++count;
            }

            ~Slot()
            {
                --count;
            }
        } slot(preparing_);

        co_await PollFuture{operations_.submit([work = std::move(work)](std::stop_token) {
            work();
        })};
    }

    PolledTask<void> stop_processes(Service& service)
    {
        // Transfer child ownership before waiting; no pool thread touches Service.
        service.retiring_pid = service.process ? service.process->id() : 0;
        auto process = std::move(service.process);
        ++service.generation;
        co_await prepare([&] {
            if (process)
                process->request_stop();
            process.reset();
        });
        service.retiring_pid = 0;
    }

    const std::string instance_ = unique_process_id();
    std::string node_id_;
    agent::BlockingWork blocking_ = [this](std::function<void()> work) {
        return prepare(std::move(work));
    };
    agent::ArtifactUploads uploads_{root_, blocking_};
    agent::FirewallControl firewall_{root_, blocking_};
    const std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();

    fs::path binary(const std::string& hash) const
    {
        return root_ / "artifacts" / (hash + ".bin");
    }

    std::string revision(const ServiceConfiguration& configuration) const
    {
        return service_revision(configuration);
    }

    void require_ready() const
    {
        if (phase_ != wire::Status::READY)
            throw Error(
                phase_ == wire::Status::INITIALIZING ? ErrorCode::unavailable
                                                     : ErrorCode::recovery_required,
                phase_ == wire::Status::INITIALIZING ? "Agent is initializing"
                                                     : "Agent recovery is required");
    }

    void fail(const char* diagnostic, const std::exception& error)
    {
        phase_ = wire::Status::RECOVERY_REQUIRED;
        failure_.set_code(std::string(error_name(classify(error))));
        failure_.set_message(std::string(diagnostic) + ": " + error.what());
        log_process_event(
            "agent",
            LogLevel::error,
            "agent.recovery_required",
            {{"message", diagnostic}, {"cause", error.what()}});
    }

    template <class F>
    PolledTask<void> persist(F work)
    {
        try {
            co_await PollFuture{journal_.submit([this, work = std::move(work)](std::stop_token) {
                persistence_progress_.begin();
                struct Complete
                {
                    Progress& progress;
                    ~Complete()
                    {
                        progress.finish();
                    }
                } complete{persistence_progress_};
                work();
            })};
        } catch (const std::exception& error) {
            fail("Agent configuration commit failed; inspect node logs", error);
            throw;
        }
    }

    PolledTask<void> save(std::string name, ServiceConfiguration configuration)
    {
        co_await persist(
            [folder = root_ / "services" / name, configuration = std::move(configuration)] {
                save_service_configuration(folder, configuration);
            });
    }

    PolledTask<void> verify_artifact(const std::string& hash, const char* diagnostic)
    {
        co_await prepare([&] {
            if (sha256_file(binary(hash)) != hash)
                throw std::invalid_argument(diagnostic);
        });
    }

    PolledTask<void> configure_plugins(const wire::ConfigurePlugins& request)
    {
        validate_service_id(request.service_id());
        validate_artifact_digest(request.expected_revision());
        auto& current = services_.at(request.service_id());
        if (current.configuration.kind != wire::RESEARCH
            && current.configuration.kind != wire::DATA)
            throw std::invalid_argument("service does not support managed native plugins");
        if (current.configuration.desired || (current.process && !current.process->exited()))
            throw std::invalid_argument("stop the service before configuring plugins");
        if (revision(current.configuration) != request.expected_revision())
            throw std::invalid_argument("service configuration changed; inspect again");
        auto next = current.configuration;
        next.desired = false;
        next.plugin_artifacts.assign(request.artifacts().begin(), request.artifacts().end());
        co_await prepare([&] {
            plugins_.verify(next.plugin_artifacts);
            for (const auto& hash : next.plugin_artifacts) {
                const auto actual = artifact_platform(binary(hash)), expected = current_platform();
                if (actual.os != expected.os || actual.arch != expected.arch)
                    throw std::invalid_argument("native plugin platform mismatch");
            }
        });
        std::sort(next.plugin_artifacts.begin(), next.plugin_artifacts.end());
        // Persist the complete selection before changing the stopped service in memory.
        co_await save(request.service_id(), next);
        current.configuration.plugin_artifacts = std::move(next.plugin_artifacts);
        current.process.reset();
        current.restarts = 0;
        current.error.clear();
        current.health = protocol::ServiceHealth::offline;
        current.last_heartbeat = 0;
        current.execution.reset();
    }

    struct Started
    {
        std::string endpoint, health_endpoint;
        std::unique_ptr<ChildProcess> process;
    };

    std::string service_endpoint(const std::string& name) const
    {
        return utf8(sockets_ / (name + ".sock"));
    }

    Started prepare_start(const std::string& name, const ServiceConfiguration& configuration)
    {
        Started result;
        const auto executable = binary(configuration.artifact);
        if (sha256_file(executable) != configuration.artifact)
            throw std::runtime_error("artifact integrity check failed");
        result.endpoint = service_endpoint(name);
        result.health_endpoint = utf8(sockets_ / (name + ".health"));

        // Only this locked Agent owns these ephemeral socket paths.
        fs::remove(result.endpoint);
        fs::remove(result.health_endpoint);
        std::vector<std::string> args{
            "--owner-pid",
            std::to_string(current_process_id()),
            "--session",
            name,
            "--directory",
            configuration.directory,
            "--health-endpoint",
            result.health_endpoint
        };
        if (configuration.kind == wire::RESEARCH || configuration.kind == wire::TRADING
            || configuration.kind == wire::DATA)
            args.insert(
                args.end(),
                {"--plugin-directory",
                    utf8(plugins_.materialize(name, configuration.plugin_artifacts))});
        if (configuration.kind == wire::DATA)
            args.insert(
                args.end(),
                {"--internal-endpoint",
                    utf8(sockets_ / (name + ".internal")),
                    "--node-id",
                    node_id_,
                    "--discovery-endpoint",
                    discovery_endpoint()});
        if ((configuration.kind == wire::DATA || configuration.kind == wire::RESEARCH
                || configuration.kind == wire::INTELLIGENCE)
            && configuration.port != 0)
            args.insert(args.end(), {"--local-endpoint", result.endpoint});
        if (configuration.kind == wire::INTELLIGENCE)
            args.insert(
                args.end(),
                {"--node-id",
                    node_id_,
                    "--program-artifact",
                    configuration.artifact,
                    "--discovery-endpoint",
                    discovery_endpoint()});
        if (configuration.kind == wire::RESEARCH) {
            args.insert(
                args.end(),
                {"--config",
                    utf8(root_ / "config" / "services" / name / "research.json"),
                    "--node-id",
                    node_id_,
                    "--discovery-endpoint",
                    discovery_endpoint()});
        }
        if (configuration.kind == wire::MARKET || configuration.kind == wire::TRADING)
            args.insert(args.end(), {"--node-id", node_id_});
        if (configuration.kind == wire::MARKET && !configuration.provider_artifact.empty()) {
            const auto source = binary(configuration.provider_artifact);
            const auto library = root_ / "services" / name
                / (configuration.provider_artifact
                    + std::string(current_platform().os == "macos" ? ".dylib" : ".so"));
            require_managed_path(library);
            if (!fs::exists(library)) {
                if (sha256_file(source) != configuration.provider_artifact)
                    throw std::runtime_error("provider artifact integrity check failed");
                fs::copy_file(source, library);
            } else if (sha256_file(library) != configuration.provider_artifact)
                throw std::runtime_error("provider library changed");
            args.insert(args.end(), {"--ctp-library", utf8(library)});
        }
        if ((configuration.kind == wire::MARKET || configuration.kind == wire::TRADING)
            && !configuration.catalog_artifact.empty()) {
            const auto source = binary(configuration.catalog_artifact);
            const auto library = root_ / "services" / name
                / (configuration.catalog_artifact
                    + std::string(current_platform().os == "macos" ? ".dylib" : ".so"));
            require_managed_path(library);
            if (!fs::exists(library)) {
                if (sha256_file(source) != configuration.catalog_artifact)
                    throw std::runtime_error("catalog artifact integrity check failed");
                fs::copy_file(source, library);
            } else if (sha256_file(library) != configuration.catalog_artifact)
                throw std::runtime_error("catalog library changed");
            args.insert(
                args.end(),
                {configuration.kind == wire::TRADING ? "--ctp-library" : "--ctp-catalog-library",
                    utf8(library)});
        }
        if (local_)
            args.insert(args.end(), {"--endpoint", result.endpoint});
        else
            args.insert(
                args.end(),
                {"--bind",
                    bind_,
                    "--port",
                    std::to_string(configuration.port),
                    "--tls-ca",
                    tls_.ca_file,
                    "--tls-cert",
                    tls_.certificate_file,
                    "--tls-key",
                    tls_.private_key_file});
        result.process = std::make_unique<ChildProcess>(executable, args);
        return result;
    }

    PolledTask<void> start(const std::string& name, Service& s, bool automatic = false)
    {
        s.starting = true;

        struct Reservation
        {
            bool& starting;

            ~Reservation()
            {
                starting = false;
            }
        } reservation{s.starting};

        if (automatic && s.generation)
            ++s.restarts;
        co_await stop_processes(s);
        try {
            Started result;
            const auto configuration = s.configuration;
            co_await prepare([&] {
                result = prepare_start(name, configuration);
            });
            s.health_endpoint = std::move(result.health_endpoint);
            s.process = std::move(result.process);
            log_process_event(
                "agent",
                LogLevel::info,
                "service.started",
                {{"service", name}, {"pid", s.process->id()}});
            s.health = protocol::ServiceHealth::starting;
            s.last_heartbeat = 0;
            s.execution.reset();
            s.failures = 0;
            s.started = std::chrono::steady_clock::now();
            s.probe = s.started + 200ms;
            s.error.clear();
        } catch (const std::exception& e) {
            s.process.reset();
            s.error = e.what();
        }
        s.retry = std::chrono::steady_clock::now() + 5s;
    }

    PolledTask<void> save_upgrade(agent::UpgradePlan next)
    {
        co_await persist([path = root_ / "maintenance-plan.json",
                             bytes = agent::encode_upgrade_plan(next).dump()] {
            require_managed_path(path);
            replace_file_durably(path, bytes);
        });
        upgrade_ = std::move(next);
    }

    template <class Request, class Response>
    PolledTask<void> quiesce_service(const std::string& name, Service& s, bool stop)
    {
        Request request;
        request.set_version(1);
        request.set_service_id(name);
        request.set_correlation_id(unique_process_id());
        request.mutable_quiesce()->set_stop(stop);
        ipc::RpcClient channel(s.health_endpoint, 1, PayloadBudget{256 * 1024}, 64 * 1024);
        auto pending = channel.request(request.SerializeAsString(), 1500ms);
        co_await PollUntil{[&] {
            channel.poll();
            return pending.wait_for(0ms) == std::future_status::ready;
        }};
        Response response;
        if (!response.ParseFromString(*pending.get()))
            throw std::runtime_error("invalid service upgrade response");
        protocol::validate_message(response);
        if (response.version() != 1 || response.service_id() != name
            || response.correlation_id() != request.correlation_id())
            throw std::runtime_error("service upgrade response identity mismatch");
        if (response.has_error())
            throw std::runtime_error(response.error().message());
        if (!response.has_health())
            throw std::runtime_error("service upgrade control unavailable");
    }

    PolledTask<void> drain_upgrade()
    {
        if (phase_ != wire::Status::READY)
            co_return;
        upgrade_error_.clear();
        if (recovering_drain_) {
            for (const auto pid : upgrade_->processes)
                if (process_running(pid)) {
                    upgrade_error_ = "waiting for previous service processes to exit";
                    co_return;
                }
            recovering_drain_ = false;
        }
        bool ready = true;
        for (auto& [name, s] : services_) {
            if (s.process && s.process->exited()) {
                if (s.process->exit_code() != 0) {
                    upgrade_error_ = "service exited unsuccessfully during upgrade: " + name;
                    ready = false;
                    continue;
                }
                s.process.reset();
                s.health = protocol::ServiceHealth::offline;
            }
            if (!s.process) {
                continue;
            }
            ready = false;
            try {
                if (s.configuration.kind == wire::MARKET)
                    co_await quiesce_service<market::proto::Request, market::proto::Response>(
                        name,
                        s,
                        true);
                else if (s.configuration.kind == wire::INTELLIGENCE)
                    co_await quiesce_service<
                        intelligence::proto::IntelligenceRequest,
                        intelligence::proto::IntelligenceResponse>(name, s, true);
                else if (s.configuration.kind == wire::RESEARCH)
                    co_await quiesce_service<
                        research::proto::ResearchRequest,
                        task::proto::TaskResponse>(name, s, true);
                else if (s.configuration.kind == wire::DATA)
                    co_await quiesce_service<data::proto::DataRequest, data::proto::DataResponse>(
                        name,
                        s,
                        true);
            } catch (const std::exception& e) {
                upgrade_error_ = name + ": " + e.what();
            }
        }
        if (ready) {
            auto next = *upgrade_;
            next.phase = agent::UpgradePlan::Phase::ready;
            co_await save_upgrade(std::move(next));
        }
    }

    PolledTask<void> coordinate_upgrade(const wire::Upgrade& request, wire::UpgradeState& result)
    {
        validate_id(request.operation_id());
        if (request.action() != "prepare" && request.action() != "resume"
            && request.action() != "complete")
            throw std::invalid_argument("invalid upgrade action");
        using Phase = agent::UpgradePlan::Phase;
        if (upgrade_ && !upgrade_->active() && upgrade_->operation == request.operation_id()) {
            result.set_phase("complete");
            co_return;
        }
        if (upgrade_active() && upgrade_->operation != request.operation_id())
            throw std::runtime_error("another upgrade owns this node");
        if (!upgrade_active()) {
            if (request.action() != "prepare")
                throw std::runtime_error("upgrade plan is missing");
            if (!maintenance_.empty())
                throw std::runtime_error("Agent is already in maintenance");
            for (const auto& [name, s] : services_)
                if ((s.configuration.desired || s.process) && s.configuration.kind != wire::MARKET
                    && s.configuration.kind != wire::RESEARCH && s.configuration.kind != wire::DATA
                    && s.configuration.kind != wire::INTELLIGENCE)
                    throw std::runtime_error(
                        "service lacks an automatic upgrade recovery boundary: " + name);
            agent::UpgradePlan next;
            next.operation = request.operation_id();
            for (const auto& [name, s] : services_) {
                next.services.emplace(name, revision(s.configuration));
                if (s.process)
                    next.processes.push_back(s.process->id());
            }
            // The node mutation owns the freeze until the candidate is confirmed.
            co_await save_upgrade(std::move(next));
        }
        const auto phase = upgrade_->phase;
        if (request.action() == "resume" && phase == Phase::ready) {
            auto next = *upgrade_;
            next.phase = Phase::restoring;
            co_await save_upgrade(std::move(next));
            for (auto& [name, s] : services_)
                if (s.configuration.desired)
                    co_await start(name, s);
        } else if (request.action() == "complete") {
            if (phase != Phase::restoring)
                throw std::runtime_error("upgrade services have not resumed");
            for (const auto& [name, s] : services_)
                if (s.configuration.desired
                    && (!s.process
                        || (s.health != protocol::ServiceHealth::ready
                            && s.health != protocol::ServiceHealth::awaiting_input)))
                    throw std::runtime_error("upgrade is waiting for restored service health");
            auto next = *upgrade_;
            next.phase = Phase::complete;
            co_await save_upgrade(std::move(next));
            result.set_phase("complete");
            co_return;
        }
        result.set_phase(std::string(agent::upgrade_phase_name(upgrade_->phase)));
        result.set_detail(upgrade_error_);
    }

    struct ObservationIdentity
    {
        std::string name;
        std::uint64_t generation, pid;
    };

    ObservationIdentity observation_identity(const std::string& name, const Service& service)
    {
        // Program/plugin changes require a stopped service. Every stop/start
        // retires the process and advances generation before awaiting I/O.
        return {name, service.generation, service.process->id()};
    }

    Service* current_observation(const ObservationIdentity& observation)
    {
        if (stopping_ || mutating_service_ == observation.name || !maintenance_.empty()
            || (upgrade_active() && upgrade_->phase != agent::UpgradePlan::Phase::restoring))
            return nullptr;
        auto found = services_.find(observation.name);
        if (found == services_.end())
            return nullptr;
        auto& service = found->second;
        if (!service.configuration.desired || !service.process || service.process->exited()
            || service.generation != observation.generation
            || service.process->id() != observation.pid)
            return nullptr;
        return &service;
    }

    PolledTask<void> supervise(std::string name)
    {
        auto* service = &services_.at(name);
        if (service->process && service->process->exited()) {
            log_process_event(
                "agent",
                LogLevel::warning,
                "service.exited",
                {{"service", name}, {"exit_code", service->process->exit_code()}});
            co_await stop_processes(*service);
            service->error = "process exited; awaiting bounded restart";
        }
        if (service->process && std::chrono::steady_clock::now() >= service->probe) {
            service->probe = std::chrono::steady_clock::now() + 5s;
            const auto observation = observation_identity(name, *service);
            const auto kind = service->configuration.kind;
            const auto endpoint = service->health_endpoint;
            HealthObservation health;
            std::string error;
            try {
                health = co_await probe_service_health(kind, name, endpoint);
            } catch (const std::exception& e) {
                error = e.what();
            }
            service = current_observation(observation);
            if (!service)
                co_return;
            if (error.empty()) {
                service->health = health.status;
                service->execution = std::move(health.execution);
                service->execution_observed = std::chrono::steady_clock::now();
                if (service->health == protocol::ServiceHealth::starting)
                    service->probe = std::chrono::steady_clock::now() + 250ms;
                service->last_heartbeat = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                                              .count();
                service->failures = 0;
                service->error.clear();
            } else if (!service->last_heartbeat
                && std::chrono::steady_clock::now() - service->started < 10s) {
                service->probe = std::chrono::steady_clock::now() + 250ms;
                service->error = "waiting for the first heartbeat: " + error;
                co_return;
            } else {
                service->health = protocol::ServiceHealth::unresponsive;
                service->error = "service heartbeat unavailable";
                log_process_event(
                    "agent",
                    LogLevel::warning,
                    "service.heartbeat_failed",
                    {{"service", name},
                        {"message", error},
                        {"consecutive_failures", service->failures + 1}});
                if (++service->failures >= 3 && phase_ == wire::Status::READY) {
                    co_await stop_processes(*service);
                    service->retry = std::chrono::steady_clock::now() + 5s;
                }
            }
        }
        if (phase_ == wire::Status::READY && !stopping_ && !node_mutation_
            && name != mutating_service_ && service->configuration.desired && !service->process
            && service->restarts < 3 && std::chrono::steady_clock::now() >= service->retry) {
            co_await start(name, *service, true);
        }
    }

    struct Loaded
    {
        std::string node_id;
        std::map<std::string, Service> services;
        std::optional<agent::UpgradePlan> upgrade;
        std::string maintenance;
    };

    Loaded load()
    {
        Loaded loaded;
        if (!root_.is_absolute() || !fs::is_directory(root_))
            throw std::invalid_argument("agent requires an existing absolute directory");
        require_managed_path(root_);
        loaded.node_id = agent::node_storage_identity(root_);
        const auto maintenance_file = root_ / "maintenance.json";
        require_managed_path(maintenance_file);
        if (fs::exists(maintenance_file)) {
            std::ifstream input(maintenance_file);
            loaded.maintenance = Json::parse(input).at("operation").get<std::string>();
            validate_id(loaded.maintenance);
        }
        for (const auto* name : {"artifacts", "services", "uploads"}) {
            require_managed_path(root_ / name);
            fs::create_directory(root_ / name);
        }
        sync_directory(root_);
        for (const auto& entry : fs::directory_iterator(root_ / "services")) {
            require_managed_path(entry.path());
            const auto name = entry.path().filename().string();
            validate_service_id(name);
            Service s;
            s.configuration = load_service_configuration(entry.path(), local_, control_port_);
            plugins_.verify(s.configuration.plugin_artifacts);
            loaded.services.emplace(name, std::move(s));
        }
        const auto upgrade_path = root_ / "maintenance-plan.json";
        require_managed_path(upgrade_path);
        if (fs::exists(upgrade_path)) {
            if (fs::file_size(upgrade_path) > 65536)
                throw std::runtime_error("invalid upgrade plan size");
            std::ifstream file(upgrade_path);
            loaded.upgrade = agent::decode_upgrade_plan(
                parse_json(std::string(std::istreambuf_iterator<char>(file), {})));
            if (loaded.upgrade->active()) {
                const auto& frozen = loaded.upgrade->services;
                if (frozen.size() != loaded.services.size())
                    throw std::runtime_error("upgrade service configuration changed");
                for (const auto& [name, service] : loaded.services)
                    if (const auto found = frozen.find(name);
                        found == frozen.end() || found->second != revision(service.configuration))
                        throw std::runtime_error("upgrade service configuration changed");
            }
        }
        return loaded;
    }

    PolledTask<void> initialize()
    {
        Loaded loaded;
        try {
            co_await prepare([&] {
                initialization_progress_.begin();
                struct Complete
                {
                    Progress& progress;
                    ~Complete()
                    {
                        progress.finish();
                    }
                } complete{initialization_progress_};
                loaded = load();
            });
            node_id_ = std::move(loaded.node_id);
            services_ = std::move(loaded.services);
            upgrade_ = std::move(loaded.upgrade);
            maintenance_ = std::move(loaded.maintenance);
            recovering_drain_ =
                upgrade_active() && upgrade_->phase == agent::UpgradePlan::Phase::draining;
            phase_ = wire::Status::READY;
        } catch (const std::exception& error) {
            fail("Agent initialization failed; inspect node logs", error);
        }
    }

    // One admitted mutation at a time; its slot and ownership end with the request.
    struct Mutation
    {
        Agent& agent;
        bool admitted = false, owned = false;

        ~Mutation()
        {
            if (admitted)
                --agent.admitted_mutations_;
            if (owned) {
                agent.mutation_active_ = false;
                agent.node_mutation_ = false;
                agent.mutating_service_.clear();
            }
        }
    };

    PolledTask<void> admit_mutation(
        const wire::Request& r,
        Mutation& mutation,
        std::chrono::steady_clock::time_point deadline)
    {
        require_ready();
        if (admitted_mutations_ == 8)
            throw Error(ErrorCode::resource_exhausted, "Agent mutation capacity is full");
        ++admitted_mutations_;
        mutation.admitted = true;
        co_await PollUntil{[&] {
            return !mutation_active_ || stopping_ || std::chrono::steady_clock::now() >= deadline;
        }};
        if (stopping_ || std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("Agent request expired before execution");
        require_ready();
        mutation_active_ = mutation.owned = true;
        node_mutation_ = r.has_upgrade() || r.has_maintenance();
        if (r.has_action())
            mutating_service_ = r.action().service_id();
        else if (r.has_update())
            mutating_service_ = r.update().service_id();
        else if (r.has_configure_plugins())
            mutating_service_ = r.configure_plugins().service_id();
        // Await any launch/stop already owned by supervision before mutating that process.
        co_await PollUntil{[&] {
            return stopping_ || std::chrono::steady_clock::now() >= deadline
                || (!upgrading_
                    && (r.has_upgrade() || r.has_maintenance()
                            ? observations_.empty()
                            : !observations_.contains(mutating_service_)));
        }};
        if (stopping_ || std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("Agent request expired before execution");
        require_ready();
    }

    void maintain(const wire::Request& r)
    {
        const auto& request = r.maintenance();
        validate_id(request.operation_id());
        if (request.instance_id() != instance_)
            throw std::invalid_argument("Agent instance changed");
        if (request.enter()) {
            if (!maintenance_.empty() && maintenance_ != request.operation_id())
                throw std::runtime_error("Agent already belongs to another maintenance operation");
            for (const auto& [name, service] : services_)
                if (service.configuration.desired
                    || (service.process && !service.process->exited()))
                    throw std::runtime_error("stop all managed services before maintenance");
            replace_file_durably(
                root_ / "maintenance.json",
                Json{{"operation", request.operation_id()}}.dump());
            maintenance_ = request.operation_id();
        } else {
            if (maintenance_ != request.operation_id())
                throw std::runtime_error("maintenance operation does not match");
            fs::remove(root_ / "maintenance.json");
            sync_directory(root_);
            maintenance_.clear();
        }
    }

    PolledTask<void>
    manage_firewall(const wire::Request& r, const std::string& peer, wire::Response& response)
    {
        if (local_ || peer.empty())
            throw std::invalid_argument("firewall management requires a remote TLS node");
        const auto& operation = r.firewall();
        validate_service_id(operation.service_id());
        const auto port = services_.at(operation.service_id()).configuration.port;
        wire::FirewallPlan report;
        co_await firewall_.manage(operation, peer, port, report);
        *response.mutable_firewall() = std::move(report);
    }

    PolledTask<void> sample_resources()
    {
        wire::HostResources sample;
        sample.set_disk_path(utf8(root_));
        co_await prepare([&] {
            sample.set_sampled_at_ms(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());
            try {
                const auto observed = resource_monitor_.sample(root_);
                if (observed.cpu_usage_percent)
                    sample.set_cpu_usage_percent(*observed.cpu_usage_percent);
                sample.set_memory_available_bytes(observed.memory_available_bytes);
                sample.set_disk_available_bytes(observed.disk_available_bytes);
            } catch (const std::system_error& error) {
                // Observation failure must not change service supervision or admission.
                sample.mutable_failure()->set_code("unavailable");
                sample.mutable_failure()->set_message(
                    std::string("cannot sample node resources: ") + error.what());
            }
        });
        host_resources_ = std::move(sample);
    }

    void report_status(wire::Response& response)
    {
        auto* status = response.mutable_status();
        const auto platform = current_platform();
        status->set_instance_id(instance_);
        status->set_node_id(node_id_);
        if (local_)
            status->set_discovery_endpoint(discovery_endpoint());
        status->set_phase(phase_);
        if (phase_ == wire::Status::RECOVERY_REQUIRED)
            *status->mutable_failure() = failure_;
        auto* execution = status->mutable_execution();
        *execution->mutable_io() = protocol::encode_progress(io_progress_.observe());
        *execution->mutable_state() = protocol::encode_progress(state_progress_.observe());
        *execution->mutable_initialization() =
            protocol::encode_progress(initialization_progress_.observe());
        *execution->mutable_persistence() =
            protocol::encode_progress(persistence_progress_.observe());
        execution->set_business_ready(
            phase_ == wire::Status::READY && !stopping_ && !node_mutation_ && maintenance_.empty()
            && !upgrade_active());
        if (host_resources_)
            *status->mutable_host_resources() = *host_resources_;
        status->set_upgrade_protocol(1);
        status->set_maintenance(node_mutation_ || !maintenance_.empty() || upgrade_active());
        status->set_pid(current_process_id());
        status->set_os(platform.os);
        status->set_arch(platform.arch);
        status->set_version(ASTERION_PRODUCT_VERSION);
        status->set_uptime_ms(
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started_)
                    .count()));
        for (auto& [name, s] : services_) {
            auto* service = status->add_services();
            service->set_id(name);
            service->set_kind(s.configuration.kind);
            service->set_artifact(s.configuration.artifact);
            service->set_revision(revision(s.configuration));
            for (const auto& hash : s.configuration.plugin_artifacts)
                service->add_plugin_artifacts(hash);
            service->set_port(s.configuration.port);
            service->set_desired_running(s.configuration.desired);
            service->set_restarts(s.restarts);
            service->set_error(s.error);
            const bool running = s.process && !s.process->exited();
            service->set_state(
                s.retiring_pid                 ? "stopping"
                    : running                  ? "running"
                    : !s.configuration.desired ? "stopped"
                    : s.starting               ? "starting"
                    : s.restarts >= 3          ? "failed"
                                               : "restarting");
            service->set_pid(running ? s.process->id() : s.retiring_pid);
            service->set_health(
                std::string(
                    protocol::service_health_name(
                        running ? s.health : protocol::ServiceHealth::offline)));
            service->set_last_heartbeat_ms(s.last_heartbeat);
            if (running && s.execution) {
                *service->mutable_execution() = *s.execution;
                protocol::age_execution_health(
                    *service->mutable_execution(),
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - s.execution_observed)
                        .count());
                if (s.health != protocol::ServiceHealth::unresponsive
                    && protocol::execution_health_stalled(service->execution()))
                    service->set_health(
                        std::string(
                            protocol::service_health_name(protocol::ServiceHealth::degraded)));
            }
            if (local_) {
                service->set_endpoint(service_endpoint(name));
                service->set_directory(s.configuration.directory);
            }
        }
    }

    PolledTask<void> deploy_service(const wire::Request& r)
    {
        const auto& d = r.deploy();
        validate_service_id(d.service_id());
        validate_artifact_digest(d.sha256());
        if (d.kind() != wire::MARKET && d.kind() != wire::RESEARCH && d.kind() != wire::TRADING
            && d.kind() != wire::DATA && d.kind() != wire::INTELLIGENCE)
            throw std::invalid_argument("explicit service kind required");
        if (!d.provider_artifact().empty()) {
            if (d.kind() != wire::MARKET)
                throw std::invalid_argument("provider library only belongs to market data");
            validate_artifact_digest(d.provider_artifact());
            co_await verify_artifact(d.provider_artifact(), "provider artifact not installed");
        }
        if (d.kind() == wire::TRADING && d.catalog_artifact().empty())
            throw std::invalid_argument("live trading requires the CTP trader library");
        if (!d.catalog_artifact().empty()) {
            if (d.kind() != wire::MARKET && d.kind() != wire::TRADING)
                throw std::invalid_argument(
                    "CTP trader library only belongs to market data or live trading");
            validate_artifact_digest(d.catalog_artifact());
            co_await verify_artifact(d.catalog_artifact(), "catalog artifact not installed");
        }
        if (d.port() > 65535 || (local_ ? d.port() != 0 : (!d.port() || d.port() == control_port_)))
            throw std::invalid_argument("invalid service port");
        if (services_.contains(d.service_id()))
            throw std::invalid_argument(
                "service already exists; stop/start preserves its ledger; " "replacement is not supported");
        for (const auto& [name, s] : services_) {
            (void)name;
            if (d.port() && s.configuration.port == d.port())
                throw std::invalid_argument("port already assigned");
        }
        co_await verify_artifact(d.sha256(), "artifact not installed");
        const auto folder = root_ / "services" / d.service_id();
        Service s;
        s.configuration.kind = d.kind();
        if (d.kind() == wire::INTELLIGENCE && !d.plugin_artifacts().empty())
            throw std::invalid_argument("intelligence does not load native provider plugins");
        s.configuration.plugin_artifacts.assign(
            d.plugin_artifacts().begin(),
            d.plugin_artifacts().end());
        co_await prepare([&] {
            plugins_.verify(s.configuration.plugin_artifacts);
        });
        s.configuration.provider_artifact = d.provider_artifact();
        s.configuration.catalog_artifact = d.catalog_artifact();
        s.configuration.artifact = d.sha256();
        s.configuration.port = static_cast<unsigned short>(d.port());
        const bool ledger = s.configuration.kind == wire::TRADING;
        if (local_ && ledger && !d.directory().empty()) {
            const fs::path ledger(std::u8string(d.directory().begin(), d.directory().end()));
            co_await prepare([&] {
                if (!ledger.is_absolute() || !fs::is_directory(ledger))
                    throw std::invalid_argument(
                        "local ledger must be an existing absolute directory");
                require_managed_path(ledger);
                s.configuration.directory = utf8(fs::canonical(ledger));
            });
            for (const auto& [name, existing] : services_) {
                (void)name;
                if (existing.configuration.directory == s.configuration.directory)
                    throw std::invalid_argument("ledger already managed by another service");
            }
        } else {
            if (!d.directory().empty())
                throw std::invalid_argument("service directory is Agent-owned");
            s.configuration.directory = utf8(folder / "ledger");
        }
        co_await prepare([&] {
            if (s.configuration.kind == wire::RESEARCH) {
                const auto policy =
                    root_ / "config" / "services" / d.service_id() / "research.json";
                require_managed_path(policy);
                if (!fs::exists(policy)) {
                    const auto source = root_ / "config" / "research.json";
                    std::ifstream input(source, std::ios::binary);
                    if (!input)
                        throw std::runtime_error(
                            "cannot read research configuration template: " + source.string());
                    const std::string contents{std::istreambuf_iterator<char>(input), {}};
                    create_directories_durably(policy.parent_path());
                    replace_file_durably(policy, contents);
                }
            }
            if (!fs::create_directory(folder))
                throw std::invalid_argument("service directory already exists");
            sync_directory(folder.parent_path());
            if (!local_ || !ledger || d.directory().empty())
                create_directories_durably(folder / "ledger");
        });
        co_await save(d.service_id(), s.configuration);
        auto [it, added] = services_.emplace(d.service_id(), std::move(s));
        (void)added;
        mutating_service_ = it->first;
        co_await start(it->first, it->second);
    }

    PolledTask<void> update_service(const wire::Request& r)
    {
        const auto& u = r.update();
        validate_service_id(u.service_id());
        validate_artifact_digest(u.expected_revision());
        auto& current = services_.at(u.service_id());
        if (current.configuration.desired || (current.process && !current.process->exited()))
            throw std::invalid_argument("stop the service before updating its programs");
        if (revision(current.configuration) != u.expected_revision())
            throw std::invalid_argument("service configuration changed; inspect again");
        auto verified = [&](const std::string& hash) -> PolledTask<void> {
            co_await prepare([&] {
                validate_artifact_digest(hash);
                const auto path = binary(hash);
                require_managed_path(path);
                if (sha256_file(path) != hash)
                    throw std::invalid_argument("update artifact is missing or corrupted");
                const auto actual = artifact_platform(path), platform = current_platform();
                if (actual.os != platform.os || actual.arch != platform.arch)
                    throw std::invalid_argument("update artifact platform mismatch");
            });
        };
        co_await verified(u.artifact());
        if (current.configuration.kind == wire::TRADING && u.catalog_artifact().empty())
            throw std::invalid_argument("live trading requires the CTP trader library");
        if (!u.catalog_artifact().empty()) {
            if (current.configuration.kind != wire::MARKET
                && current.configuration.kind != wire::TRADING)
                throw std::invalid_argument(
                    "CTP trader library only belongs to market data or live trading");
            co_await verified(u.catalog_artifact());
        }
        if (current.configuration.kind == wire::MARKET) {
            if (!u.provider_artifact().empty())
                co_await verified(u.provider_artifact());
        } else if (!u.provider_artifact().empty())
            throw std::invalid_argument("provider library only belongs to market data");
        auto next = current.configuration;
        next.plugin_artifacts.assign(u.plugin_artifacts().begin(), u.plugin_artifacts().end());
        co_await prepare([&] {
            plugins_.verify(next.plugin_artifacts);
        });
        next.desired = false;
        next.artifact = u.artifact();
        next.provider_artifact = u.provider_artifact();
        next.catalog_artifact = u.catalog_artifact();
        // Publish metadata before changing the in-memory selection. No ledger
        // rewrite, automatic restart, or implicit binary rollback occurs.
        co_await save(u.service_id(), next);
        current.process.reset();
        current.configuration = std::move(next);
        current.restarts = 0;
        current.error.clear();
        current.health = protocol::ServiceHealth::offline;
        current.last_heartbeat = 0;
        current.execution.reset();
    }

    PolledTask<void> act_on_service(const wire::Request& r)
    {
        const auto& a = r.action();
        validate_service_id(a.service_id());
        const auto found = services_.find(a.service_id());
        if (found == services_.end())
            throw Error(ErrorCode::not_found, "service is not deployed");
        auto& s = found->second;
        if (a.kind() != wire::Action::START && a.kind() != wire::Action::STOP
            && a.kind() != wire::Action::RESTART)
            throw std::invalid_argument("unknown service action");
        const bool desired = a.kind() != wire::Action::STOP;
        auto next = s.configuration;
        next.desired = desired;
        co_await save(a.service_id(), next);
        s.configuration = std::move(next);
        if (!desired) {
            co_await stop_processes(s);
            s.error.clear();
        } else if (a.kind() == wire::Action::RESTART || !s.process || s.process->exited()) {
            s.restarts = 0;
            co_await start(a.service_id(), s);
        }
    }

public:
    std::string discovery_endpoint() const
    {
        return utf8(sockets_ / "discovery.sock");
    }

    wire::Response discover(const wire::Request& request)
    {
        wire::Response response;
        response.set_version(1);
        response.set_correlation_id(request.correlation_id());
        try {
            protocol::validate_message(request);
            validate_id(request.correlation_id());
            if (request.version() != 1 || !request.has_status())
                throw Error(ErrorCode::permission_denied, "service discovery is read only");
            report_status(response);
            // This socket is private to the node. A TCP deployment's public local
            // socket is intentionally not part of its remote management view.
            for (auto& service : *response.mutable_status()->mutable_services())
                if (service.kind() == wire::DATA || service.kind() == wire::RESEARCH
                    || service.kind() == wire::INTELLIGENCE)
                    service.set_endpoint(service_endpoint(service.id()));
        } catch (const std::exception& error) {
            response.clear_status();
            response.mutable_error()->set_code(std::string(error_name(classify(error))));
            response.mutable_error()->set_message(error.what());
        }
        return response;
    }

    Agent(
        fs::path root,
        ipc::TlsIdentity tls,
        std::string bind,
        unsigned short control_port,
        Progress& io_progress)
        : root_(std::move(root)),
          tls_(std::move(tls)),
          bind_(std::move(bind)),
          local_(bind_.empty()),
          control_port_(control_port),
          io_progress_(io_progress)
    {
        sockets_ = fs::path("/tmp") / ("ast-" + instance_.substr(0, 12));
        if (!fs::create_directory(sockets_))
            throw std::runtime_error("cannot create private service socket directory");
        fs::permissions(sockets_, fs::perms::owner_all);
        initializing_.emplace(initialize());
    }

    void advance(bool stopping)
    {
        state_progress_.finish();
        stopping_ = stopping;
        if (initializing_ && initializing_->poll()) {
            initializing_->take();
            initializing_.reset();
        }
        if (sampling_ && sampling_->poll()) {
            sampling_->take();
            sampling_.reset();
        }
        const auto sample_now = std::chrono::steady_clock::now();
        if (!initializing_ && !stopping_ && !sampling_ && sample_now >= next_sample_) {
            next_sample_ = sample_now + 5s;
            sampling_.emplace(sample_resources());
        }
        for (auto it = observations_.begin(); it != observations_.end();) {
            if (!it->second.poll()) {
                ++it;
                continue;
            }
            it->second.take();
            it = observations_.erase(it);
        }
        if (upgrading_ && upgrading_->poll()) {
            try {
                upgrading_->take();
            } catch (const std::exception& e) {
                upgrade_error_ = e.what();
            }
            upgrading_.reset();
        }
        if (initializing_ || stopping_ || !maintenance_.empty() || node_mutation_)
            return;
        const auto now = std::chrono::steady_clock::now();
        if (now < next_supervision_)
            return;
        next_supervision_ = now + 200ms;
        if (upgrade_active() && upgrade_->phase != agent::UpgradePlan::Phase::restoring) {
            if (phase_ == wire::Status::READY && !mutation_active_ && !upgrading_
                && observations_.empty() && upgrade_->phase == agent::UpgradePlan::Phase::draining)
                upgrading_.emplace(drain_upgrade());
            return;
        }
        auto next = services_.upper_bound(last_supervised_);
        for (std::size_t visited = 0; visited < services_.size() && observations_.size() < 8;
            ++visited) {
            if (next == services_.end())
                next = services_.begin();
            const auto name = next++->first;
            last_supervised_ = name;
            if (name != mutating_service_ && !observations_.contains(name))
                observations_.emplace(name, supervise(name));
        }
    }

    bool background_idle() const
    {
        return observations_.empty() && !upgrading_ && !initializing_ && !sampling_;
    }

    PolledTask<void> shutdown()
    {
        co_await PollUntil{[&] {
            return background_idle();
        }};
        // Stop Data consumers before their source services.
        for (const auto kind :
            {wire::RESEARCH, wire::INTELLIGENCE, wire::TRADING, wire::MARKET, wire::DATA})
            for (auto& [name, service] : services_)
                if (service.configuration.kind == kind)
                    co_await stop_processes(service);
    }

    ~Agent()
    {
        services_.clear();
        if (!sockets_.empty()) {
            std::error_code ec;
            fs::remove_all(sockets_, ec);
        }
    }

    PolledTask<wire::Response> dispatch(
        wire::Request r,
        std::string peer,
        ipc::PeerRole role,
        std::chrono::steady_clock::time_point deadline)
    {
        Mutation mutation{*this};
        wire::Response response;
        response.set_version(1);
        response.set_correlation_id(r.correlation_id());
        try {
            if (stopping_ || std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("Agent request expired before execution");
            protocol::validate_message(r);
            validate_id(r.correlation_id());
            if (r.version() != 1)
                throw std::invalid_argument("unsupported node protocol");
            // Anyone the node CA certified may read status; everything that installs,
            // runs, reconfigures or exposes programs needs the admin certificate.
            if (!r.has_status() && role != ipc::PeerRole::admin && role != ipc::PeerRole::local)
                throw Error(
                    ErrorCode::permission_denied,
                    "this operation requires the node administrator certificate");
            if (!r.has_status())
                co_await admit_mutation(r, mutation, deadline);
            if (r.has_upgrade()) {
                co_await coordinate_upgrade(r.upgrade(), *response.mutable_upgrade());
                co_return response;
            }
            if (upgrade_active() && !r.has_status())
                throw std::runtime_error(
                    "Agent upgrade is coordinating services; mutations are disabled");
            if (r.has_maintenance()) {
                maintain(r);
                response.mutable_accepted();
                co_return response;
            }
            if (!maintenance_.empty() && !r.has_status())
                throw std::runtime_error("Agent is in maintenance; mutations are disabled");
            if (r.has_firewall()) {
                co_await manage_firewall(r, peer, response);
                co_return response;
            }
            if (r.has_status()) {
                report_status(response);
                co_return response;
            }
            if (r.has_upload())
                co_await uploads_.begin(r.upload());
            else if (r.has_chunk())
                co_await uploads_.append(r.chunk());
            else if (r.has_finish())
                co_await uploads_.finish(r.finish());
            else if (r.has_deploy())
                co_await deploy_service(r);
            else if (r.has_configure_plugins())
                co_await configure_plugins(r.configure_plugins());
            else if (r.has_update())
                co_await update_service(r);
            else if (r.has_action())
                co_await act_on_service(r);
            else
                throw std::invalid_argument("missing node operation");
            response.mutable_accepted();
        } catch (const std::exception& error) {
            response.mutable_error()->set_code(std::string(error_name(classify(error))));
            response.mutable_error()->set_message(error.what());
        }
        co_return response;
    }
};

} // namespace

int main(int argc, char** argv)
{
    CLI::App app{"Asterion Node Agent: trusted remote deployment and supervision"};
    app.set_version_flag("--version", "Asterion Node Agent " ASTERION_PRODUCT_VERSION);
    std::string directory, inspect, inspect_artifact, program_manifest;
    service::Transport transport;
    auto* inspect_option =
        app.add_option("--inspect-plugin", inspect, "Print a native plugin's identity and exit");
    auto* artifact_option =
        app.add_option("--plugin-artifact", inspect_artifact, "Expected inspected plugin identity");
    inspect_option->needs(artifact_option);
    artifact_option->needs(inspect_option);
    app.add_option("--directory", directory)
        ->check(CLI::ExistingDirectory)
        ->excludes(inspect_option);
    app.add_option(
           "--prepare-programs",
           program_manifest,
           "Synchronize service programs offline and exit")
        ->excludes(inspect_option);
    app.add_option("--endpoint", transport.endpoint);
    app.add_option("--bind", transport.bind);
    app.add_option("--port", transport.port)->check(CLI::Range(1, 65535));
    app.add_option("--tls-ca", transport.tls.ca_file);
    app.add_option("--tls-cert", transport.tls.certificate_file);
    app.add_option("--tls-key", transport.tls.private_key_file);
    argv = app.ensure_utf8(argv);
    CLI11_PARSE(app, argc, argv);
    if (!inspect.empty())
        return asterion::agent::inspect_plugin(inspect, inspect_artifact);
    if (directory.empty()) {
        std::cerr << "--directory is required\n";
        return 2;
    }
    auto run = [&]() -> int {
        try {
            service::install_stop_signals();
            const fs::path root(std::u8string(directory.begin(), directory.end()));
            if (!root.is_absolute() || !fs::is_directory(root))
                throw std::invalid_argument("agent requires an existing absolute directory");
            require_managed_path(root);
            const auto logs = root / "logs";
            require_managed_path(logs);
            fs::create_directory(logs);
            ::setenv("ASTERION_LOG_DIRECTORY", logs.c_str(), 1);
            initialize_process_logging("agent");
            log_process_event(
                "agent",
                LogLevel::info,
                "agent.started",
                {{"mode", program_manifest.empty() ? "supervision" : "program-preparation"}});
            std::uint64_t test_owner = 0;
            if (environment_variable("ASTERION_TEST_NODE_ISOLATED") == "1")
                if (const auto owner = environment_variable("ASTERION_TEST_OWNER_PID"))
                    test_owner = std::stoull(*owner);
            if (!program_manifest.empty()) {
                asterion::agent::prepare_service_programs(
                    fs::path(directory),
                    fs::path(program_manifest),
                    transport.bind.empty(),
                    transport.port);
                return 0;
            }
            transport.validate();
            asterion::FileLock ownership(root, "agent.lock");
            prune_process_logs(logs);
            if (!transport.remote()) {
                // Only the Agent holding agent.lock owns this path; a stale socket
                // from a crashed predecessor is replaced.
                require_managed_path(transport.endpoint);
                fs::remove(transport.endpoint);
            }
            const auto pid_file = root / "agent.pid";
            require_managed_path(pid_file);
            {
                std::ofstream pid(pid_file);
                pid << current_process_id();
            }
            Progress io_progress;
            std::unique_ptr<Agent> agent;
            using Host = service::RpcHost;
            auto serve = [&](wire::Request request, Host::Peer peer) -> PolledTask<std::string> {
                auto response = co_await agent->dispatch(
                    request,
                    std::move(peer.address),
                    peer.role,
                    std::chrono::steady_clock::now() + 10s);
                protocol::log_rpc_result("agent", request, response, request.has_status());
                co_return response.SerializeAsString();
            };
            auto handle = [&](const Host::Peer& peer, std::string frame) -> Host::Reply {
                wire::Request request;
                if (!request.ParseFromString(frame))
                    throw std::invalid_argument("malformed Protobuf request");
                auto operation =
                    std::make_shared<PolledTask<std::string>>(serve(std::move(request), peer));
                return [operation]() -> std::optional<std::string> {
                    if (!operation->poll())
                        return {};
                    return operation->take();
                };
            };
            std::optional<PolledTask<void>> stopping;
            Host::Options options;
            options.connections = 16;
            // Uploads carry at most 1 MiB of data plus the Protobuf envelope. Agent
            // does not receive Task datasets and must not reserve their 64 MiB frames.
            options.request_bytes = 2 * 1024 * 1024;
            options.payload_bytes = 8 * 1024 * 1024;
            options.receive = 10s;
            options.drain = 5s;
            auto test_owner_probe = std::chrono::steady_clock::now();
            options.advance = [&](Host::Stage stage) {
                if (test_owner && std::chrono::steady_clock::now() >= test_owner_probe) {
                    test_owner_probe = std::chrono::steady_clock::now() + 500ms;
                    if (!process_running(test_owner))
                        service::request_stop();
                }
                agent->advance(stage != Host::Stage::running);
                if (stage != Host::Stage::stopping_resources)
                    return false;
                if (!stopping)
                    stopping.emplace(agent->shutdown());
                if (!stopping->poll())
                    return false;
                stopping->take();
                return true;
            };
            // Bind before starting children; one I/O owner advances requests and supervision.
            agent = std::make_unique<Agent>(
                root,
                transport.tls,
                transport.bind,
                transport.port,
                io_progress);
            options.local_endpoints.push_back(
                {agent->discovery_endpoint(),
                    [&](const Host::Peer&, std::string frame) -> Host::Reply {
                        wire::Request request;
                        if (!request.ParseFromString(frame))
                            throw std::invalid_argument("malformed discovery request");
                        auto response = agent->discover(request).SerializeAsString();
                        return [response = std::move(response)] {
                            return std::optional<std::string>(response);
                        };
                    },
                    4,
                    false,
                    65536,
                    4 * 1024 * 1024});
            Host host(
                transport,
                [&](const Host::Peer& peer, std::string frame) {
                    return handle(peer, std::move(frame));
                },
                std::move(options),
                io_progress);
            try {
                if (!host.run())
                    std::_Exit(0);
            } catch (const std::exception& error) {
                // Suspended requests may still be borrowed by file or process work.
                // A fatal loop failure must not destroy those frames under the pool.
                log_process_event(
                    "agent",
                    LogLevel::error,
                    "agent.failed",
                    {{"message", error.what()}});
                std::_Exit(1);
            }

        } catch (const std::exception& error) {
            std::cerr << "Node failed: " << error.what() << '\n';
            log_process_event(
                "agent",
                LogLevel::error,
                "agent.failed",
                {{"message", error.what()}});
            return 1;
        }
        return 0;
    };
    return run();
}
