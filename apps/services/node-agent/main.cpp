#include "development_programs.hpp"
#include <asterion/protocol/health.hpp>
#include <set>
#include "plugin_artifacts.hpp"
#include "service_configuration.hpp"
#include "resource_budget.hpp"
#include "supervision_rpc.hpp"
#include <asterion/v1/data_service.pb.h>
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
#include <asterion/kernel/thread_pool.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/v1/market.pb.h>
#include <asterion/v1/node.pb.h>
#include <asterion/v1/task.pb.h>
#include <atomic>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif
namespace fs = std::filesystem;
namespace wire = asterion::node::v1;
using namespace asterion;
using namespace std::chrono_literals;
namespace {
using namespace asterion::agent;
std::string utf8(const fs::path& p) {
  const auto s = p.u8string();
  return {s.begin(), s.end()};
}
void encode_resources(wire::ResourceUse& result, const Resources& resources) {
  result.set_cpu_slots(resources.cpu);
  result.set_memory_mib(resources.memory_mib);
  result.set_io_slots(resources.io);
}
struct Service {
  ServiceConfiguration configuration;
  std::string worker_endpoint;
  std::map<std::string, std::unique_ptr<ChildProcess>> workers;
  std::chrono::steady_clock::time_point dispatch_at{};
  std::string error, health_endpoint, health = "starting";
  std::optional<runtime::v1::ExecutionHealth> execution;
  std::chrono::steady_clock::time_point execution_observed{};
  std::int64_t last_heartbeat = 0;
  unsigned failures = 0;
  std::chrono::steady_clock::time_point probe{};
  // When the current process was started; probes repeat quickly until the
  // first heartbeat so a healthy service is reported within moments.
  std::chrono::steady_clock::time_point started{};
  unsigned int restarts = 0;
  std::uint64_t generation = 0, retiring_pid = 0;
  unsigned retiring_workers = 0;
  bool starting = false, waiting_capacity = false;
  std::unique_ptr<ChildProcess> process;
  std::chrono::steady_clock::time_point retry{};
};
class Agent {
  Json firewall_plan_ = nullptr;
  std::chrono::steady_clock::time_point firewall_expiry_{};
  fs::path root_;
  PluginArtifacts plugins_{root_, current_executable()};
  ipc::TlsIdentity tls_;
  std::string bind_;
  fs::path sockets_;
  bool local_;
  unsigned short control_port_;
  std::map<std::string, Service> services_;
  struct Upload {
    std::uint64_t size, offset = 0;
  };
  std::map<std::string, Upload> uploads_;
  std::string maintenance_;
  Json upgrade_ = nullptr;
  std::string upgrade_error_;
  wire::Status::Phase phase_ = wire::Status::INITIALIZING;
  wire::Error failure_;
  Progress& io_progress_;
  Progress state_progress_, initialization_progress_, persistence_progress_;
  bool recovering_drain_ = false;
  bool upgrade_active() const { return !upgrade_.is_null() && upgrade_.at("phase") != "complete"; }
  ThreadPool journal_{1, 8}, operations_{2, 8};
  bool mutation_active_ = false, node_mutation_ = false, stopping_ = false;
  std::size_t admitted_mutations_ = 0;
  std::string last_supervised_, last_dispatched_;
  std::string mutating_service_;
  // This is one node-wide execution ceiling, independent of Task's queue order.
  static constexpr unsigned worker_limit = 2;
  unsigned worker_reservations_ = 0;
  std::optional<ResourceBudget> resource_budget_;
  std::map<std::string, PolledTask<void>> observations_;
  std::optional<PolledTask<void>> upgrading_, initializing_;
  std::chrono::steady_clock::time_point next_supervision_{};
  std::size_t preparing_ = 0;
  template <class F> PolledTask<void> prepare(F work) {
    co_await PollUntil{[&] { return preparing_ < 8; }};
    struct Slot {
      std::size_t& count;
      explicit Slot(std::size_t& value) : count(value) { ++count; }
      ~Slot() { --count; }
    } slot(preparing_);
    co_await PollFuture{operations_.submit([work = std::move(work)](std::stop_token) { work(); })};
  }
  PolledTask<void> stop_processes(Service& service) {
    // Transfer child ownership before waiting; no pool thread touches Service.
    service.retiring_pid = service.process ? service.process->id() : 0;
    service.retiring_workers = static_cast<unsigned>(service.workers.size());
    auto workers = std::move(service.workers);
    auto process = std::move(service.process);
    ++service.generation;
    co_await prepare([&] {
      for (auto& [id, child] : workers)
        child->request_stop();
      workers.clear();
      if (process)
        process->request_stop();
      process.reset();
    });
    service.retiring_pid = 0;
    service.retiring_workers = 0;
  }
  const std::string instance_ = unique_process_id();
  const std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
  fs::path binary(const std::string& hash) const {
    return root_ / "artifacts" / (hash + (current_platform().os == "windows" ? ".exe" : ".bin"));
  }
  std::string revision(const ServiceConfiguration& configuration) const {
    return service_revision(configuration);
  }
  void require_ready() const {
    if (phase_ != wire::Status::READY)
      throw Error(phase_ == wire::Status::INITIALIZING ? ErrorCode::unavailable
                                                       : ErrorCode::recovery_required,
                  phase_ == wire::Status::INITIALIZING ? "Agent is initializing"
                                                       : "Agent recovery is required");
  }
  void fail(const char* diagnostic, const std::exception& error) {
    phase_ = wire::Status::RECOVERY_REQUIRED;
    failure_.set_code(std::string(error_name(classify(error))));
    failure_.set_message(std::string(diagnostic) + ": " + error.what());
    log_process_event("agent", LogLevel::error, "agent.recovery_required",
                      {{"message", diagnostic}, {"cause", error.what()}});
  }
  template <class F> PolledTask<void> persist(F work) {
    try {
      co_await PollFuture{journal_.submit([this, work = std::move(work)](std::stop_token) {
        persistence_progress_.begin();
        struct Complete {
          Progress& progress;
          ~Complete() { progress.finish(); }
        } complete{persistence_progress_};
        work();
      })};
    } catch (const std::exception& error) {
      fail("Agent configuration commit failed; inspect node logs", error);
      throw;
    }
  }
  PolledTask<void> save(std::string name, ServiceConfiguration configuration) {
    co_await persist(
        [folder = root_ / "services" / name, configuration = std::move(configuration)] {
          save_service_configuration(folder, configuration);
        });
  }
  PolledTask<void> verify_artifact(const std::string& hash, const char* diagnostic) {
    co_await prepare([&] {
      if (sha256_file(binary(hash)) != hash)
        throw std::invalid_argument(diagnostic);
    });
  }
  // A peer may be deployed later, but its identity is already reserved by the
  // first member of the pair. No process needs to wait for its peer to start.
  static void validate_binding(const std::map<std::string, Service>& services,
                               const std::string& name, wire::ServiceKind kind,
                               const std::string& task, const std::string& data) {
    for (const auto& [other_name, other] : services) {
      if (other_name == name)
        continue;
      const auto& c = other.configuration;
      if ((kind == wire::TASK_SERVICE && data == other_name &&
           (c.kind != wire::DATA_SERVICE || c.task_service != name)) ||
          (kind == wire::DATA_SERVICE && task == other_name &&
           (c.kind != wire::TASK_SERVICE || c.data_service != name)) ||
          (c.kind == wire::TASK_SERVICE && c.data_service == name &&
           (kind != wire::DATA_SERVICE || task != other_name)) ||
          (c.kind == wire::DATA_SERVICE && c.task_service == name &&
           (kind != wire::TASK_SERVICE || data != other_name)) ||
          (kind == wire::TASK_SERVICE && c.kind == kind && c.data_service == data) ||
          (kind == wire::DATA_SERVICE && c.kind == kind && c.task_service == task))
        throw std::invalid_argument("data and task service bindings disagree");
    }
  }
  PolledTask<void> configure_plugins(const wire::ConfigurePlugins& request) {
    validate_service_id(request.service_id());
    validate_artifact_digest(request.expected_revision());
    auto& current = services_.at(request.service_id());
    if (current.configuration.kind != wire::TASK_SERVICE &&
        current.configuration.kind != wire::DATA_SERVICE)
      throw std::invalid_argument("service does not support managed native plugins");
    if (current.configuration.desired || (current.process && !current.process->exited()) ||
        !current.workers.empty())
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
    current.health = "offline";
    current.last_heartbeat = 0;
    current.execution.reset();
  }
  struct Started {
    std::string endpoint, health_endpoint, worker_endpoint;
    std::unique_ptr<ChildProcess> process;
  };
  std::string service_endpoint(const std::string& name) const {
#ifdef _WIN32
    return "asterion." + instance_ + "." + name;
#else
    return utf8(sockets_ / (name + ".sock"));
#endif
  }
  Started prepare_start(const std::string& name, const ServiceConfiguration& configuration) {
    Started result;
    const auto executable = binary(configuration.artifact);
    if (sha256_file(executable) != configuration.artifact)
      throw std::runtime_error("artifact integrity check failed");
    result.endpoint = service_endpoint(name);
    result.health_endpoint = utf8(sockets_ / (name + ".health"));
    result.worker_endpoint = utf8(sockets_ / (name + ".workers"));
#ifdef _WIN32
    result.health_endpoint = result.endpoint + ".health";
    result.worker_endpoint = result.endpoint + ".workers";
#else
    // Only this locked Agent owns these ephemeral socket paths.
    fs::remove(result.endpoint);
    fs::remove(result.health_endpoint);
    fs::remove(result.worker_endpoint);
#endif
    std::vector<std::string> args{"--owner-pid",       std::to_string(current_process_id()),
                                  "--session",         name,
                                  "--directory",       configuration.directory,
                                  "--health-endpoint", result.health_endpoint};
    if (configuration.kind == wire::TASK_SERVICE || configuration.kind == wire::LIVE_TRADING ||
        configuration.kind == wire::DATA_SERVICE)
      args.insert(args.end(), {"--plugin-directory",
                               utf8(plugins_.materialize(name, configuration.plugin_artifacts))});
    if (configuration.kind == wire::DATA_SERVICE)
      args.insert(args.end(), {"--worker-endpoint", result.worker_endpoint, "--task-instance",
                               configuration.task_service});
    if (configuration.kind == wire::DATA_SERVICE || configuration.kind == wire::TASK_SERVICE)
      args.insert(args.end(), {"--file-workers", std::to_string(resource_budget_->file_workers)});
    if (configuration.kind == wire::TASK_SERVICE) {
      if (sha256_file(binary(configuration.data_artifact)) != configuration.data_artifact)
        throw std::runtime_error("data worker integrity check failed");
      if (sha256_file(binary(configuration.factor_artifact)) != configuration.factor_artifact)
        throw std::runtime_error("factor worker integrity check failed");
      if (sha256_file(binary(configuration.worker_artifact)) != configuration.worker_artifact)
        throw std::runtime_error("backtest worker integrity check failed");
      args.insert(args.end(), {"--worker-endpoint", result.worker_endpoint, "--data-instance",
                               configuration.data_service, "--data-endpoint",
                               utf8(sockets_ / (configuration.data_service + ".workers"))});
    }
    if (configuration.kind == wire::MARKET_DATA && !configuration.provider_artifact.empty()) {
      const auto source = binary(configuration.provider_artifact);
      if (sha256_file(source) != configuration.provider_artifact)
        throw std::runtime_error("provider artifact integrity check failed");
      const auto library = root_ / "services" / name /
                           (configuration.provider_artifact +
                            std::string(current_platform().os == "windows" ? ".dll"
                                        : current_platform().os == "macos" ? ".dylib"
                                                                           : ".so"));
      require_managed_path(library);
      if (!fs::exists(library))
        fs::copy_file(source, library);
      else if (sha256_file(library) != configuration.provider_artifact)
        throw std::runtime_error("provider library changed");
      args.insert(args.end(), {"--ctp-library", utf8(library)});
    }
    if ((configuration.kind == wire::MARKET_DATA || configuration.kind == wire::LIVE_TRADING) &&
        !configuration.catalog_artifact.empty()) {
      const auto source = binary(configuration.catalog_artifact);
      if (sha256_file(source) != configuration.catalog_artifact)
        throw std::runtime_error("catalog artifact integrity check failed");
      const auto library = root_ / "services" / name /
                           (configuration.catalog_artifact +
                            std::string(current_platform().os == "macos"     ? ".dylib"
                                        : current_platform().os == "windows" ? ".dll"
                                                                             : ".so"));
      require_managed_path(library);
      if (!fs::exists(library))
        fs::copy_file(source, library);
      else if (sha256_file(library) != configuration.catalog_artifact)
        throw std::runtime_error("catalog library changed");
      args.insert(args.end(), {configuration.kind == wire::LIVE_TRADING ? "--ctp-library"
                                                                        : "--ctp-catalog-library",
                               utf8(library)});
    }
    if (local_)
      args.insert(args.end(), {"--endpoint", result.endpoint});
    else
      args.insert(args.end(), {"--bind", bind_, "--port", std::to_string(configuration.port),
                               "--tls-ca", tls_.ca_file, "--tls-cert", tls_.certificate_file,
                               "--tls-key", tls_.private_key_file});
    result.process = std::make_unique<ChildProcess>(executable, args);
    return result;
  }
  PolledTask<void> start(const std::string& name, Service& s, bool automatic = false) {
    const auto extra = s.process || s.retiring_pid || s.starting
                           ? Resources{}
                           : resource_budget_->service(s.configuration.kind);
    if (!resource_budget_->fits(resource_usage(), extra)) {
      s.waiting_capacity = true;
      s.error = "Node resource capacity is full; service is waiting";
      s.retry = std::chrono::steady_clock::now() + 1s;
      co_return;
    }
    s.waiting_capacity = false;
    s.starting = true;
    struct Reservation {
      bool& starting;
      ~Reservation() { starting = false; }
    } reservation{s.starting};
    // Waiting for capacity is not a failed process start and consumes no retry.
    if (automatic && s.generation)
      ++s.restarts;
    co_await stop_processes(s);
    try {
      Started result;
      const auto configuration = s.configuration;
      co_await prepare([&] { result = prepare_start(name, configuration); });
      s.health_endpoint = std::move(result.health_endpoint);
      s.worker_endpoint = std::move(result.worker_endpoint);
      s.process = std::move(result.process);
      log_process_event("agent", LogLevel::info, "service.started",
                        {{"service", name}, {"pid", s.process->id()}});
      s.health = "starting";
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
  PolledTask<void> save_upgrade(Json next) {
    co_await persist([path = root_ / "maintenance-plan.json", bytes = next.dump()] {
      require_managed_path(path);
      replace_file_durably(path, bytes);
    });
    upgrade_ = std::move(next);
  }
  template <class Request, class Response>
  PolledTask<void> quiesce_service(const std::string& name, Service& s, bool stop) {
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
    if (response.version() != 1 || response.service_id() != name ||
        response.correlation_id() != request.correlation_id())
      throw std::runtime_error("service upgrade response identity mismatch");
    if (response.has_error())
      throw std::runtime_error(response.error().message());
    if (!response.has_health())
      throw std::runtime_error("service upgrade control unavailable");
  }
  PolledTask<void> drain_upgrade() {
    if (phase_ != wire::Status::READY)
      co_return;
    upgrade_error_.clear();
    if (recovering_drain_) {
      for (const auto& pid : upgrade_.at("processes"))
        if (process_running(pid.get<std::uint64_t>())) {
          upgrade_error_ = "waiting for previous service processes to exit";
          co_return;
        }
      recovering_drain_ = false;
    }
    bool ready = true;
    for (auto& [name, s] : services_) {
      for (auto it = s.workers.begin(); it != s.workers.end();) {
        if (it->second->exited())
          it = s.workers.erase(it);
        else
          ++it;
      }
      if (s.process && s.process->exited()) {
        if (s.process->exit_code() != 0) {
          upgrade_error_ = "service exited unsuccessfully during upgrade: " + name;
          ready = false;
          continue;
        }
        s.process.reset();
        s.health = "offline";
      }
      if (!s.process) {
        if (!s.workers.empty())
          ready = false;
        continue;
      }
      ready = false;
      try {
        if (s.configuration.kind == wire::MARKET_DATA)
          co_await quiesce_service<market::v1::Request, market::v1::Response>(name, s, true);
        else if (s.configuration.kind == wire::TASK_SERVICE)
          co_await quiesce_service<task::v1::TaskRequest, task::v1::TaskResponse>(
              name, s, s.workers.empty());
        else if (s.configuration.kind == wire::DATA_SERVICE) {
          const auto peer = services_.find(s.configuration.task_service);
          const bool task_stopped =
              peer == services_.end() || (!peer->second.process && peer->second.workers.empty());
          if (task_stopped)
            co_await quiesce_service<data::v1::DataRequest, data::v1::DataResponse>(name, s, true);
        }
      } catch (const std::exception& e) {
        upgrade_error_ = name + ": " + e.what();
      }
    }
    if (ready) {
      auto next = upgrade_;
      next["phase"] = "ready";
      co_await save_upgrade(std::move(next));
    }
  }
  PolledTask<void> coordinate_upgrade(const wire::Upgrade& request, wire::UpgradeState& result) {
    validate_id(request.operation_id());
    if (request.action() != "prepare" && request.action() != "resume" &&
        request.action() != "complete")
      throw std::invalid_argument("invalid upgrade action");
    if (!upgrade_.is_null() && upgrade_.at("phase") == "complete") {
      if (upgrade_.at("operation") == request.operation_id()) {
        result.set_phase("complete");
        co_return;
      }
    }
    if (upgrade_active() && upgrade_.at("operation") != request.operation_id())
      throw std::runtime_error("another upgrade owns this node");
    if (!upgrade_active()) {
      if (request.action() != "prepare")
        throw std::runtime_error("upgrade plan is missing");
      if (!maintenance_.empty())
        throw std::runtime_error("Agent is already in maintenance");
      for (const auto& [name, s] : services_)
        if ((s.configuration.desired || s.process || !s.workers.empty()) &&
            s.configuration.kind != wire::MARKET_DATA &&
            s.configuration.kind != wire::TASK_SERVICE &&
            s.configuration.kind != wire::DATA_SERVICE)
          throw std::runtime_error("service lacks an automatic upgrade recovery boundary: " + name);
      Json revisions = Json::object();
      for (const auto& [name, s] : services_)
        revisions[name] = revision(s.configuration);
      Json processes = Json::array();
      for (const auto& [name, s] : services_) {
        if (s.process)
          processes.push_back(s.process->id());
        for (const auto& [id, worker] : s.workers)
          processes.push_back(worker->id());
      }
      Json next = {{"processes", processes},
                   {"version", 1},
                   {"operation", request.operation_id()},
                   {"phase", "draining"},
                   {"services", revisions}};
      // The node mutation owns the freeze until the candidate is confirmed.
      co_await save_upgrade(std::move(next));
    }
    const auto phase = upgrade_.at("phase").get<std::string>();
    if (request.action() == "resume" && phase == "ready") {
      auto next = upgrade_;
      next["phase"] = "restoring";
      co_await save_upgrade(std::move(next));
      for (auto& [name, s] : services_)
        if (s.configuration.desired)
          co_await start(name, s);
    } else if (request.action() == "complete") {
      if (phase != "restoring")
        throw std::runtime_error("upgrade services have not resumed");
      for (const auto& [name, s] : services_)
        if (s.configuration.desired &&
            (!s.process || (s.health != "ready" && s.health != "awaiting_input")))
          throw std::runtime_error("upgrade is waiting for restored service health");
      auto next = upgrade_;
      next["phase"] = "complete";
      co_await save_upgrade(std::move(next));
      result.set_phase("complete");
      co_return;
    }
    result.set_phase(upgrade_.at("phase").get<std::string>());
    result.set_detail(upgrade_error_);
  }
  unsigned owned_workers() const {
    unsigned count = 0;
    for (const auto& [name, service] : services_)
      count += static_cast<unsigned>(service.workers.size()) + service.retiring_workers;
    return count;
  }
  Resources service_usage() const {
    Resources used;
    for (const auto& [name, service] : services_)
      if (service.process || service.retiring_pid || service.starting)
        used += resource_budget_->service(service.configuration.kind);
    return used;
  }
  Resources resource_usage() const {
    auto used = service_usage();
    used += ResourceBudget::workers(owned_workers() + worker_reservations_);
    return used;
  }
  unsigned worker_allowance() const {
    const auto services = service_usage();
    for (const auto& [name, service] : services_)
      if (service.configuration.desired && service.waiting_capacity &&
          resource_budget_->fits(services, resource_budget_->service(service.configuration.kind)))
        return 0; // Let existing workers finish so a queued service can acquire its reservation.
    const auto count = owned_workers() + worker_reservations_;
    return resource_budget_->worker_slots(resource_usage(), worker_limit - count);
  }
  bool dispatch_ready(const std::string& name, const Service& service) const {
    return name != mutating_service_ && service.configuration.kind == wire::TASK_SERVICE &&
           service.process && service.configuration.desired && service.health == "ready" &&
           std::chrono::steady_clock::now() >= service.dispatch_at;
  }
  bool dispatch_turn(const std::string& name) const {
    // Observation completion order must not decide who gets newly freed slots.
    // Advance this cursor only when granting a turn, including an empty reply.
    auto next = services_.upper_bound(last_dispatched_);
    for (std::size_t visited = 0; visited < services_.size(); ++visited) {
      if (next == services_.end())
        next = services_.begin();
      const auto& [candidate, service] = *next++;
      if (dispatch_ready(candidate, service))
        return candidate == name;
    }
    return false;
  }
  struct WorkerReservation {
    unsigned& total;
    unsigned remaining;
    WorkerReservation(unsigned& total, unsigned slots) : total(total), remaining(slots) {
      total += slots;
    }
    WorkerReservation(const WorkerReservation&) = delete;
    ~WorkerReservation() { total -= remaining; }
    void started() {
      --total;
      --remaining;
    }
  };
  PolledTask<void> launch_tasks(const std::string& name, Service& service,
                                const task::v1::TaskResponse& response,
                                WorkerReservation& reservation) {
    // Dispatch policy lives in Task Service. Agent accepts only installed
    // program roles, never an executable path or arbitrary arguments.
    if (static_cast<unsigned>(response.launches().launches_size()) > reservation.remaining)
      throw std::runtime_error("task dispatch exceeded its reserved worker allowance");
    for (const auto& launch : response.launches().launches()) {
      validate_id(launch.task_id());
      if (service.workers.contains(launch.task_id()))
        throw std::runtime_error("dispatch repeated a running task");
      if (launch.daily_factor() && (launch.program() != task::v1::FACTOR_PROGRAM ||
                                    launch.daily_download() || launch.minute_download()))
        throw std::invalid_argument("invalid daily factor worker launch");
      // The data pipeline only runs data-source downloads.
      if ((launch.program() == task::v1::DATA_PIPELINE_PROGRAM) !=
              (launch.daily_download() || launch.minute_download()) ||
          (launch.daily_download() && launch.minute_download()))
        throw std::invalid_argument("invalid download worker launch");
      std::string artifact;
      switch (launch.program()) {
      case task::v1::BACKTEST_PROGRAM:
        artifact = service.configuration.worker_artifact;
        break;
      case task::v1::FACTOR_PROGRAM:
        artifact = service.configuration.factor_artifact;
        break;
      case task::v1::DATA_PIPELINE_PROGRAM:
        artifact = service.configuration.data_artifact;
        break;
      default:
        throw std::runtime_error("unsupported worker program");
      }
      std::unique_ptr<ChildProcess> child;
      const auto worker_endpoint = service.worker_endpoint;
      co_await prepare([&] {
        const auto worker = binary(artifact);
        if (sha256_file(worker) != artifact)
          throw std::runtime_error("task worker integrity check failed");
        std::vector<std::string> args{"--owner-pid", std::to_string(current_process_id()),
                                      "--endpoint",  worker_endpoint,
                                      "--session",   name,
                                      "--task",      launch.task_id()};
        if (launch.program() == task::v1::DATA_PIPELINE_PROGRAM)
          args.insert(args.end(), {"--plugin-directory",
                                   utf8(plugins_.materialize(name, {launch.provider_artifact()}))});
        if (launch.program() == task::v1::BACKTEST_PROGRAM)
          args.insert(args.end(), {"--plugin-directory",
                                   utf8(plugins_.materialize(name, {launch.risk_artifact()}))});
        if (launch.minute_download())
          args.push_back("--minute-download");
        if (launch.daily_download())
          args.push_back("--daily-download");
        if (launch.daily_factor())
          args.push_back("--daily-factor");
        child = std::make_unique<ChildProcess>(worker, args);
      });
      service.workers.emplace(launch.task_id(), std::move(child));
      reservation.started();
    }
  }

  struct ObservationIdentity {
    std::string name, revision;
    std::uint64_t generation, pid;
  };
  ObservationIdentity observation_identity(const std::string& name, const Service& service) {
    return {name, revision(service.configuration), service.generation, service.process->id()};
  }
  Service* current_observation(const ObservationIdentity& observation, bool task_dispatch) {
    if ((task_dispatch && phase_ != wire::Status::READY) || stopping_ ||
        mutating_service_ == observation.name || !maintenance_.empty() ||
        (upgrade_active() && (task_dispatch || upgrade_.at("phase") != "restoring")))
      return nullptr;
    auto found = services_.find(observation.name);
    if (found == services_.end())
      return nullptr;
    auto& service = found->second;
    if (!service.configuration.desired || !service.process || service.process->exited() ||
        service.generation != observation.generation || service.process->id() != observation.pid ||
        revision(service.configuration) != observation.revision)
      return nullptr;
    return &service;
  }
  PolledTask<void> supervise(std::string name) {
    auto* service = &services_.at(name);
    if (service->process && service->process->exited()) {
      log_process_event("agent", LogLevel::warning, "service.exited",
                        {{"service", name}, {"exit_code", service->process->exit_code()}});
      co_await stop_processes(*service);
      service->error = "process exited; awaiting bounded restart";
    }
    if (service->process && std::chrono::steady_clock::now() >= service->probe) {
      service->probe = std::chrono::steady_clock::now() + 5s;
      const auto observation = observation_identity(name, *service);
      const auto kind = service->configuration.kind;
      const auto endpoint = service->health_endpoint;
      const auto peer = kind == wire::TASK_SERVICE ? service->configuration.data_service
                                                   : service->configuration.task_service;
      HealthObservation health;
      std::string error;
      try {
        health = co_await probe_service_health(kind, name, endpoint, peer);
      } catch (const std::exception& e) {
        error = e.what();
      }
      service = current_observation(observation, false);
      if (!service)
        co_return;
      if (error.empty()) {
        service->health = std::move(health.status);
        service->execution = std::move(health.execution);
        service->execution_observed = std::chrono::steady_clock::now();
        if (service->health == "starting")
          service->probe = std::chrono::steady_clock::now() + 250ms;
        service->last_heartbeat = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count();
        service->failures = 0;
        service->error.clear();
      } else if (!service->last_heartbeat &&
                 std::chrono::steady_clock::now() - service->started < 10s) {
        service->probe = std::chrono::steady_clock::now() + 250ms;
        service->error = "waiting for the first heartbeat: " + error;
        co_return;
      } else {
        service->health = "unresponsive";
        service->error = "service heartbeat unavailable";
        log_process_event("agent", LogLevel::warning, "service.heartbeat_failed",
                          {{"service", name},
                           {"message", error},
                           {"consecutive_failures", service->failures + 1}});
        if (++service->failures >= 3 && phase_ == wire::Status::READY) {
          co_await stop_processes(*service);
          service->retry = std::chrono::steady_clock::now() + 5s;
        }
      }
    }
    if (phase_ == wire::Status::READY && !stopping_ && !node_mutation_ && !upgrade_active() &&
        dispatch_ready(name, *service)) {
      std::vector<std::string> running;
      for (auto it = service->workers.begin(); it != service->workers.end();) {
        if (it->second->exited())
          it = service->workers.erase(it);
        else {
          running.push_back(it->first);
          ++it;
        }
      }
      const auto slots = worker_allowance();
      if (!slots || !dispatch_turn(name))
        co_return; // Tasks stay queued; never launch and then kill to enforce admission.
      last_dispatched_ = name;
      service->dispatch_at = std::chrono::steady_clock::now() + 1s;
      WorkerReservation reservation(worker_reservations_, slots);
      const auto observation = observation_identity(name, *service);
      const auto endpoint = service->worker_endpoint;
      task::v1::TaskResponse response;
      std::string error;
      try {
        response = co_await request_task_dispatch(name, endpoint, running, reservation.remaining);
      } catch (const std::exception& e) {
        error = e.what();
      }
      service = current_observation(observation, true);
      if (!service)
        co_return;
      if (error.empty()) {
        try {
          co_await launch_tasks(name, *service, response, reservation);
        } catch (const std::exception& e) {
          error = e.what();
        }
      }
      if (!error.empty())
        service->error = std::move(error);
    }
    if (phase_ == wire::Status::READY && !stopping_ && !node_mutation_ &&
        name != mutating_service_ && service->configuration.desired && !service->process &&
        service->restarts < 3 && std::chrono::steady_clock::now() >= service->retry) {
      co_await start(name, *service, true);
    }
  }

  struct Loaded {
    std::map<std::string, Service> services;
    Json upgrade = nullptr;
    HostCapacity capacity;
  };
  Loaded load() {
    Loaded loaded;
    loaded.capacity = host_capacity();
    if (!root_.is_absolute() || !fs::is_directory(root_))
      throw std::invalid_argument("agent requires an existing absolute directory");
    require_managed_path(root_);
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
    for (const auto& [name, service] : loaded.services)
      validate_binding(loaded.services, name, service.configuration.kind,
                       service.configuration.task_service, service.configuration.data_service);
    const auto upgrade_path = root_ / "maintenance-plan.json";
    require_managed_path(upgrade_path);
    if (fs::exists(upgrade_path)) {
      if (fs::file_size(upgrade_path) > 65536)
        throw std::runtime_error("invalid upgrade plan size");
      std::ifstream file(upgrade_path);
      loaded.upgrade = parse_json(std::string(std::istreambuf_iterator<char>(file), {}));
      require_fields(loaded.upgrade, {"version", "operation", "phase", "services", "processes"});
      if (loaded.upgrade.at("version") != 1 ||
          (loaded.upgrade.at("phase") != "draining" && loaded.upgrade.at("phase") != "ready" &&
           loaded.upgrade.at("phase") != "restoring" && loaded.upgrade.at("phase") != "complete"))
        throw std::runtime_error("invalid upgrade plan");
      validate_id(loaded.upgrade.at("operation").get<std::string>());
      if (!loaded.upgrade.at("processes").is_array())
        throw std::runtime_error("invalid upgrade processes");
      if ((!loaded.upgrade.is_null() && loaded.upgrade.at("phase") != "complete") &&
          loaded.upgrade.at("services").size() != loaded.services.size())
        throw std::runtime_error("upgrade service configuration changed");
      for (const auto& [name, service] : loaded.services)
        if ((!loaded.upgrade.is_null() && loaded.upgrade.at("phase") != "complete") &&
            loaded.upgrade.at("services").at(name) != revision(service.configuration))
          throw std::runtime_error("upgrade service configuration changed");
    }
    return loaded;
  }
  PolledTask<void> initialize() {
    Loaded loaded;
    try {
      co_await prepare([&] {
        initialization_progress_.begin();
        struct Complete {
          Progress& progress;
          ~Complete() { progress.finish(); }
        } complete{initialization_progress_};
        loaded = load();
      });
      services_ = std::move(loaded.services);
      resource_budget_.emplace(loaded.capacity, current_platform().os == "macos");
      upgrade_ = std::move(loaded.upgrade);
      recovering_drain_ = upgrade_active() && upgrade_.at("phase") == "draining";
      phase_ = wire::Status::READY;
    } catch (const std::exception& error) {
      fail("Agent initialization failed; inspect node logs", error);
    }
  }

public:
  Agent(fs::path root, ipc::TlsIdentity tls, std::string bind, unsigned short control_port,
        Progress& io_progress)
      : root_(std::move(root)), tls_(std::move(tls)), bind_(std::move(bind)), local_(bind_.empty()),
        control_port_(control_port), io_progress_(io_progress) {
#ifndef _WIN32
    sockets_ = fs::path("/tmp") / ("ast-" + instance_.substr(0, 12));
    if (!fs::create_directory(sockets_))
      throw std::runtime_error("cannot create private service socket directory");
    fs::permissions(sockets_, fs::perms::owner_all);
#endif
    initializing_.emplace(initialize());
  }
  void advance(bool stopping) {
    state_progress_.finish();
    stopping_ = stopping;
    if (initializing_ && initializing_->poll()) {
      initializing_->take();
      initializing_.reset();
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
    if (upgrade_active() && upgrade_.at("phase") != "restoring") {
      if (phase_ == wire::Status::READY && !mutation_active_ && !upgrading_ &&
          observations_.empty() && upgrade_.at("phase") == "draining")
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
  bool background_idle() const { return observations_.empty() && !upgrading_ && !initializing_; }
  PolledTask<void> shutdown() {
    co_await PollUntil{[&] { return background_idle(); }};
    // Stop workers before their task service, and task services before data.
    for (const auto kind :
         {wire::TASK_SERVICE, wire::LIVE_TRADING, wire::MARKET_DATA, wire::DATA_SERVICE})
      for (auto& [name, service] : services_)
        if (service.configuration.kind == kind)
          co_await stop_processes(service);
  }

  ~Agent() {
    services_.clear();
    if (!sockets_.empty()) {
      std::error_code ec;
      fs::remove_all(sockets_, ec);
    }
  }
  PolledTask<wire::Response> dispatch(wire::Request r, std::string peer, ipc::PeerRole role,
                                      std::chrono::steady_clock::time_point deadline) {
    struct Mutation {
      Agent& agent;
      bool admitted = false, owned = false;
      ~Mutation() {
        if (admitted)
          --agent.admitted_mutations_;
        if (owned) {
          agent.mutation_active_ = false;
          agent.node_mutation_ = false;
          agent.mutating_service_.clear();
        }
      }
    } mutation{*this};
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
        throw Error(ErrorCode::permission_denied,
                    "this operation requires the node administrator certificate");
      if (!r.has_status()) {
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
          return stopping_ || std::chrono::steady_clock::now() >= deadline ||
                 (!upgrading_ && (r.has_upgrade() || r.has_maintenance()
                                      ? observations_.empty()
                                      : !observations_.contains(mutating_service_)));
        }};
        if (stopping_ || std::chrono::steady_clock::now() >= deadline)
          throw std::runtime_error("Agent request expired before execution");
        require_ready();
      }
      if (r.has_upgrade()) {
        co_await coordinate_upgrade(r.upgrade(), *response.mutable_upgrade());
        co_return response;
      }
      if (upgrade_active() && !r.has_status())
        throw std::runtime_error("Agent upgrade is coordinating services; mutations are disabled");
      if (r.has_maintenance()) {
        const auto& request = r.maintenance();
        validate_id(request.operation_id());
        if (request.instance_id() != instance_)
          throw std::invalid_argument("Agent instance changed");
        if (request.enter()) {
          if (!maintenance_.empty() && maintenance_ != request.operation_id())
            throw std::runtime_error("Agent already belongs to another maintenance operation");
          for (const auto& [name, service] : services_)
            if (service.configuration.desired || (service.process && !service.process->exited()) ||
                !service.workers.empty())
              throw std::runtime_error("stop all managed services before maintenance");
          maintenance_ = request.operation_id();
        } else {
          if (maintenance_ != request.operation_id())
            throw std::runtime_error("maintenance operation does not match");
          maintenance_.clear();
        }
        response.mutable_accepted();
        co_return response;
      }
      if (!maintenance_.empty() && !r.has_status())
        throw std::runtime_error("Agent is in maintenance; mutations are disabled");
      if (r.has_firewall()) {
        if (local_ || peer.empty())
          throw std::invalid_argument("firewall management requires a remote TLS node");
        const auto& operation = r.firewall();
        validate_service_id(operation.service_id());
        const auto& service = services_.at(operation.service_id());
        const auto os = current_platform().os;
        const auto file = root_ / "firewall" / (operation.service_id() + ".json");
        Json owned = nullptr;
        co_await prepare([&] {
          require_managed_path(file.parent_path());
          require_managed_path(file);
          if (fs::exists(file)) {
            if (fs::file_size(file) > 65536)
              throw std::invalid_argument("invalid firewall record");
            std::ifstream input(file);
            owned = Json::parse(input);
          }
        });
        if (operation.action() == "allow" || operation.action() == "remove") {
          if (!operation.token().empty())
            throw std::invalid_argument("inspection does not accept a confirmation token");
          firewall_plan_ = nullptr;
          Json observed;
          co_await prepare([&] {
            observed = asterion::node::run_firewall_script(
                os, asterion::node::firewall_inspection(os, peer));
          });
          const bool remove = operation.action() == "remove";
          if (!remove && !owned.is_null() && owned.at("source") != peer)
            throw std::invalid_argument("remove previous source rule before changing source");
          firewall_plan_ = {
              {"token", unique_process_id()},
              {"service", operation.service_id()},
              {"source", remove && !owned.is_null() ? owned.at("source").get<std::string>() : peer},
              {"peer", peer},
              {"port", service.configuration.port},
              {"backend", observed.at("backend")},
              {"state", observed.at("state")},
              {"can_apply",
               observed.at("state") == "active" &&
                   (observed.at("backend") == "ufw" || observed.at("backend") == "windows") &&
                   (!remove || !owned.is_null())},
              {"rule", owned.is_null() ? "asterion-" + unique_process_id()
                                       : owned.at("rule").get<std::string>()},
              {"action", operation.action()},
              {"verification", "not_checked"}};
          firewall_expiry_ = std::chrono::steady_clock::now() + 5min;
        } else if (operation.action() == "apply") {
          if (firewall_plan_.is_null() || firewall_plan_.at("token") != operation.token() ||
              firewall_plan_.at("service") != operation.service_id() ||
              firewall_plan_.at("peer") != peer ||
              std::chrono::steady_clock::now() > firewall_expiry_)
            throw std::invalid_argument("firewall confirmation expired; inspect again");
          auto plan = firewall_plan_;
          firewall_plan_ = nullptr;
          Json observed;
          co_await prepare([&] {
            observed = asterion::node::run_firewall_script(
                os, asterion::node::firewall_inspection(os, peer));
          });
          if (!plan.at("can_apply").get<bool>() || observed.at("state") != "active" ||
              observed.at("backend") != plan.at("backend"))
            throw std::invalid_argument("firewall state changed; inspect again");
          const bool remove = plan.at("action") == "remove";
          if (remove && (owned.is_null() || owned.at("rule") != plan.at("rule") ||
                         owned.at("source") != plan.at("source")))
            throw std::invalid_argument("no owned firewall rule");
          const auto port = service.configuration.port;
          co_await prepare([&] {
            if (!remove) {
              fs::create_directory(file.parent_path());
              replace_file_durably(file, plan.dump());
            }
            const auto changed = asterion::node::run_firewall_script(
                os, asterion::node::firewall_change(os, plan.at("source"), port, plan.at("rule"),
                                                    remove));
            if (changed != Json{{"changed", true}})
              throw std::runtime_error("invalid firewall result");
            if (remove)
              fs::remove(file);
          });
          plan["can_apply"] = false;
          plan["state"] = remove ? "removed" : "applied";
          firewall_plan_ = std::move(plan);
        } else
          throw std::invalid_argument("unsupported firewall action");
        auto* report = response.mutable_firewall();
        const auto& plan = firewall_plan_;
        report->set_token(plan.at("token").get<std::string>());
        report->set_source(plan.at("source").get<std::string>());
        report->set_port(plan.at("port").get<std::uint32_t>());
        report->set_backend(plan.at("backend").get<std::string>());
        report->set_state(plan.at("state").get<std::string>());
        report->set_can_apply(plan.at("can_apply").get<bool>());
        report->set_rule(plan.at("rule").get<std::string>());
        report->set_action(plan.at("action").get<std::string>());
        report->set_verification(plan.at("verification").get<std::string>());
        co_return response;
      }
      if (r.has_status()) {
        auto* status = response.mutable_status();
        const auto platform = current_platform();
        status->set_instance_id(instance_);
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
        execution->set_business_ready(phase_ == wire::Status::READY && !stopping_ &&
                                      !node_mutation_ && maintenance_.empty() && !upgrade_active());
        auto* capacity = status->mutable_worker_capacity();
        capacity->set_limit(worker_limit);
        capacity->set_owned(owned_workers());
        capacity->set_reserved(worker_reservations_);
        if (resource_budget_) {
          auto* budget = status->mutable_resource_budget();
          encode_resources(*budget->mutable_limit(), resource_budget_->limit);
          encode_resources(*budget->mutable_committed(), resource_usage());
          budget->set_file_workers(resource_budget_->file_workers);
        }
        status->set_upgrade_protocol(1);
        status->set_maintenance(node_mutation_ || !maintenance_.empty() || upgrade_active());
        status->set_pid(current_process_id());
        status->set_os(platform.os);
        status->set_arch(platform.arch);
        status->set_version("0.1.0");
        status->set_uptime_ms(
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - started_)
                                           .count()));
        for (auto& [name, s] : services_) {
          auto* service = status->add_services();
          service->set_id(name);
          service->set_kind(s.configuration.kind);
          encode_resources(*service->mutable_resource_request(),
                           resource_budget_->service(s.configuration.kind));
          service->set_task_service(s.configuration.task_service);
          service->set_data_service(s.configuration.data_service);
          service->set_active_workers(static_cast<unsigned>(s.workers.size()) + s.retiring_workers);
          service->set_artifact(s.configuration.artifact);
          service->set_revision(revision(s.configuration));
          for (const auto& hash : s.configuration.plugin_artifacts)
            service->add_plugin_artifacts(hash);
          service->set_port(s.configuration.port);
          service->set_desired_running(s.configuration.desired);
          service->set_restarts(s.restarts);
          service->set_error(s.error);
          const bool running = s.process && !s.process->exited();
          service->set_state(s.retiring_pid || s.retiring_workers ? "stopping"
                             : running                            ? "running"
                             : !s.configuration.desired           ? "stopped"
                             : s.waiting_capacity                 ? "waiting_capacity"
                             : s.starting                         ? "starting"
                             : s.restarts >= 3                    ? "failed"
                                                                  : "restarting");
          service->set_pid(running ? s.process->id() : s.retiring_pid);
          service->set_health(running ? s.health : "offline");
          service->set_last_heartbeat_ms(s.last_heartbeat);
          if (running && s.execution) {
            *service->mutable_execution() = *s.execution;
            protocol::age_execution_health(
                *service->mutable_execution(),
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - s.execution_observed)
                    .count());
            if (s.health != "unresponsive" &&
                protocol::execution_health_stalled(service->execution()))
              service->set_health("degraded");
          }
          if (local_) {
            service->set_endpoint(service_endpoint(name));
            service->set_directory(s.configuration.directory);
          }
        }
        co_return response;
      }
      if (r.has_upload()) {
        const auto& u = r.upload();
        validate_artifact_digest(u.sha256());
        const auto platform = current_platform();
        if (u.os() != platform.os || u.arch() != platform.arch || !u.size() ||
            u.size() > max_artifact_bytes)
          throw std::invalid_argument("artifact platform or size mismatch");
        const auto path = root_ / "uploads" / u.sha256();
        co_await prepare([&] {
          require_managed_path(path);
          std::ofstream out(path, std::ios::binary | std::ios::trunc);
          if (!out)
            throw std::runtime_error("cannot create upload");
        });
        uploads_[u.sha256()] = {u.size(), 0};
      } else if (r.has_chunk()) {
        const auto& c = r.chunk();
        validate_artifact_digest(c.sha256());
        auto& u = uploads_.at(c.sha256());
        if (c.offset() != u.offset || c.data().empty() || c.data().size() > 1024 * 1024 ||
            c.data().size() > u.size - u.offset)
          throw std::invalid_argument("invalid upload chunk");
        const auto path = root_ / "uploads" / c.sha256();
        co_await prepare([&] {
          require_managed_path(path);
          std::ofstream out(path, std::ios::binary | std::ios::app);
          out.write(c.data().data(), static_cast<std::streamsize>(c.data().size()));
          out.flush();
          if (!out)
            throw std::runtime_error("upload write failed");
        });
        u.offset += c.data().size();
      } else if (r.has_finish()) {
        const auto hash = r.finish().sha256();
        validate_artifact_digest(hash);
        const auto u = uploads_.at(hash);
        const auto path = root_ / "uploads" / hash;
        const auto target = binary(hash);
        co_await prepare([&] {
          require_managed_path(path);
          require_managed_path(target);
          // A failed directory sync may leave the renamed target in place.
          // Revalidate and durably acknowledge it on an explicit finish retry.
          const auto source = fs::exists(path) ? path : target;
          if (u.offset != u.size || sha256_file(source) != hash)
            throw std::invalid_argument("artifact size or checksum mismatch");
          const auto actual = artifact_platform(source);
          const auto platform = current_platform();
          if (actual.os != platform.os || actual.arch != platform.arch)
            throw std::invalid_argument("uploaded executable platform mismatch");
          if (fs::exists(target)) {
            if (sha256_file(target) != hash)
              throw std::runtime_error("existing artifact corrupted");
            sync_directory(target.parent_path());
            if (fs::exists(path))
              fs::remove(path);
          } else {
            fs::permissions(path, fs::perms::owner_all);
            publish_file_durably(path, target);
          }
          sync_directory(path.parent_path());
        });
        uploads_.erase(hash);
      } else if (r.has_deploy()) {
        const auto& d = r.deploy();
        validate_service_id(d.service_id());
        validate_artifact_digest(d.sha256());
        if (d.kind() != wire::MARKET_DATA && d.kind() != wire::TASK_SERVICE &&
            d.kind() != wire::LIVE_TRADING && d.kind() != wire::DATA_SERVICE)
          throw std::invalid_argument("explicit service kind required");
        if (d.kind() == wire::DATA_SERVICE) {
          validate_service_id(d.task_service());
        } else if (!d.task_service().empty())
          throw std::invalid_argument("task binding only belongs to data service");
        if (d.kind() == wire::TASK_SERVICE) {
          validate_service_id(d.data_service());
        } else if (!d.data_service().empty())
          throw std::invalid_argument("data binding only belongs to task service");
        validate_binding(services_, d.service_id(), d.kind(), d.task_service(), d.data_service());
        if (!d.provider_artifact().empty()) {
          if (d.kind() != wire::MARKET_DATA)
            throw std::invalid_argument("provider library only belongs to market data");
          validate_artifact_digest(d.provider_artifact());
          co_await verify_artifact(d.provider_artifact(), "provider artifact not installed");
        }
        if (d.kind() == wire::LIVE_TRADING && d.catalog_artifact().empty())
          throw std::invalid_argument("live trading requires the CTP trader library");
        if (!d.catalog_artifact().empty()) {
          if (d.kind() != wire::MARKET_DATA && d.kind() != wire::LIVE_TRADING)
            throw std::invalid_argument(
                "CTP trader library only belongs to market data or live trading");
          validate_artifact_digest(d.catalog_artifact());
          co_await verify_artifact(d.catalog_artifact(), "catalog artifact not installed");
        }
        if (d.kind() == wire::TASK_SERVICE) {
          validate_artifact_digest(d.data_artifact());
          co_await verify_artifact(d.data_artifact(), "data worker is not installed");
          validate_artifact_digest(d.factor_artifact());
          co_await verify_artifact(d.factor_artifact(), "factor worker is not installed");
          validate_artifact_digest(d.worker_artifact());
          co_await verify_artifact(d.worker_artifact(), "backtest worker is not installed");
        } else if (!d.worker_artifact().empty() || !d.factor_artifact().empty() ||
                   !d.data_artifact().empty())
          throw std::invalid_argument("worker artifact only belongs to task service");
        if (d.port() > 65535 || (local_ ? d.port() != 0 : (!d.port() || d.port() == control_port_)))
          throw std::invalid_argument("invalid service port");
        if (services_.contains(d.service_id()))
          throw std::invalid_argument("service already exists; stop/start preserves its ledger; "
                                      "replacement is not supported");
        for (const auto& [name, s] : services_) {
          (void)name;
          if (d.port() && s.configuration.port == d.port())
            throw std::invalid_argument("port already assigned");
        }
        co_await verify_artifact(d.sha256(), "artifact not installed");
        const auto folder = root_ / "services" / d.service_id();
        Service s;
        s.configuration.kind = d.kind();
        s.configuration.task_service = d.task_service();
        s.configuration.data_service = d.data_service();
        s.configuration.plugin_artifacts.assign(d.plugin_artifacts().begin(),
                                                d.plugin_artifacts().end());
        co_await prepare([&] { plugins_.verify(s.configuration.plugin_artifacts); });
        s.configuration.provider_artifact = d.provider_artifact();
        s.configuration.catalog_artifact = d.catalog_artifact();
        s.configuration.worker_artifact = d.worker_artifact();
        s.configuration.factor_artifact = d.factor_artifact();
        s.configuration.data_artifact = d.data_artifact();
        s.configuration.artifact = d.sha256();
        s.configuration.port = static_cast<unsigned short>(d.port());
        const bool ledger = s.configuration.kind == wire::LIVE_TRADING;
        if (local_ && ledger) {
          const fs::path ledger(std::u8string(d.directory().begin(), d.directory().end()));
          co_await prepare([&] {
            if (!ledger.is_absolute() || !fs::is_directory(ledger))
              throw std::invalid_argument("local ledger must be an existing absolute directory");
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
          if (!fs::create_directory(folder))
            throw std::invalid_argument("service directory already exists");
          sync_directory(folder.parent_path());
          if (!local_ || !ledger)
            create_directories_durably(folder / "ledger");
        });
        co_await save(d.service_id(), s.configuration);
        auto [it, added] = services_.emplace(d.service_id(), std::move(s));
        (void)added;
        mutating_service_ = it->first;
        co_await start(it->first, it->second);
      } else if (r.has_configure_plugins()) {
        co_await configure_plugins(r.configure_plugins());
      } else if (r.has_update()) {
        const auto& u = r.update();
        validate_service_id(u.service_id());
        validate_artifact_digest(u.expected_revision());
        auto& current = services_.at(u.service_id());
        if (current.configuration.desired || (current.process && !current.process->exited()) ||
            !current.workers.empty())
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
        if (current.configuration.kind == wire::LIVE_TRADING && u.catalog_artifact().empty())
          throw std::invalid_argument("live trading requires the CTP trader library");
        if (!u.catalog_artifact().empty()) {
          if (current.configuration.kind != wire::MARKET_DATA &&
              current.configuration.kind != wire::LIVE_TRADING)
            throw std::invalid_argument(
                "CTP trader library only belongs to market data or live trading");
          co_await verified(u.catalog_artifact());
        }
        if (current.configuration.kind == wire::MARKET_DATA) {
          if (!u.provider_artifact().empty())
            co_await verified(u.provider_artifact());
        } else if (!u.provider_artifact().empty())
          throw std::invalid_argument("provider library only belongs to market data");
        if (current.configuration.kind == wire::TASK_SERVICE) {
          co_await verified(u.worker_artifact());
          co_await verified(u.factor_artifact());
          co_await verified(u.data_artifact());
        } else if (!u.worker_artifact().empty() || !u.factor_artifact().empty() ||
                   !u.data_artifact().empty())
          throw std::invalid_argument("worker artifacts only belong to task service");
        auto next = current.configuration;
        next.plugin_artifacts.assign(u.plugin_artifacts().begin(), u.plugin_artifacts().end());
        co_await prepare([&] { plugins_.verify(next.plugin_artifacts); });
        next.desired = false;
        next.artifact = u.artifact();
        next.provider_artifact = u.provider_artifact();
        next.catalog_artifact = u.catalog_artifact();
        next.worker_artifact = u.worker_artifact();
        next.factor_artifact = u.factor_artifact();
        next.data_artifact = u.data_artifact();
        // Publish metadata before changing the in-memory selection. No ledger
        // rewrite, automatic restart, or implicit binary rollback occurs.
        co_await save(u.service_id(), next);
        current.process.reset();
        current.configuration = std::move(next);
        current.restarts = 0;
        current.error.clear();
        current.health = "offline";
        current.last_heartbeat = 0;
        current.execution.reset();
      } else if (r.has_action()) {
        const auto& a = r.action();
        validate_service_id(a.service_id());
        auto& s = services_.at(a.service_id());
        if (a.kind() != wire::Action::START && a.kind() != wire::Action::STOP &&
            a.kind() != wire::Action::RESTART)
          throw std::invalid_argument("unknown service action");
        const bool desired = a.kind() != wire::Action::STOP;
        auto next = s.configuration;
        next.desired = desired;
        co_await save(a.service_id(), next);
        s.configuration = std::move(next);
        if (!desired) {
          co_await stop_processes(s);
          s.waiting_capacity = false;
          s.error.clear();
        } else if (a.kind() == wire::Action::RESTART || !s.process || s.process->exited()) {
          s.restarts = 0;
          co_await start(a.service_id(), s);
        }
      } else
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
int main(int argc, char** argv) {
  CLI::App app{"Asterion Node Agent: trusted remote deployment and supervision"};
  app.set_version_flag("--version", "Asterion Node Agent 0.1.0");
  std::string directory, inspect, development_manifest;
  service::Transport transport;
  auto* inspect_option =
      app.add_option("--inspect-plugin", inspect, "Print a native plugin's identity and exit");
  app.add_option("--directory", directory)->check(CLI::ExistingDirectory)->excludes(inspect_option);
  app.add_option("--prepare-development", development_manifest,
                 "Synchronize development programs offline and exit")
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
    return asterion::agent::inspect_plugin(inspect);
  if (directory.empty()) {
    std::cerr << "--directory is required\n";
    return 2;
  }
  auto run = [&]() -> int {
    try {
      service::install_stop_signals();
      std::uint64_t test_owner = 0;
      if (environment_variable("ASTERION_TEST_NODE_ISOLATED") == "1")
        if (const auto owner = environment_variable("ASTERION_TEST_OWNER_PID"))
          test_owner = std::stoull(*owner);
      if (!development_manifest.empty()) {
        if (environment_variable("ASTERION_ENVIRONMENT") != "development")
          throw std::invalid_argument("program preparation requires the development environment");
        asterion::agent::prepare_development_programs(fs::path(directory),
                                                      fs::path(development_manifest));
        return 0;
      }
      transport.validate();
      const fs::path root(std::u8string(directory.begin(), directory.end()));
      if (!root.is_absolute() || !fs::is_directory(root))
        throw std::invalid_argument("agent requires an existing absolute directory");
      require_managed_path(root);
      asterion::FileLock ownership(root, "agent.lock");
      // Agent and managed services each keep daily log files under logs/.
      const auto logs = root / "logs";
      require_managed_path(logs);
      fs::create_directory(logs);
#ifndef _WIN32
      ::setenv("ASTERION_LOG_DIRECTORY", logs.c_str(), 1);
#endif
      log_process_event("agent", LogLevel::info, "agent.started", {{"pid", current_process_id()}});
#ifndef _WIN32
      if (!transport.remote()) {
        // Only the Agent holding agent.lock owns this path; a stale socket
        // from a crashed predecessor is replaced.
        require_managed_path(transport.endpoint);
        fs::remove(transport.endpoint);
      }
#endif
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
        auto response = co_await agent->dispatch(request, std::move(peer.address), peer.role,
                                                 std::chrono::steady_clock::now() + 10s);
        protocol::log_rpc_result("agent", request, response, request.has_status());
        co_return response.SerializeAsString();
      };
      auto handle = [&](const Host::Peer& peer, std::string frame) -> Host::Reply {
        wire::Request request;
        if (!request.ParseFromString(frame))
          throw std::invalid_argument("malformed Protobuf request");
        auto operation = std::make_shared<PolledTask<std::string>>(serve(std::move(request), peer));
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
      Host host(transport, handle, std::move(options), io_progress);
      agent =
          std::make_unique<Agent>(root, transport.tls, transport.bind, transport.port, io_progress);
      try {
        if (!host.run())
          std::_Exit(0);
      } catch (const std::exception& error) {
        // Suspended requests may still be borrowed by file or process work.
        // A fatal loop failure must not destroy those frames under the pool.
        log_process_event("agent", LogLevel::error, "agent.failed", {{"message", error.what()}});
        std::_Exit(1);
      }

    } catch (const std::exception& error) {
      std::cerr << "Node failed: " << error.what() << '\n';
      log_process_event("agent", LogLevel::error, "agent.failed", {{"message", error.what()}});
      return 1;
    }
    return 0;
  };
  return run();
}
