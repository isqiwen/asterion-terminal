#include "firewall.hpp"
#include "windows_service.hpp"
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
#include <csignal>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <regex>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace fs = std::filesystem;
namespace wire = asterion::node::v1;
using namespace asterion;
using namespace std::chrono_literals;
namespace {
volatile std::sig_atomic_t stopping = 0;
std::atomic<bool> service_stopping{false};
void stop(int) {
  stopping = 1;
}
std::string utf8(const fs::path& p) {
  const auto s = p.u8string();
  return {s.begin(), s.end()};
}
void id(const std::string& s) {
  if (!std::regex_match(s, std::regex("[A-Za-z0-9][A-Za-z0-9_-]{0,63}")))
    throw std::invalid_argument("invalid service id");
}
void digest(const std::string& s) {
  if (!std::regex_match(s, std::regex("[a-f0-9]{64}")))
    throw std::invalid_argument("invalid artifact digest");
}
void safe(const fs::path& p) {
  if (fs::is_symlink(p))
    throw std::invalid_argument("managed paths cannot be symlinks");
}
struct Service {
  wire::ServiceKind kind = wire::UNSPECIFIED_SERVICE;
  std::string provider_artifact, worker_artifact, factor_artifact, data_artifact, worker_endpoint;
  std::map<std::string, std::unique_ptr<ChildProcess>> workers;
  std::chrono::steady_clock::time_point dispatch_at{};
  std::string artifact, error, endpoint, health_endpoint, health = "starting", directory;
  std::int64_t last_heartbeat = 0;
  unsigned failures = 0;
  std::chrono::steady_clock::time_point probe{};
  unsigned short port = 0;
  bool desired = true;
  unsigned int restarts = 0;
  std::unique_ptr<ChildProcess> process;
  std::chrono::steady_clock::time_point retry{};
};
class Agent {
  Json firewall_plan_ = nullptr;
  std::chrono::steady_clock::time_point firewall_expiry_{};
  fs::path root_;
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
  std::mutex mutex_;
  std::jthread supervisor_;
  const std::string instance_ = unique_process_id();
  const std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
  fs::path binary(const std::string& hash) const {
    return root_ / "artifacts" / (hash + (current_platform().os == "windows" ? ".exe" : ".bin"));
  }
  Json configuration(const Service& s) const {
    Json value{{"version", 2},
               {"kind", static_cast<int>(s.kind)},
               {"provider_artifact", s.provider_artifact},
               {"artifact", s.artifact},
               {"port", s.port},
               {"desired", s.desired},
               {"directory", s.directory}};
    if (s.kind == wire::TASK_SERVICE) {
      value["worker_artifact"] = s.worker_artifact;
      value["factor_artifact"] = s.factor_artifact;
      value["data_artifact"] = s.data_artifact;
    }
    return value;
  }
  std::string revision(const Service& s) const { return sha256_bytes(configuration(s).dump()); }
  void save(const std::string& name, const Service& s) {
    const auto path = root_ / "services" / name / "service.json";
    const auto pending = path.parent_path() / "service.pending";
    safe(path);
    safe(pending);
    if (fs::exists(pending))
      throw std::runtime_error("unfinished service configuration requires explicit recovery");
    const auto configuration = this->configuration(s);
    {
      std::ofstream out(pending, std::ios::binary);
      out << configuration.dump();
      out.flush();
      if (!out)
        throw std::runtime_error("cannot persist service configuration");
    }
#ifdef _WIN32
    if (!MoveFileExW(pending.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
      throw std::runtime_error("cannot publish service configuration");
#else
    fs::rename(pending, path);
#endif
  }
  void start(const std::string& name, Service& s) {
    s.workers.clear();
    try {
      const auto executable = binary(s.artifact);
      if (sha256_file(executable) != s.artifact)
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
                                    "--directory",       s.directory,
                                    "--health-endpoint", s.health_endpoint};
      if (s.kind == wire::TASK_SERVICE) {
        if (sha256_file(binary(s.data_artifact)) != s.data_artifact)
          throw std::runtime_error("data worker integrity check failed");
        if (sha256_file(binary(s.factor_artifact)) != s.factor_artifact)
          throw std::runtime_error("factor worker integrity check failed");
        if (sha256_file(binary(s.worker_artifact)) != s.worker_artifact)
          throw std::runtime_error("backtest worker integrity check failed");
        args.insert(args.end(), {"--worker-endpoint", s.worker_endpoint});
      }
      if (s.kind == wire::PAPER_TRADING)
        args.insert(args.end(), {"--mode", "paper"});
      if (s.kind == wire::MARKET_DATA && !s.provider_artifact.empty()) {
        const auto source = binary(s.provider_artifact);
        if (sha256_file(source) != s.provider_artifact)
          throw std::runtime_error("provider artifact integrity check failed");
        const auto library =
            root_ / "services" / name /
            (s.provider_artifact + std::string(current_platform().os == "windows" ? ".dll"
                                               : current_platform().os == "macos" ? ".dylib"
                                                                                  : ".so"));
        safe(library);
        if (!fs::exists(library))
          fs::copy_file(source, library);
        else if (sha256_file(library) != s.provider_artifact)
          throw std::runtime_error("provider library changed");
        args.insert(args.end(), {"--ctp-library", utf8(library)});
      }
      if (local_)
        args.insert(args.end(), {"--endpoint", s.endpoint});
      else
        args.insert(args.end(),
                    {"--bind", bind_, "--port", std::to_string(s.port), "--tls-ca", tls_.ca_file,
                     "--tls-cert", tls_.certificate_file, "--tls-key", tls_.private_key_file});
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
  void dispatch_tasks(const std::string& name, Service& service) {
    for (auto it = service.workers.begin(); it != service.workers.end();) {
      if (it->second->exited())
        it = service.workers.erase(it);
      else
        ++it;
    }
    if (service.workers.size() >= 2)
      return;
    research::v1::TaskRequest request;
    request.set_version(1);
    request.set_service_id(name);
    request.set_correlation_id(unique_process_id());
    request.mutable_list();
    auto channel = ipc::Channel::connect(service.worker_endpoint, 500ms);
    channel.send(request.SerializeAsString(), 1s);
    research::v1::TaskResponse response;
    if (!response.ParseFromString(channel.receive(1s)))
      throw std::runtime_error("invalid task dispatch response");
    protocol::validate_message(response);
    if (response.version() != 1 || response.service_id() != name ||
        response.correlation_id() != request.correlation_id() || !response.has_tasks())
      throw std::runtime_error("task queue unavailable");
    for (const auto& task : response.tasks().tasks()) {
      if (service.workers.size() >= 2)
        break;
      if (task.state() != research::v1::QUEUED || service.workers.contains(task.id()))
        continue;
      if (task.kind() != research::v1::BACKTEST && task.kind() != research::v1::FACTOR &&
          task.kind() != research::v1::DATA_IMPORT && task.kind() != research::v1::CALENDAR_IMPORT)
        throw std::runtime_error("unsupported queued task kind");
      validate_id(task.id());
      const auto artifact =
          (task.kind() == research::v1::DATA_IMPORT || task.kind() == research::v1::CALENDAR_IMPORT)
              ? service.data_artifact
          : task.kind() == research::v1::FACTOR ? service.factor_artifact
                                                : service.worker_artifact;
      const auto worker = binary(artifact);
      if (sha256_file(worker) != artifact)
        throw std::runtime_error("research worker integrity check failed");
      std::vector<std::string> args{"--owner-pid", std::to_string(current_process_id()),
                                    "--endpoint",  service.worker_endpoint,
                                    "--session",   name,
                                    "--task",      task.id()};
      if (task.kind() == research::v1::CALENDAR_IMPORT)
        args.push_back("--settlement-calendar");
      service.workers.emplace(task.id(), std::make_unique<ChildProcess>(worker, args));
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
    safe(root_);
    for (const auto* name : {"artifacts", "services", "uploads"}) {
      safe(root_ / name);
      fs::create_directory(root_ / name);
    }
    for (const auto& entry : fs::directory_iterator(root_ / "services")) {
      safe(entry.path());
      const auto name = entry.path().filename().string();
      id(name);
      const auto path = entry.path() / "service.json";
      safe(path);
      safe(entry.path() / "service.pending");
      if (fs::exists(entry.path() / "service.pending"))
        throw std::runtime_error("unfinished service configuration requires explicit recovery");
      if (!fs::is_regular_file(path) || fs::file_size(path) > 65536)
        throw std::invalid_argument("invalid managed configuration file");
      std::ifstream input(path);
      std::string raw{std::istreambuf_iterator<char>(input), {}};
      auto j = parse_json(raw, 65536);
      if (j.size() != (j.at("kind") == static_cast<int>(wire::TASK_SERVICE) ? 10 : 7) ||
          j.at("version") != 2 || !j.at("desired").is_boolean() ||
          !j.at("port").is_number_unsigned())
        throw std::invalid_argument("invalid managed service configuration");
      Service s;
      s.kind = static_cast<wire::ServiceKind>(j.at("kind").get<int>());
      s.provider_artifact = j.at("provider_artifact").get<std::string>();
      if (s.kind != wire::PAPER_TRADING && s.kind != wire::MARKET_DATA &&
          s.kind != wire::TASK_SERVICE && s.kind != wire::STRATEGY)
        throw std::invalid_argument("unsupported service kind");
      if (!s.provider_artifact.empty()) {
        if (s.kind != wire::MARKET_DATA)
          throw std::invalid_argument("provider library only belongs to market data");
        digest(s.provider_artifact);
      }
      if (s.kind == wire::TASK_SERVICE) {
        s.worker_artifact = j.at("worker_artifact").get<std::string>();
        digest(s.worker_artifact);
        s.factor_artifact = j.at("factor_artifact").get<std::string>();
        digest(s.factor_artifact);
        s.data_artifact = j.at("data_artifact").get<std::string>();
        digest(s.data_artifact);
        if (!s.provider_artifact.empty())
          throw std::invalid_argument("task service does not load a market provider");
      }
      s.artifact = j.at("artifact").get<std::string>();
      digest(s.artifact);
      const auto port = j.at("port").get<unsigned int>();
      if (port > 65535 || (local_ ? port != 0 : (!port || port == control_port_)))
        throw std::invalid_argument("invalid managed port");
      s.port = static_cast<unsigned short>(port);
      s.desired = j.at("desired").get<bool>();
      s.directory = j.at("directory").get<std::string>();
      const fs::path ledger(std::u8string(s.directory.begin(), s.directory.end()));
      if (!ledger.is_absolute())
        throw std::invalid_argument("ledger must be absolute");
      safe(ledger);
      if (!fs::is_directory(ledger))
        throw std::invalid_argument("managed ledger is missing");
      services_.emplace(name, std::move(s));
    }
    for (auto& [name, service] : services_)
      if (service.desired)
        start(name, service);
    supervisor_ = std::jthread([this](std::stop_token token) {
      while (!token.stop_requested()) {
        std::this_thread::sleep_for(200ms);
        std::lock_guard lock(mutex_);
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
              if (s.kind == wire::MARKET_DATA) {
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
              } else if (s.kind == wire::TASK_SERVICE) {
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
              } else if (s.kind == wire::STRATEGY) {
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
          if (s.kind == wire::TASK_SERVICE && s.process && s.desired &&
              std::chrono::steady_clock::now() >= s.dispatch_at) {
            s.dispatch_at = std::chrono::steady_clock::now() + 1s;
            try {
              dispatch_tasks(name, s);
            } catch (const std::exception& e) {
              s.error = e.what();
            }
          }
          if (s.desired && !s.process && s.restarts < 3 &&
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
  wire::Response dispatch(const wire::Request& r, const std::string& peer,
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
      if (r.has_maintenance()) {
        const auto& request = r.maintenance();
        validate_id(request.operation_id());
        if (request.instance_id() != instance_)
          throw std::invalid_argument("Agent instance changed");
        if (request.enter()) {
          if (!maintenance_.empty() && maintenance_ != request.operation_id())
            throw std::runtime_error("Agent already belongs to another maintenance operation");
          for (const auto& [name, service] : services_)
            if (service.desired || (service.process && !service.process->exited()) ||
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
        id(operation.service_id());
        const auto& service = services_.at(operation.service_id());
        const auto os = current_platform().os;
        const auto file = root_ / "firewall" / (operation.service_id() + ".json");
        safe(file.parent_path());
        safe(file);
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
              {"port", service.port},
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
            std::ofstream output(file);
            output << plan.dump();
            output.close();
            if (!output)
              throw std::runtime_error("cannot record firewall ownership");
          }
          const auto changed = asterion::node::run_firewall_script(
              os, asterion::node::firewall_change(os, plan.at("source"), service.port,
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
        report->set_token(plan.at("token"));
        report->set_source(plan.at("source"));
        report->set_port(plan.at("port"));
        report->set_backend(plan.at("backend"));
        report->set_state(plan.at("state"));
        report->set_can_apply(plan.at("can_apply"));
        report->set_rule(plan.at("rule"));
        report->set_action(plan.at("action"));
        report->set_verification(plan.at("verification"));
        return response;
      }
      if (r.has_status()) {
        auto* status = response.mutable_status();
        const auto platform = current_platform();
        status->set_instance_id(instance_);
        status->set_maintenance(!maintenance_.empty());
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
          service->set_kind(s.kind);
          service->set_active_workers(static_cast<unsigned>(s.workers.size()));
          service->set_artifact(s.artifact);
          service->set_revision(revision(s));
          service->set_port(s.port);
          service->set_desired_running(s.desired);
          service->set_restarts(s.restarts);
          service->set_error(s.error);
          const bool running = s.process && !s.process->exited();
          service->set_state(running           ? "running"
                             : !s.desired      ? "stopped"
                             : s.restarts >= 3 ? "failed"
                                               : "restarting");
          service->set_pid(running ? s.process->id() : 0);
          service->set_health(running ? s.health : "offline");
          service->set_last_heartbeat_ms(s.last_heartbeat);
          if (local_) {
            service->set_endpoint(s.endpoint);
            service->set_directory(s.directory);
          }
        }
        return response;
      }
      if (r.has_upload()) {
        const auto& u = r.upload();
        digest(u.sha256());
        const auto platform = current_platform();
        if (u.os() != platform.os || u.arch() != platform.arch || !u.size() ||
            u.size() > 128 * 1024 * 1024)
          throw std::invalid_argument("artifact platform or size mismatch");
        const auto path = root_ / "uploads" / u.sha256();
        safe(path);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out)
          throw std::runtime_error("cannot create upload");
        uploads_[u.sha256()] = {u.size(), 0};
      } else if (r.has_chunk()) {
        const auto& c = r.chunk();
        digest(c.sha256());
        auto& u = uploads_.at(c.sha256());
        if (c.offset() != u.offset || c.data().empty() || c.data().size() > 1024 * 1024 ||
            c.data().size() > u.size - u.offset)
          throw std::invalid_argument("invalid upload chunk");
        const auto path = root_ / "uploads" / c.sha256();
        safe(path);
        std::ofstream out(path, std::ios::binary | std::ios::app);
        out.write(c.data().data(), static_cast<std::streamsize>(c.data().size()));
        out.flush();
        if (!out)
          throw std::runtime_error("upload write failed");
        u.offset += c.data().size();
      } else if (r.has_finish()) {
        const auto hash = r.finish().sha256();
        digest(hash);
        const auto u = uploads_.at(hash);
        const auto path = root_ / "uploads" / hash;
        if (u.offset != u.size || sha256_file(path) != hash)
          throw std::invalid_argument("artifact size or checksum mismatch");
        const auto actual = artifact_platform(path);
        const auto platform = current_platform();
        if (actual.os != platform.os || actual.arch != platform.arch)
          throw std::invalid_argument("uploaded executable platform mismatch");
        const auto target = binary(hash);
        safe(target);
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
        id(d.service_id());
        digest(d.sha256());
        if (d.kind() != wire::PAPER_TRADING && d.kind() != wire::MARKET_DATA &&
            d.kind() != wire::TASK_SERVICE && d.kind() != wire::STRATEGY)
          throw std::invalid_argument("explicit service kind required");
        if (!d.provider_artifact().empty()) {
          if (d.kind() != wire::MARKET_DATA)
            throw std::invalid_argument("provider library only belongs to market data");
          digest(d.provider_artifact());
          if (sha256_file(binary(d.provider_artifact())) != d.provider_artifact())
            throw std::invalid_argument("provider artifact not installed");
        }
        if (d.kind() == wire::TASK_SERVICE) {
          digest(d.data_artifact());
          if (sha256_file(binary(d.data_artifact())) != d.data_artifact())
            throw std::invalid_argument("data worker is not installed");
          digest(d.factor_artifact());
          if (sha256_file(binary(d.factor_artifact())) != d.factor_artifact())
            throw std::invalid_argument("factor worker is not installed");
          digest(d.worker_artifact());
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
          if (d.port() && s.port == d.port())
            throw std::invalid_argument("port already assigned");
        }
        if (sha256_file(binary(d.sha256())) != d.sha256())
          throw std::invalid_argument("artifact not installed");
        const auto folder = root_ / "services" / d.service_id();
        Service s;
        s.kind = d.kind();
        s.provider_artifact = d.provider_artifact();
        s.worker_artifact = d.worker_artifact();
        s.factor_artifact = d.factor_artifact();
        s.data_artifact = d.data_artifact();
        s.artifact = d.sha256();
        s.port = static_cast<unsigned short>(d.port());
        if (local_ && s.kind == wire::PAPER_TRADING) {
          const fs::path ledger(std::u8string(d.directory().begin(), d.directory().end()));
          if (!ledger.is_absolute() || !fs::is_directory(ledger))
            throw std::invalid_argument("local ledger must be an existing absolute directory");
          safe(ledger);
          s.directory = utf8(fs::canonical(ledger));
          for (const auto& [name, existing] : services_) {
            (void)name;
            if (existing.directory == s.directory)
              throw std::invalid_argument("ledger already managed by another service");
          }
        } else {
          if (!d.directory().empty())
            throw std::invalid_argument("service directory is Agent-owned");
          s.directory = utf8(folder / "ledger");
        }
        if (!fs::create_directory(folder))
          throw std::invalid_argument("service directory already exists");
        if (!local_ || s.kind != wire::PAPER_TRADING)
          fs::create_directory(folder / "ledger");
        save(d.service_id(), s);
        auto [it, added] = services_.emplace(d.service_id(), std::move(s));
        (void)added;
        start(it->first, it->second);
      } else if (r.has_update()) {
        const auto& u = r.update();
        id(u.service_id());
        digest(u.expected_revision());
        auto& current = services_.at(u.service_id());
        if (current.desired || (current.process && !current.process->exited()) ||
            !current.workers.empty())
          throw std::invalid_argument("stop the service before updating its programs");
        if (revision(current) != u.expected_revision())
          throw std::invalid_argument("service configuration changed; inspect again");
        auto verified = [&](const std::string& hash) {
          digest(hash);
          const auto path = binary(hash);
          safe(path);
          if (sha256_file(path) != hash)
            throw std::invalid_argument("update artifact is missing or corrupted");
          const auto actual = artifact_platform(path), platform = current_platform();
          if (actual.os != platform.os || actual.arch != platform.arch)
            throw std::invalid_argument("update artifact platform mismatch");
        };
        verified(u.artifact());
        if (current.kind == wire::MARKET_DATA) {
          if (!u.provider_artifact().empty())
            verified(u.provider_artifact());
        } else if (!u.provider_artifact().empty())
          throw std::invalid_argument("provider library only belongs to market data");
        if (current.kind == wire::TASK_SERVICE) {
          verified(u.worker_artifact());
          verified(u.factor_artifact());
          verified(u.data_artifact());
        } else if (!u.worker_artifact().empty() || !u.factor_artifact().empty() ||
                   !u.data_artifact().empty())
          throw std::invalid_argument("worker artifacts only belong to task service");
        Service next;
        next.kind = current.kind;
        next.port = current.port;
        next.directory = current.directory;
        next.desired = false;
        next.artifact = u.artifact();
        next.provider_artifact = u.provider_artifact();
        next.worker_artifact = u.worker_artifact();
        next.factor_artifact = u.factor_artifact();
        next.data_artifact = u.data_artifact();
        // Publish metadata before changing the in-memory selection. No ledger
        // rewrite, automatic restart, or implicit binary rollback occurs.
        save(u.service_id(), next);
        current.process.reset();
        current.artifact = next.artifact;
        current.provider_artifact = next.provider_artifact;
        current.worker_artifact = next.worker_artifact;
        current.factor_artifact = next.factor_artifact;
        current.data_artifact = next.data_artifact;
        current.restarts = 0;
        current.error.clear();
        current.health = "offline";
        current.last_heartbeat = 0;
      } else if (r.has_action()) {
        const auto& a = r.action();
        id(a.service_id());
        auto& s = services_.at(a.service_id());
        if (a.kind() != wire::Action::START && a.kind() != wire::Action::STOP &&
            a.kind() != wire::Action::RESTART)
          throw std::invalid_argument("unknown service action");
        const bool desired = a.kind() != wire::Action::STOP;
        const bool previous = s.desired;
        s.desired = desired;
        try {
          save(a.service_id(), s);
        } catch (...) {
          s.desired = previous;
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
  std::string directory, bind, endpoint, transport_log;
  unsigned short port = 0;
  ipc::TlsIdentity tls;
  app.add_option("--directory", directory)->required()->check(CLI::ExistingDirectory);
  app.add_option("--endpoint", endpoint);
  app.add_option("--transport-log", transport_log,
                 "Optional absolute path for bounded transport diagnostics");
  app.add_option("--bind", bind);
  app.add_option("--port", port)->check(CLI::Range(1, 65535));
  app.add_option("--tls-ca", tls.ca_file);
  app.add_option("--tls-cert", tls.certificate_file);
  app.add_option("--tls-key", tls.private_key_file);
  argv = app.ensure_utf8(argv);
  CLI11_PARSE(app, argc, argv);
  auto run = [&]() -> int {
    try {
      std::signal(SIGINT, stop);
      std::signal(SIGTERM, stop);
      // Bind before starting any children: another agent cannot adopt this
      // port.
      const bool local = !endpoint.empty();
      if (local ? (!bind.empty() || port || !tls.ca_file.empty() || !tls.certificate_file.empty() ||
                   !tls.private_key_file.empty())
                : (bind.empty() || !port || tls.ca_file.empty() || tls.certificate_file.empty() ||
                   tls.private_key_file.empty()))
        throw std::invalid_argument("choose local endpoint OR TCP with complete TLS identity");
      const fs::path root(std::u8string(directory.begin(), directory.end()));
      if (!root.is_absolute() || !fs::is_directory(root))
        throw std::invalid_argument("agent requires an existing absolute directory");
      safe(root);
      asterion::FileLock ownership(root, "agent.lock");
      std::unique_ptr<ipc::TlsListener> tcp;
      std::unique_ptr<ipc::Listener> ipc_listener;
      if (local) {
#ifndef _WIN32
        safe(endpoint);
        fs::remove(endpoint);
#endif
        ipc_listener = std::make_unique<ipc::Listener>(endpoint);
      } else
        tcp = std::make_unique<ipc::TlsListener>(bind, port, tls);
      const auto pid_file = root / "agent.pid";
      safe(pid_file);
      {
        std::ofstream pid(pid_file);
        pid << current_process_id();
      }
      std::unique_ptr<Logger> transport;
      if (!transport_log.empty()) {
        const fs::path path(std::u8string(transport_log.begin(), transport_log.end()));
        if (!path.is_absolute())
          throw std::invalid_argument("transport log path must be absolute");
        safe(path);
        transport = std::make_unique<Logger>(
            LoggerOptions{.name = "agent-transport", .stderr_sink = false, .file = path});
      }
      Agent agent(root, tls, bind, port);
      std::uint64_t next_connection = 0;
      // Destroy/join clients before Agent and its supervised state. Admission,
      // handshake and reads are bounded; business mutations remain serialized.
      ThreadPool clients(8, 8);
      auto dispatch = [&](auto pending) {
        auto peer = std::make_shared<decltype(pending)>(std::move(pending));
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        const auto connection = ++next_connection;
        static_cast<void>(
            clients.submit([peer, &agent, &transport, deadline, connection](std::stop_token stop) {
              std::string stage = "handshake", operation = "unknown";
              const auto report = [&](std::string_view outcome) {
                if (transport) {
                  transport->write(LogLevel::info, "agent.transport",
                                   {{"connection", connection},
                                    {"operation", operation},
                                    {"stage", stage},
                                    {"outcome", outcome}});
                  transport->flush();
                }
              };
              try {
                auto remaining = [&] {
                  const auto left = deadline - std::chrono::steady_clock::now();
                  if (stop.stop_requested() || left <= std::chrono::steady_clock::duration::zero())
                    throw Error(ErrorCode::unavailable, "Agent request admission timed out");
                  return std::chrono::ceil<std::chrono::milliseconds>(left);
                };
                auto channel = [&] {
                  if constexpr (requires { std::move(*peer).handshake(10s); })
                    return std::move(*peer).handshake(remaining());
                  else
                    return std::move(*peer);
                }();
                stage = "receive";
                wire::Request request;
                const auto payload = channel.receive(remaining());
                stage = "parse";
                if (!request.ParseFromString(payload)) {
                  report("rejected");
                  return;
                }
                if (const auto* field = request.GetDescriptor()->FindFieldByNumber(
                        static_cast<int>(request.operation_case())))
                  operation = field->name();
                stage = "peer";
                std::string address;
                if constexpr (requires { channel.peer_address(); })
                  address = channel.peer_address();
                stage = "dispatch";
                const auto response =
                    agent.dispatch(request, address, deadline, stop).SerializeAsString();
                stage = "send";
                channel.send(response, 10s);
                report("completed");
              } catch (const std::exception&) {
                report("failed");
                // Only this peer is closed; never retry an operation or stop
                // Agent.
              }
            }));
      };
      while (!stopping && !service_stopping.load()) {
        try {
          if (local)
            dispatch(ipc_listener->accept(1s));
          else
            dispatch(tcp->accept_pending(1s));
        } catch (const Error&) {
          // Idle timeout, rejected peer or full bounded queue. Unscheduled
          // channels are destroyed here, including unauthenticated TCP peers.
        }
      }
    } catch (const std::exception& error) {
      std::cerr << "Node failed: " << error.what() << '\n';
      return 1;
    }
    return 0;
  };
#ifdef _WIN32
  if (!system_service.empty())
    return asterion::node::run_service(system_service, run, [] { service_stopping = true; });
#endif
  return run();
}
