#include <set>
#include "plugin_artifacts.hpp"
#include "service_configuration.hpp"
#include "managed_paths.hpp"
#include "firewall.hpp"
#include "windows_service.hpp"
#include <asterion/kernel/service_host.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <CLI/CLI.hpp>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/ipc/tls_channel.hpp>
#include <asterion/kernel/logger.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/file_lock.hpp>
#include <asterion/kernel/process/owner.hpp>
#include <asterion/kernel/thread_pool.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/v1/market.pb.h>
#include <asterion/v1/node.pb.h>
#include <asterion/v1/research.pb.h>
#include <asterion/v1/strategy.pb.h>
#include <atomic>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <regex>
#include <thread>
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
struct Service {
  ServiceConfiguration configuration;
  std::string worker_endpoint;
  std::map<std::string, std::unique_ptr<ChildProcess>> workers;
  std::chrono::steady_clock::time_point dispatch_at{};
  std::string error, endpoint, health_endpoint, health = "starting";
  std::int64_t last_heartbeat = 0;
  unsigned failures = 0;
  std::chrono::steady_clock::time_point probe{};
  unsigned int restarts = 0;
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
  bool upgrade_write_failed_ = false;
  bool recovering_drain_ = false;
  bool upgrade_active() const {
    return upgrade_write_failed_ || (!upgrade_.is_null() && upgrade_.at("phase") != "complete");
  }
  std::mutex mutex_;
  std::jthread supervisor_;
  const std::string instance_ = unique_process_id();
  const std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
  fs::path binary(const std::string& hash) const {
    return root_ / "artifacts" / (hash + (current_platform().os == "windows" ? ".exe" : ".bin"));
  }
  std::string revision(const ServiceConfiguration& configuration) const {
    return service_revision(configuration);
  }
  void save(const std::string& name, const ServiceConfiguration& configuration) {
    save_service_configuration(root_ / "services" / name, configuration);
  }
  void configure_plugins(const wire::ConfigurePlugins& request) {
    validate_service_id(request.service_id());
    validate_artifact_digest(request.expected_revision());
    auto& current = services_.at(request.service_id());
    if (current.configuration.kind != wire::TASK_SERVICE)
      throw std::invalid_argument("service does not support managed native plugins");
    if (current.configuration.desired || (current.process && !current.process->exited()) ||
        !current.workers.empty())
      throw std::invalid_argument("stop the service before configuring plugins");
    if (revision(current.configuration) != request.expected_revision())
      throw std::invalid_argument("service configuration changed; inspect again");
    auto next = current.configuration;
    next.desired = false;
    next.plugin_artifacts.assign(request.artifacts().begin(), request.artifacts().end());
    plugins_.verify(next.plugin_artifacts);
    for (const auto& hash : next.plugin_artifacts) {
      const auto actual = artifact_platform(binary(hash)), expected = current_platform();
      if (actual.os != expected.os || actual.arch != expected.arch)
        throw std::invalid_argument("native plugin platform mismatch");
    }
    std::sort(next.plugin_artifacts.begin(), next.plugin_artifacts.end());
    // Persist the complete selection before changing the stopped service in memory.
    save(request.service_id(), next);
    current.configuration.plugin_artifacts = std::move(next.plugin_artifacts);
    current.process.reset();
    current.restarts = 0;
    current.error.clear();
    current.health = "offline";
    current.last_heartbeat = 0;
  }
  void start(const std::string& name, Service& s) {
    s.workers.clear();
    try {
      const auto executable = binary(s.configuration.artifact);
      if (sha256_file(executable) != s.configuration.artifact)
        throw std::runtime_error("artifact integrity check failed");
      s.endpoint = utf8(sockets_ / (name + ".sock"));
      s.health_endpoint = utf8(sockets_ / (name + ".health"));
      s.worker_endpoint = utf8(sockets_ / (name + ".workers"));
#ifdef _WIN32
      s.endpoint = "asterion." + instance_ + "." + name;
      s.health_endpoint = s.endpoint + ".health";
      s.worker_endpoint = s.endpoint + ".workers";
#else
      // Only this locked Agent owns these ephemeral socket paths.
      fs::remove(s.endpoint);
      fs::remove(s.health_endpoint);
      fs::remove(s.worker_endpoint);
#endif
      std::vector<std::string> args{"--owner-pid",       std::to_string(current_process_id()),
                                    "--session",         name,
                                    "--directory",       s.configuration.directory,
                                    "--health-endpoint", s.health_endpoint};
      if (s.configuration.kind == wire::TASK_SERVICE || s.configuration.kind == wire::PAPER_TRADING)
        args.insert(args.end(),
                    {"--plugin-directory",
                     utf8(plugins_.materialize(name, s.configuration.plugin_artifacts))});
      if (s.configuration.kind == wire::TASK_SERVICE) {
        if (sha256_file(binary(s.configuration.data_artifact)) != s.configuration.data_artifact)
          throw std::runtime_error("data worker integrity check failed");
        if (sha256_file(binary(s.configuration.factor_artifact)) != s.configuration.factor_artifact)
          throw std::runtime_error("factor worker integrity check failed");
        if (sha256_file(binary(s.configuration.worker_artifact)) != s.configuration.worker_artifact)
          throw std::runtime_error("backtest worker integrity check failed");
        args.insert(args.end(), {"--worker-endpoint", s.worker_endpoint});
      }
      if (s.configuration.kind == wire::PAPER_TRADING)
        args.insert(args.end(), {"--mode", "paper"});
      if (s.configuration.kind == wire::MARKET_DATA && !s.configuration.provider_artifact.empty()) {
        const auto source = binary(s.configuration.provider_artifact);
        if (sha256_file(source) != s.configuration.provider_artifact)
          throw std::runtime_error("provider artifact integrity check failed");
        const auto library = root_ / "services" / name /
                             (s.configuration.provider_artifact +
                              std::string(current_platform().os == "windows" ? ".dll"
                                          : current_platform().os == "macos" ? ".dylib"
                                                                             : ".so"));
        require_managed_path(library);
        if (!fs::exists(library))
          fs::copy_file(source, library);
        else if (sha256_file(library) != s.configuration.provider_artifact)
          throw std::runtime_error("provider library changed");
        args.insert(args.end(), {"--ctp-library", utf8(library)});
      }
      if (s.configuration.kind == wire::MARKET_DATA && !s.configuration.catalog_artifact.empty()) {
        const auto source = binary(s.configuration.catalog_artifact);
        if (sha256_file(source) != s.configuration.catalog_artifact)
          throw std::runtime_error("catalog artifact integrity check failed");
        const auto library = root_ / "services" / name /
                             (s.configuration.catalog_artifact +
                              std::string(current_platform().os == "macos"     ? ".dylib"
                                          : current_platform().os == "windows" ? ".dll"
                                                                               : ".so"));
        require_managed_path(library);
        if (!fs::exists(library))
          fs::copy_file(source, library);
        else if (sha256_file(library) != s.configuration.catalog_artifact)
          throw std::runtime_error("catalog library changed");
        args.insert(args.end(), {"--ctp-catalog-library", utf8(library)});
      }
      if (local_)
        args.insert(args.end(), {"--endpoint", s.endpoint});
      else
        args.insert(args.end(), {"--bind", bind_, "--port", std::to_string(s.configuration.port),
                                 "--tls-ca", tls_.ca_file, "--tls-cert", tls_.certificate_file,
                                 "--tls-key", tls_.private_key_file});
      s.process = std::make_unique<ChildProcess>(executable, args);
      s.health = "starting";
      s.last_heartbeat = 0;
      s.failures = 0;
      s.probe = std::chrono::steady_clock::now() + 2s;
      s.error.clear();
    } catch (const std::exception& e) {
      s.process.reset();
      s.error = e.what();
    }
    s.retry = std::chrono::steady_clock::now() + 5s;
  }
  void save_upgrade() {
    const auto path = root_ / "maintenance-plan.json";
    require_managed_path(path);
    try {
      replace_file_durably(path, upgrade_.dump());
    } catch (...) {
      upgrade_write_failed_ = true;
      throw;
    }
  }
  template <class Request, class Response>
  void quiesce_service(const std::string& name, Service& s, bool stop) {
    Request request;
    request.set_version(1);
    request.set_service_id(name);
    request.set_correlation_id(unique_process_id());
    request.mutable_quiesce()->set_stop(stop);
    auto channel = ipc::Channel::connect(s.health_endpoint, 500ms);
    channel.send(request.SerializeAsString(), 1s);
    Response response;
    if (!response.ParseFromString(channel.receive(1s)))
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
  void drain_upgrade() {
    if (upgrade_write_failed_)
      return;
    upgrade_error_.clear();
    if (recovering_drain_) {
      for (const auto& pid : upgrade_.at("processes"))
        if (process_running(pid.get<std::uint64_t>())) {
          upgrade_error_ = "waiting for previous service processes to exit";
          return;
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
          quiesce_service<market::v1::Request, market::v1::Response>(name, s, true);
        else if (s.configuration.kind == wire::TASK_SERVICE)
          quiesce_service<research::v1::TaskRequest, research::v1::TaskResponse>(name, s,
                                                                                 s.workers.empty());
      } catch (const std::exception& e) {
        upgrade_error_ = name + ": " + e.what();
      }
    }
    if (ready) {
      upgrade_["phase"] = "ready";
      save_upgrade();
    }
  }
  void coordinate_upgrade(const wire::Upgrade& request, wire::UpgradeState& result) {
    validate_id(request.operation_id());
    if (request.action() != "prepare" && request.action() != "resume" &&
        request.action() != "complete")
      throw std::invalid_argument("invalid upgrade action");
    if (upgrade_write_failed_)
      throw std::runtime_error("upgrade plan publication requires recovery");
    if (!upgrade_.is_null() && upgrade_.at("phase") == "complete") {
      if (upgrade_.at("operation") == request.operation_id()) {
        result.set_phase("complete");
        return;
      }
      if (request.action() == "prepare")
        upgrade_ = nullptr;
    }
    if (!upgrade_.is_null() && upgrade_.at("operation") != request.operation_id())
      throw std::runtime_error("another upgrade owns this node");
    if (upgrade_.is_null()) {
      if (request.action() != "prepare")
        throw std::runtime_error("upgrade plan is missing");
      if (!maintenance_.empty())
        throw std::runtime_error("Agent is already in maintenance");
      for (const auto& [name, s] : services_)
        if ((s.configuration.desired || s.process || !s.workers.empty()) &&
            s.configuration.kind != wire::MARKET_DATA && s.configuration.kind != wire::TASK_SERVICE)
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
      upgrade_ = {{"processes", processes},
                  {"version", 1},
                  {"operation", request.operation_id()},
                  {"phase", "draining"},
                  {"services", revisions}};
      // Freeze in memory before publication; a failed write must not reopen mutations.
      save_upgrade();
    }
    const auto phase = upgrade_.at("phase").get<std::string>();
    if (request.action() == "resume" && phase == "ready") {
      upgrade_["phase"] = "restoring";
      save_upgrade();
      for (auto& [name, s] : services_)
        if (s.configuration.desired)
          start(name, s);
    } else if (request.action() == "complete") {
      if (phase != "restoring")
        throw std::runtime_error("upgrade services have not resumed");
      for (const auto& [name, s] : services_)
        if (s.configuration.desired &&
            (!s.process || (s.health != "ready" && s.health != "awaiting_input")))
          throw std::runtime_error("upgrade is waiting for restored service health");
      upgrade_["phase"] = "complete";
      save_upgrade();
      result.set_phase("complete");
      return;
    }
    result.set_phase(upgrade_.at("phase").get<std::string>());
    result.set_detail(upgrade_error_);
  }
  void dispatch_tasks(const std::string& name, Service& service) {
    for (auto it = service.workers.begin(); it != service.workers.end();) {
      if (it->second->exited())
        it = service.workers.erase(it);
      else
        ++it;
    }
    research::v1::TaskRequest request;
    request.set_version(1);
    request.set_service_id(name);
    request.set_correlation_id(unique_process_id());
    auto* dispatch = request.mutable_dispatch();
    for (const auto& [id, process] : service.workers) {
      (void)process;
      dispatch->add_running(id);
    }
    auto channel = ipc::Channel::connect(service.worker_endpoint, 500ms);
    channel.send(request.SerializeAsString(), 1s);
    research::v1::TaskResponse response;
    if (!response.ParseFromString(channel.receive(1s)))
      throw std::runtime_error("invalid task dispatch response");
    protocol::validate_message(response);
    if (response.version() != 1 || response.service_id() != name ||
        response.correlation_id() != request.correlation_id() || !response.has_launches())
      throw std::runtime_error("task queue unavailable");
    // Dispatch policy lives in Task Service. Agent accepts only installed
    // program roles, never an executable path or arbitrary arguments.
    // A host resource ceiling, independent of the service's scheduling policy.
    if (service.workers.size() + static_cast<std::size_t>(response.launches().launches_size()) > 32)
      throw std::runtime_error("worker process capacity exceeded");
    for (const auto& launch : response.launches().launches()) {
      validate_id(launch.task_id());
      if (service.workers.contains(launch.task_id()))
        throw std::runtime_error("dispatch repeated a running task");
      if (launch.daily_factor() && (launch.program() != research::v1::FACTOR_PROGRAM ||
                                    launch.daily_download() || launch.minute_download()))
        throw std::invalid_argument("invalid daily factor worker launch");
      // The data pipeline only runs data-source downloads.
      if ((launch.program() == research::v1::DATA_PIPELINE_PROGRAM) !=
              (launch.daily_download() || launch.minute_download()) ||
          (launch.daily_download() && launch.minute_download()))
        throw std::invalid_argument("invalid download worker launch");
      std::string artifact;
      switch (launch.program()) {
      case research::v1::BACKTEST_PROGRAM:
        artifact = service.configuration.worker_artifact;
        break;
      case research::v1::FACTOR_PROGRAM:
        artifact = service.configuration.factor_artifact;
        break;
      case research::v1::DATA_PIPELINE_PROGRAM:
        artifact = service.configuration.data_artifact;
        break;
      default:
        throw std::runtime_error("unsupported worker program");
      }
      const auto worker = binary(artifact);
      if (sha256_file(worker) != artifact)
        throw std::runtime_error("research worker integrity check failed");
      std::vector<std::string> args{"--owner-pid", std::to_string(current_process_id()),
                                    "--endpoint",  service.worker_endpoint,
                                    "--session",   name,
                                    "--task",      launch.task_id()};
      if (launch.program() == research::v1::DATA_PIPELINE_PROGRAM)
        args.insert(args.end(), {"--plugin-directory",
                                 utf8(plugins_.materialize(name, {launch.provider_artifact()}))});
      if (launch.program() == research::v1::BACKTEST_PROGRAM)
        args.insert(args.end(), {"--plugin-directory",
                                 utf8(plugins_.materialize(name, {launch.risk_artifact()}))});
      if (launch.minute_download())
        args.push_back("--minute-download");
      if (launch.daily_download())
        args.push_back("--daily-download");
      if (launch.daily_factor())
        args.push_back("--daily-factor");
      service.workers.emplace(launch.task_id(), std::make_unique<ChildProcess>(worker, args));
    }
  }

public:
  Agent(fs::path root, ipc::TlsIdentity tls, std::string bind, unsigned short control_port)
      : root_(std::move(root)), tls_(std::move(tls)), bind_(std::move(bind)), local_(bind_.empty()),
        control_port_(control_port) {
#ifndef _WIN32
    sockets_ = fs::path("/tmp") / ("ast-" + instance_.substr(0, 12));
    if (!fs::create_directory(sockets_))
      throw std::runtime_error("cannot create private service socket directory");
    fs::permissions(sockets_, fs::perms::owner_all);
#endif
    if (!root_.is_absolute() || !fs::is_directory(root_))
      throw std::invalid_argument("agent requires an existing absolute directory");
    require_managed_path(root_);
    for (const auto* name : {"artifacts", "services", "uploads"}) {
      require_managed_path(root_ / name);
      fs::create_directory(root_ / name);
    }
    for (const auto& entry : fs::directory_iterator(root_ / "services")) {
      require_managed_path(entry.path());
      const auto name = entry.path().filename().string();
      validate_service_id(name);
      Service s;
      s.configuration = load_service_configuration(entry.path(), local_, control_port_);
      plugins_.verify(s.configuration.plugin_artifacts);
      services_.emplace(name, std::move(s));
    }
    const auto upgrade_path = root_ / "maintenance-plan.json";
    require_managed_path(upgrade_path);
    if (fs::exists(upgrade_path)) {
      if (fs::file_size(upgrade_path) > 65536)
        throw std::runtime_error("invalid upgrade plan size");
      std::ifstream file(upgrade_path);
      upgrade_ = parse_json(std::string(std::istreambuf_iterator<char>(file), {}));
      require_fields(upgrade_, {"version", "operation", "phase", "services", "processes"});
      if (upgrade_.at("version") != 1 ||
          (upgrade_.at("phase") != "draining" && upgrade_.at("phase") != "ready" &&
           upgrade_.at("phase") != "restoring" && upgrade_.at("phase") != "complete"))
        throw std::runtime_error("invalid upgrade plan");
      validate_id(upgrade_.at("operation").get<std::string>());
      if (!upgrade_.at("processes").is_array())
        throw std::runtime_error("invalid upgrade processes");
      recovering_drain_ = upgrade_.at("phase") == "draining";
      if (upgrade_active() && upgrade_.at("services").size() != services_.size())
        throw std::runtime_error("upgrade service configuration changed");
      for (const auto& [name, service] : services_)
        if (upgrade_active() && upgrade_.at("services").at(name) != revision(service.configuration))
          throw std::runtime_error("upgrade service configuration changed");
    }
    for (auto& [name, service] : services_)
      if (service.configuration.desired &&
          (!upgrade_active() || upgrade_.at("phase") == "restoring"))
        start(name, service);
    supervisor_ = std::jthread([this](std::stop_token token) {
      while (!token.stop_requested()) {
        std::this_thread::sleep_for(200ms);
        std::lock_guard lock(mutex_);
        if (upgrade_active() && upgrade_.at("phase") != "restoring") {
          try {
            if (upgrade_.at("phase") == "draining")
              drain_upgrade();
          } catch (const std::exception& e) {
            upgrade_error_ = e.what();
          }
          continue;
        }
        for (auto& [name, s] : services_) {
          if (s.process && s.process->exited()) {
            s.workers.clear();
            s.process.reset();
            s.error = "process exited; awaiting bounded restart";
          }
          if (s.process && std::chrono::steady_clock::now() >= s.probe) {
            s.probe = std::chrono::steady_clock::now() + 5s;
            try {
              auto channel = ipc::Channel::connect(s.health_endpoint, 500ms);
              if (s.configuration.kind == wire::MARKET_DATA) {
                market::v1::Request ping;
                ping.set_version(1);
                ping.set_service_id(name);
                ping.set_correlation_id("health." + unique_process_id());
                ping.mutable_heartbeat();
                channel.send(ping.SerializeAsString(), 1s);
                market::v1::Response reply;
                if (!reply.ParseFromString(channel.receive(1s)))
                  throw std::runtime_error("invalid market health");
                protocol::validate_message(reply);
                if (reply.version() != 1 || reply.service_id() != name ||
                    reply.correlation_id() != ping.correlation_id() || !reply.has_health() ||
                    reply.health().instance_id().empty())
                  throw std::runtime_error("market health identity mismatch");
                const auto phase = reply.health().phase();
                s.health = phase == "connected"                             ? "ready"
                           : phase == "error" || phase == "sdk_unavailable" ? "degraded"
                                                                            : "awaiting_input";
              } else if (s.configuration.kind == wire::TASK_SERVICE) {
                research::v1::TaskRequest ping;
                ping.set_version(1);
                ping.set_service_id(name);
                ping.set_correlation_id("health." + unique_process_id());
                ping.mutable_heartbeat();
                channel.send(ping.SerializeAsString(), 1s);
                research::v1::TaskResponse reply;
                if (!reply.ParseFromString(channel.receive(1s)))
                  throw std::runtime_error("invalid task health");
                protocol::validate_message(reply);
                if (reply.version() != 1 || reply.service_id() != name ||
                    reply.correlation_id() != ping.correlation_id() || !reply.has_health() ||
                    reply.health().instance_id().empty())
                  throw std::runtime_error("task health identity mismatch");
                s.health = reply.health().recovery_required() ? "degraded" : "ready";
              } else if (s.configuration.kind == wire::STRATEGY) {
                strategy::v1::Request ping;
                ping.set_version(1);
                ping.set_session_id(name);
                ping.set_correlation_id("health." + unique_process_id());
                ping.mutable_heartbeat();
                channel.send(ping.SerializeAsString(), 1s);
                strategy::v1::Response reply;
                if (!reply.ParseFromString(channel.receive(1s)))
                  throw std::runtime_error("invalid strategy health");
                protocol::validate_message(reply);
                if (reply.version() != 1 || reply.session_id() != name ||
                    reply.correlation_id() != ping.correlation_id() || !reply.has_health() ||
                    reply.health().instance_id().empty())
                  throw std::runtime_error("strategy health identity mismatch");
                s.health = reply.health().recovery_required() ? "degraded"
                           : reply.health().initialized()     ? "ready"
                                                              : "awaiting_input";
              } else {
                protocol::v1::Request ping;
                ping.set_version(1);
                ping.set_session_id(name);
                ping.set_mode(protocol::v1::PAPER);
                ping.set_correlation_id("health." + unique_process_id());
                ping.mutable_heartbeat();
                channel.send(ping.SerializeAsString(), 1s);
                protocol::v1::Response reply;
                if (!reply.ParseFromString(channel.receive(1s)))
                  throw std::runtime_error("invalid health response");
                protocol::validate_message(reply);
                if (reply.version() != 1 || reply.session_id() != name ||
                    reply.mode() != protocol::v1::PAPER ||
                    reply.correlation_id() != ping.correlation_id() || !reply.has_health())
                  throw std::runtime_error("health identity mismatch");
                s.health = reply.health().recovery_required() ? "degraded"
                           : reply.health().initialized()     ? "ready"
                                                              : "awaiting_input";
              }
              s.last_heartbeat = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count();
              s.failures = 0;
              s.error.clear();
            } catch (const std::exception&) {
              s.health = "unresponsive";
              s.error = "service heartbeat unavailable";
              if (++s.failures >= 3) {
                s.workers.clear();
                s.process.reset();
                s.retry = std::chrono::steady_clock::now() + 5s;
              }
            }
          }
          if (!upgrade_active() && s.configuration.kind == wire::TASK_SERVICE && s.process &&
              s.configuration.desired && std::chrono::steady_clock::now() >= s.dispatch_at) {
            s.dispatch_at = std::chrono::steady_clock::now() + 1s;
            try {
              dispatch_tasks(name, s);
            } catch (const std::exception& e) {
              s.error = e.what();
            }
          }
          if (s.configuration.desired && !s.process && s.restarts < 3 &&
              std::chrono::steady_clock::now() >= s.retry) {
            ++s.restarts;
            start(name, s);
          }
        }
      }
    });
  }
  ~Agent() {
    supervisor_.request_stop();
    supervisor_.join();
    services_.clear();
    if (!sockets_.empty()) {
      std::error_code ec;
      fs::remove_all(sockets_, ec);
    }
  }
  wire::Response dispatch(const wire::Request& r, const std::string& peer, ipc::PeerRole role,
                          std::chrono::steady_clock::time_point deadline, std::stop_token stop) {
    std::lock_guard lock(mutex_);
    wire::Response response;
    response.set_version(1);
    response.set_correlation_id(r.correlation_id());
    try {
      if (stop.stop_requested() || std::chrono::steady_clock::now() >= deadline)
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
      if (r.has_upgrade()) {
        coordinate_upgrade(r.upgrade(), *response.mutable_upgrade());
        return response;
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
        return response;
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
        require_managed_path(file.parent_path());
        require_managed_path(file);
        Json owned = nullptr;
        if (fs::exists(file)) {
          if (fs::file_size(file) > 65536)
            throw std::invalid_argument("invalid firewall record");
          std::ifstream input(file);
          owned = Json::parse(input);
        }
        if (operation.action() == "allow" || operation.action() == "remove") {
          if (!operation.token().empty())
            throw std::invalid_argument("inspection does not accept a confirmation token");
          firewall_plan_ = nullptr;
          auto observed = asterion::node::run_firewall_script(
              os, asterion::node::firewall_inspection(os, peer));
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
          const auto observed = asterion::node::run_firewall_script(
              os, asterion::node::firewall_inspection(os, peer));
          if (!plan.at("can_apply").get<bool>() || observed.at("state") != "active" ||
              observed.at("backend") != plan.at("backend"))
            throw std::invalid_argument("firewall state changed; inspect again");
          const bool remove = plan.at("action") == "remove";
          if (remove && (owned.is_null() || owned.at("rule") != plan.at("rule") ||
                         owned.at("source") != plan.at("source")))
            throw std::invalid_argument("no owned firewall rule");
          if (!remove) {
            fs::create_directory(file.parent_path());
            replace_file_durably(file, plan.dump());
          }
          const auto changed = asterion::node::run_firewall_script(
              os, asterion::node::firewall_change(os, plan.at("source"), service.configuration.port,
                                                  plan.at("rule"), remove));
          if (changed != Json{{"changed", true}})
            throw std::runtime_error("invalid firewall result");
          if (remove)
            fs::remove(file);
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
        return response;
      }
      if (r.has_status()) {
        auto* status = response.mutable_status();
        const auto platform = current_platform();
        status->set_instance_id(instance_);
        status->set_upgrade_protocol(1);
        status->set_maintenance(!maintenance_.empty() || upgrade_active());
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
          service->set_active_workers(static_cast<unsigned>(s.workers.size()));
          service->set_artifact(s.configuration.artifact);
          service->set_revision(revision(s.configuration));
          for (const auto& hash : s.configuration.plugin_artifacts)
            service->add_plugin_artifacts(hash);
          service->set_port(s.configuration.port);
          service->set_desired_running(s.configuration.desired);
          service->set_restarts(s.restarts);
          service->set_error(s.error);
          const bool running = s.process && !s.process->exited();
          service->set_state(running                    ? "running"
                             : !s.configuration.desired ? "stopped"
                             : s.restarts >= 3          ? "failed"
                                                        : "restarting");
          service->set_pid(running ? s.process->id() : 0);
          service->set_health(running ? s.health : "offline");
          service->set_last_heartbeat_ms(s.last_heartbeat);
          if (local_) {
            service->set_endpoint(s.endpoint);
            service->set_directory(s.configuration.directory);
          }
        }
        return response;
      }
      if (r.has_upload()) {
        const auto& u = r.upload();
        validate_artifact_digest(u.sha256());
        const auto platform = current_platform();
        if (u.os() != platform.os || u.arch() != platform.arch || !u.size() ||
            u.size() > max_artifact_bytes)
          throw std::invalid_argument("artifact platform or size mismatch");
        const auto path = root_ / "uploads" / u.sha256();
        require_managed_path(path);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out)
          throw std::runtime_error("cannot create upload");
        uploads_[u.sha256()] = {u.size(), 0};
      } else if (r.has_chunk()) {
        const auto& c = r.chunk();
        validate_artifact_digest(c.sha256());
        auto& u = uploads_.at(c.sha256());
        if (c.offset() != u.offset || c.data().empty() || c.data().size() > 1024 * 1024 ||
            c.data().size() > u.size - u.offset)
          throw std::invalid_argument("invalid upload chunk");
        const auto path = root_ / "uploads" / c.sha256();
        require_managed_path(path);
        std::ofstream out(path, std::ios::binary | std::ios::app);
        out.write(c.data().data(), static_cast<std::streamsize>(c.data().size()));
        out.flush();
        if (!out)
          throw std::runtime_error("upload write failed");
        u.offset += c.data().size();
      } else if (r.has_finish()) {
        const auto hash = r.finish().sha256();
        validate_artifact_digest(hash);
        const auto u = uploads_.at(hash);
        const auto path = root_ / "uploads" / hash;
        if (u.offset != u.size || sha256_file(path) != hash)
          throw std::invalid_argument("artifact size or checksum mismatch");
        const auto actual = artifact_platform(path);
        const auto platform = current_platform();
        if (actual.os != platform.os || actual.arch != platform.arch)
          throw std::invalid_argument("uploaded executable platform mismatch");
        const auto target = binary(hash);
        require_managed_path(target);
        if (fs::exists(target)) {
          if (sha256_file(target) != hash)
            throw std::runtime_error("existing artifact corrupted");
          fs::remove(path);
        } else {
          fs::permissions(path, fs::perms::owner_all);
          fs::rename(path, target);
        }
        uploads_.erase(hash);
      } else if (r.has_deploy()) {
        const auto& d = r.deploy();
        validate_service_id(d.service_id());
        validate_artifact_digest(d.sha256());
        if (d.kind() != wire::PAPER_TRADING && d.kind() != wire::MARKET_DATA &&
            d.kind() != wire::TASK_SERVICE && d.kind() != wire::STRATEGY)
          throw std::invalid_argument("explicit service kind required");
        if (!d.provider_artifact().empty()) {
          if (d.kind() != wire::MARKET_DATA)
            throw std::invalid_argument("provider library only belongs to market data");
          validate_artifact_digest(d.provider_artifact());
          if (sha256_file(binary(d.provider_artifact())) != d.provider_artifact())
            throw std::invalid_argument("provider artifact not installed");
        }
        if (!d.catalog_artifact().empty()) {
          if (d.kind() != wire::MARKET_DATA)
            throw std::invalid_argument("catalog library only belongs to market data");
          validate_artifact_digest(d.catalog_artifact());
          if (sha256_file(binary(d.catalog_artifact())) != d.catalog_artifact())
            throw std::invalid_argument("catalog artifact not installed");
        }
        if (d.kind() == wire::TASK_SERVICE) {
          validate_artifact_digest(d.data_artifact());
          if (sha256_file(binary(d.data_artifact())) != d.data_artifact())
            throw std::invalid_argument("data worker is not installed");
          validate_artifact_digest(d.factor_artifact());
          if (sha256_file(binary(d.factor_artifact())) != d.factor_artifact())
            throw std::invalid_argument("factor worker is not installed");
          validate_artifact_digest(d.worker_artifact());
          if (sha256_file(binary(d.worker_artifact())) != d.worker_artifact())
            throw std::invalid_argument("backtest worker is not installed");
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
        if (sha256_file(binary(d.sha256())) != d.sha256())
          throw std::invalid_argument("artifact not installed");
        const auto folder = root_ / "services" / d.service_id();
        Service s;
        s.configuration.kind = d.kind();
        s.configuration.plugin_artifacts.assign(d.plugin_artifacts().begin(),
                                                d.plugin_artifacts().end());
        plugins_.verify(s.configuration.plugin_artifacts);
        s.configuration.provider_artifact = d.provider_artifact();
        s.configuration.catalog_artifact = d.catalog_artifact();
        s.configuration.worker_artifact = d.worker_artifact();
        s.configuration.factor_artifact = d.factor_artifact();
        s.configuration.data_artifact = d.data_artifact();
        s.configuration.artifact = d.sha256();
        s.configuration.port = static_cast<unsigned short>(d.port());
        if (local_ && s.configuration.kind == wire::PAPER_TRADING) {
          const fs::path ledger(std::u8string(d.directory().begin(), d.directory().end()));
          if (!ledger.is_absolute() || !fs::is_directory(ledger))
            throw std::invalid_argument("local ledger must be an existing absolute directory");
          require_managed_path(ledger);
          s.configuration.directory = utf8(fs::canonical(ledger));
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
        if (!fs::create_directory(folder))
          throw std::invalid_argument("service directory already exists");
        if (!local_ || s.configuration.kind != wire::PAPER_TRADING)
          fs::create_directory(folder / "ledger");
        save(d.service_id(), s.configuration);
        auto [it, added] = services_.emplace(d.service_id(), std::move(s));
        (void)added;
        start(it->first, it->second);
      } else if (r.has_configure_plugins()) {
        configure_plugins(r.configure_plugins());
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
        auto verified = [&](const std::string& hash) {
          validate_artifact_digest(hash);
          const auto path = binary(hash);
          require_managed_path(path);
          if (sha256_file(path) != hash)
            throw std::invalid_argument("update artifact is missing or corrupted");
          const auto actual = artifact_platform(path), platform = current_platform();
          if (actual.os != platform.os || actual.arch != platform.arch)
            throw std::invalid_argument("update artifact platform mismatch");
        };
        verified(u.artifact());
        if (!u.catalog_artifact().empty()) {
          if (current.configuration.kind != wire::MARKET_DATA)
            throw std::invalid_argument("catalog library only belongs to market data");
          verified(u.catalog_artifact());
        }
        if (current.configuration.kind == wire::MARKET_DATA) {
          if (!u.provider_artifact().empty())
            verified(u.provider_artifact());
        } else if (!u.provider_artifact().empty())
          throw std::invalid_argument("provider library only belongs to market data");
        if (current.configuration.kind == wire::TASK_SERVICE) {
          verified(u.worker_artifact());
          verified(u.factor_artifact());
          verified(u.data_artifact());
        } else if (!u.worker_artifact().empty() || !u.factor_artifact().empty() ||
                   !u.data_artifact().empty())
          throw std::invalid_argument("worker artifacts only belong to task service");
        auto next = current.configuration;
        next.plugin_artifacts.assign(u.plugin_artifacts().begin(), u.plugin_artifacts().end());
        plugins_.verify(next.plugin_artifacts);
        next.desired = false;
        next.artifact = u.artifact();
        next.provider_artifact = u.provider_artifact();
        next.catalog_artifact = u.catalog_artifact();
        next.worker_artifact = u.worker_artifact();
        next.factor_artifact = u.factor_artifact();
        next.data_artifact = u.data_artifact();
        // Publish metadata before changing the in-memory selection. No ledger
        // rewrite, automatic restart, or implicit binary rollback occurs.
        save(u.service_id(), next);
        current.process.reset();
        current.configuration = std::move(next);
        current.restarts = 0;
        current.error.clear();
        current.health = "offline";
        current.last_heartbeat = 0;
      } else if (r.has_action()) {
        const auto& a = r.action();
        validate_service_id(a.service_id());
        auto& s = services_.at(a.service_id());
        if (a.kind() != wire::Action::START && a.kind() != wire::Action::STOP &&
            a.kind() != wire::Action::RESTART)
          throw std::invalid_argument("unknown service action");
        const bool desired = a.kind() != wire::Action::STOP;
        const bool previous = s.configuration.desired;
        s.configuration.desired = desired;
        try {
          save(a.service_id(), s.configuration);
        } catch (...) {
          s.configuration.desired = previous;
          throw;
        }
        if (!desired) {
          s.workers.clear();
          s.process.reset();
          s.error.clear();
        } else if (a.kind() == wire::Action::RESTART || !s.process || s.process->exited()) {
          s.workers.clear();
          s.process.reset();
          s.restarts = 0;
          start(a.service_id(), s);
        }
      } else
        throw std::invalid_argument("missing node operation");
      response.mutable_accepted();
    } catch (const std::exception& error) {
      response.mutable_error()->set_code(std::string(error_name(classify(error))));
      response.mutable_error()->set_message(error.what());
    }
    return response;
  }
};
} // namespace
int main(int argc, char** argv) {
  CLI::App app{"Asterion Node Agent: trusted remote deployment and supervision"};
  app.set_version_flag("--version", "Asterion Node Agent 0.1.0");
  std::string system_service;
#ifdef _WIN32
  app.add_option("--windows-service", system_service);
#endif
  std::string directory, transport_log, inspect;
  service::Transport transport;
  auto* inspect_option =
      app.add_option("--inspect-plugin", inspect, "Print a native plugin's identity and exit");
  app.add_option("--directory", directory)->check(CLI::ExistingDirectory)->excludes(inspect_option);
  app.add_option("--endpoint", transport.endpoint);
  app.add_option("--transport-log", transport_log,
                 "Optional absolute path for bounded transport diagnostics");
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
      transport.validate();
      const fs::path root(std::u8string(directory.begin(), directory.end()));
      if (!root.is_absolute() || !fs::is_directory(root))
        throw std::invalid_argument("agent requires an existing absolute directory");
      require_managed_path(root);
      asterion::FileLock ownership(root, "agent.lock");
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
      std::unique_ptr<Logger> transport_logger;
      if (!transport_log.empty()) {
        const fs::path path(std::u8string(transport_log.begin(), transport_log.end()));
        if (!path.is_absolute())
          throw std::invalid_argument("transport log path must be absolute");
        require_managed_path(path);
        transport_logger = std::make_unique<Logger>(
            LoggerOptions{.name = "agent-transport", .stderr_sink = false, .file = path});
      }
      std::unique_ptr<Agent> agent;
      std::atomic<std::uint64_t> next_connection{0};
      // One bounded request per connection. Admission (queueing, TLS and the
      // read) is bounded from accept; business mutations stay serialized in
      // Agent::dispatch.
      auto handle = [&](service::Connection& channel, std::stop_token stop) {
        const auto deadline = channel.accepted_at() + 10s;
        const auto connection = ++next_connection;
        std::string stage = "receive", operation = "unknown";
        const auto report = [&](std::string_view outcome) {
          if (transport_logger) {
            transport_logger->write(LogLevel::info, "agent.transport",
                                    {{"connection", connection},
                                     {"operation", operation},
                                     {"stage", stage},
                                     {"outcome", outcome}});
            transport_logger->flush();
          }
        };
        try {
          const auto left = deadline - std::chrono::steady_clock::now();
          if (stop.stop_requested() || left <= std::chrono::steady_clock::duration::zero())
            throw Error(ErrorCode::unavailable, "Agent request admission timed out");
          wire::Request request;
          const auto payload = channel.receive(std::chrono::ceil<std::chrono::milliseconds>(left));
          stage = "parse";
          if (!request.ParseFromString(payload)) {
            report("rejected");
            return;
          }
          if (const auto* field = request.GetDescriptor()->FindFieldByNumber(
                  static_cast<int>(request.operation_case())))
            operation = field->name();
          stage = "dispatch";
          const auto response =
              agent->dispatch(request, channel.peer_address(), channel.peer_role(), deadline, stop)
                  .SerializeAsString();
          stage = "send";
          channel.send(response, 10s);
          report("completed");
        } catch (const std::exception&) {
          report("failed");
          // Only this peer is closed; never retry an operation or stop Agent.
        }
      };
      // Bind before starting any children: another agent cannot adopt this
      // endpoint while supervised services start.
      service::HostOptions options;
      options.handshake = 10s;
      options.poll = 1s;
      service::ServiceHost host(transport, handle, options);
      agent = std::make_unique<Agent>(root, transport.tls, transport.bind, transport.port);
      if (!host.run())
        std::_Exit(0);
    } catch (const std::exception& error) {
      std::cerr << "Node failed: " << error.what() << '\n';
      return 1;
    }
    return 0;
  };
#ifdef _WIN32
  if (!system_service.empty())
    return asterion::node::run_service(system_service, run, [] { service::request_stop(); });
#endif
  return run();
}
