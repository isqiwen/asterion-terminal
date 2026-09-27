#include "node_client.hpp"
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/v1/node.pb.h>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include <stdexcept>
namespace asterion::terminal {
namespace wire = asterion::node::v1;
using namespace std::chrono_literals;
struct NodeClient::Impl {
  NodeEndpoint endpoint;
  mutable std::mutex mutex;
  mutable std::mutex state_mutex;
  std::condition_variable wake;
  std::jthread heartbeat;
  Json cached = nullptr;
  bool connected = false;
  std::string error;
  std::int64_t last_heartbeat = 0, latency = 0;
  std::uint64_t sequence = 0;
  std::chrono::steady_clock::time_point last_seen{};
  explicit Impl(NodeEndpoint value) : endpoint(std::move(value)) {
    validate_id(endpoint.id);
    refresh();
    heartbeat = std::jthread([this](std::stop_token token) {
      std::unique_lock lock(mutex);
      while (!wake.wait_for(lock, 5s, [&] { return token.stop_requested(); })) {
        try {
          refresh();
        } catch (const std::exception& e) {
          std::lock_guard state_lock(state_mutex);
          connected = false;
          error = e.what();
        }
      }
    });
  }
  ~Impl() {
    heartbeat.request_stop();
    wake.notify_all();
    if (heartbeat.joinable())
      heartbeat.join();
  }
  wire::Response call(wire::Request request) {
    request.set_version(1);
    request.set_correlation_id("node." + std::to_string(++sequence));
    const auto timeout = request.has_status() ? 3s : request.has_firewall() ? 30s : 10s;
    std::string stage = "connect";
    const auto* operation = request.GetDescriptor()->FindFieldByNumber(request.operation_case());
    const auto operation_name = operation ? operation->name() : "missing";
    try {
      auto exchange = [&](auto channel) {
        stage = "send";
        channel.send(request.SerializeAsString(), timeout);
        stage = "receive";
        return channel.receive(timeout);
      };
      const auto payload = endpoint.endpoint.empty()
                               ? exchange(ipc::TlsChannel::connect(endpoint.host, endpoint.port,
                                                                   endpoint.tls, timeout))
                               : exchange(ipc::Channel::connect(endpoint.endpoint, timeout));
      stage = "validate";
      wire::Response response;
      if (!response.ParseFromString(payload))
        throw Error(ErrorCode::unavailable, "invalid node response");
      protocol::validate_message(response);
      if (response.version() != 1 || response.correlation_id() != request.correlation_id())
        throw Error(ErrorCode::unavailable, "node response identity mismatch");
      if (response.has_error())
        throw_remote_error(response.error().code(), response.error().message());
      if (request.has_status()     ? !response.has_status()
          : request.has_firewall() ? !response.has_firewall()
                                   : !response.has_accepted())
        throw Error(ErrorCode::unavailable, "unexpected node response");
      return response;
    } catch (const Error& e) {
      std::lock_guard state_lock(state_mutex);
      connected = false;
      error = "Agent " + operation_name + " " + stage + ": " + e.what();
      throw Error(e.code(), error);
    }
  }
  std::string upload_artifact(const std::filesystem::path& path, const std::string& os,
                              const std::string& arch) {
    const auto actual = artifact_platform(path);
    if (actual.os != os || actual.arch != arch)
      throw std::invalid_argument("program binary platform does not match selected target");
    const auto hash = sha256_file(path);
    const auto size = std::filesystem::file_size(path);
    wire::Request begin;
    auto* upload = begin.mutable_upload();
    upload->set_sha256(hash);
    upload->set_size(size);
    upload->set_os(os);
    upload->set_arch(arch);
    call(std::move(begin));
    std::ifstream input(path, std::ios::binary);
    std::string buffer(1024 * 1024, '\0');
    std::uint64_t offset = 0;
    while (input.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) ||
           input.gcount()) {
      wire::Request chunk;
      auto* c = chunk.mutable_chunk();
      c->set_sha256(hash);
      c->set_offset(offset);
      c->set_data(buffer.data(), static_cast<std::size_t>(input.gcount()));
      call(std::move(chunk));
      offset += static_cast<std::uint64_t>(input.gcount());
    }
    if (!input.eof() || offset != size)
      throw std::runtime_error("artifact changed or failed while reading");
    wire::Request finish;
    finish.mutable_finish()->set_sha256(hash);
    call(std::move(finish));
    return hash;
  }
  void refresh_after_acknowledgement() {
    // The mutation has already been acknowledged. A failed read must mark
    // telemetry unavailable, not turn confirmed success into a retryable error.
    try {
      refresh();
    } catch (const std::exception& failure) {
      std::lock_guard state_lock(state_mutex);
      connected = false;
      error = failure.what();
    }
  }
  void refresh() {
    const auto start = std::chrono::steady_clock::now();
    wire::Request request;
    request.mutable_status();
    const auto response = call(std::move(request));
    const auto& status = response.status();
    if (status.instance_id().empty() || status.os().empty() || status.arch().empty())
      throw Error(ErrorCode::unavailable, "invalid node health");
    Json services = Json::array();
    for (const auto& s : status.services())
      services.push_back({{"id", s.id()},
                          {"kind", s.kind() == wire::PAPER_TRADING  ? "paper"
                                   : s.kind() == wire::MARKET_DATA  ? "market"
                                   : s.kind() == wire::TASK_SERVICE ? "research"
                                   : s.kind() == wire::STRATEGY     ? "strategy"
                                                                    : "unsupported"},
                          {"active_workers", s.active_workers()},
                          {"artifact", s.artifact()},
                          {"revision", s.revision()},
                          {"port", s.port()},
                          {"state", s.state()},
                          {"desired_running", s.desired_running()},
                          {"pid", s.pid()},
                          {"restarts", s.restarts()},
                          {"error", s.error()},
                          {"health", s.health()},
                          {"last_heartbeat_ms", s.last_heartbeat_ms()},
                          {"endpoint", s.endpoint()},
                          {"directory", s.directory()}});
    std::lock_guard state_lock(state_mutex);
    cached = {{"instance_id", status.instance_id()},
              {"maintenance", status.maintenance()},
              {"pid", status.pid()},
              {"os", status.os()},
              {"arch", status.arch()},
              {"version", status.version()},
              {"uptime_ms", status.uptime_ms()},
              {"services", services}};
    last_heartbeat = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    last_seen = std::chrono::steady_clock::now();
    latency = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - start)
                  .count();
    connected = true;
    error.clear();
  }
};
NodeClient::NodeClient(NodeEndpoint endpoint)
    : impl_(std::make_unique<Impl>(std::move(endpoint))) {}
NodeClient::~NodeClient() = default;
void NodeClient::maintenance(bool enter, const std::string& operation,
                             const std::string& instance) {
  std::lock_guard lock(impl_->mutex);
  wire::Request request;
  auto* maintenance = request.mutable_maintenance();
  maintenance->set_enter(enter);
  maintenance->set_operation_id(operation);
  maintenance->set_instance_id(instance);
  const auto response = impl_->call(std::move(request));
  if (!response.has_accepted())
    throw std::runtime_error("Agent did not accept maintenance");
  impl_->refresh_after_acknowledgement();
}
Json NodeClient::status() const {
  std::lock_guard lock(impl_->state_mutex);
  return {{"id", impl_->endpoint.id},
          {"host", impl_->endpoint.host},
          {"port", impl_->endpoint.port},
          {"state", impl_->connected && std::chrono::steady_clock::now() - impl_->last_seen <= 15s
                        ? "online"
                        : "unreachable"},
          {"last_heartbeat_ms", impl_->last_heartbeat},
          {"latency_ms", impl_->latency},
          {"error", impl_->error},
          {"health", impl_->cached}};
}
ServiceEndpoint NodeClient::service_endpoint(const std::string& service, const std::string& kind) {
  std::lock_guard lock(impl_->mutex);
  impl_->refresh();
  for (const auto& s : impl_->cached.at("services"))
    if (s.at("id") == service) {
      if (s.at("kind") != kind)
        throw std::invalid_argument("service kind mismatch");
      if (s.at("state") != "running")
        throw std::invalid_argument("service process is not running");
      return {impl_->endpoint.host, service, s.at("port").get<std::uint16_t>(), impl_->endpoint.tls,
              s.at("endpoint").get<std::string>()};
    }
  throw std::invalid_argument("unknown managed service");
}
void NodeClient::deploy(const std::filesystem::path& path, const std::string& os,
                        const std::string& arch, const std::string& service, std::uint16_t port,
                        const std::string& directory, const std::string& kind,
                        const std::filesystem::path& provider, const std::filesystem::path& worker,
                        const std::filesystem::path& factor, const std::filesystem::path& data) {
  if (!path.is_absolute())
    throw std::invalid_argument("artifact path must be absolute");
  std::lock_guard lock(impl_->mutex);
  impl_->refresh();
  if (impl_->cached.at("os") != os || impl_->cached.at("arch") != arch)
    throw std::invalid_argument("selected artifact platform does not match node");
  for (const auto& existing : impl_->cached.at("services"))
    if (existing.at("id") == service)
      throw std::invalid_argument(
          "service already exists; its program and ledger are not replaced");
  auto upload_artifact = [&](const std::filesystem::path& path) {
    return impl_->upload_artifact(path, os, arch);
  };
  if (kind != "paper" && kind != "market" && kind != "research" && kind != "strategy")
    throw std::invalid_argument("invalid service kind");
  const auto hash = upload_artifact(path);
  const auto library = provider.empty() ? std::string{} : upload_artifact(provider);
  const auto worker_hash = worker.empty() ? std::string{} : upload_artifact(worker);
  const auto factor_hash = factor.empty() ? std::string{} : upload_artifact(factor);
  const auto data_hash = data.empty() ? std::string{} : upload_artifact(data);
  wire::Request deploy;
  auto* d = deploy.mutable_deploy();
  d->set_service_id(service);
  d->set_sha256(hash);
  d->set_port(port);
  d->set_directory(directory);
  d->set_kind(kind == "market"     ? wire::MARKET_DATA
              : kind == "research" ? wire::TASK_SERVICE
              : kind == "strategy" ? wire::STRATEGY
                                   : wire::PAPER_TRADING);
  d->set_provider_artifact(library);
  d->set_worker_artifact(worker_hash);
  d->set_factor_artifact(factor_hash);
  d->set_data_artifact(data_hash);
  impl_->call(std::move(deploy));
  impl_->refresh_after_acknowledgement();
}
void NodeClient::update(const std::filesystem::path& executable, const std::string& os,
                        const std::string& arch, const std::string& service,
                        const std::string& revision, const std::filesystem::path& provider,
                        const std::filesystem::path& worker, const std::filesystem::path& factor,
                        const std::filesystem::path& data) {
  std::lock_guard lock(impl_->mutex);
  impl_->refresh();
  bool found = false;
  for (const auto& s : impl_->cached.at("services"))
    if (s.at("id") == service) {
      found = true;
      if (s.at("desired_running") != false || s.at("state") != "stopped" ||
          s.at("revision") != revision)
        throw std::invalid_argument("stop and inspect the current service before updating");
    }
  if (!found || impl_->cached.at("os") != os || impl_->cached.at("arch") != arch)
    throw std::invalid_argument("update target mismatch");
  auto upload = [&](const std::filesystem::path& path) {
    return path.empty() ? std::string{} : impl_->upload_artifact(path, os, arch);
  };
  wire::Request request;
  auto* u = request.mutable_update();
  u->set_service_id(service);
  u->set_expected_revision(revision);
  u->set_artifact(upload(executable));
  u->set_provider_artifact(upload(provider));
  u->set_worker_artifact(upload(worker));
  u->set_factor_artifact(upload(factor));
  u->set_data_artifact(upload(data));
  impl_->call(request);
  impl_->refresh_after_acknowledgement();
}
ServiceEndpoint NodeClient::local_session(const std::filesystem::path& directory) {
  if (impl_->endpoint.endpoint.empty())
    throw std::invalid_argument("local Agent required");
  if (!directory.is_absolute() || !std::filesystem::is_directory(directory) ||
      std::filesystem::is_symlink(directory))
    throw std::invalid_argument("choose an existing absolute trading record directory");
  if (std::filesystem::exists(directory / "pending.tmp") ||
      std::filesystem::is_symlink(directory / "pending.tmp"))
    throw std::invalid_argument("incomplete trading journal write; preserve it "
                                "for inspection before recovery");
  const auto path = std::filesystem::canonical(directory).u8string();
  const std::string value(path.begin(), path.end());
  std::string service;
  {
    std::lock_guard lock(impl_->mutex);
    impl_->refresh();
    for (const auto& s : impl_->cached.at("services"))
      if (s.at("directory") == value) {
        service = s.at("id").get<std::string>();
        break;
      }
  }
  if (service.empty()) {
    service = "paper-" + unique_process_id();
    const char* configured = std::getenv("ASTERION_TRADING_EXECUTABLE");
    auto executable =
        configured
            ? std::filesystem::path(std::u8string(configured, configured + std::strlen(configured)))
            : current_executable().parent_path() / (current_platform().os == "windows"
                                                        ? "asterion-trading.exe"
                                                        : "asterion-trading");
    const auto platform = current_platform();
    deploy(executable, platform.os, platform.arch, service, 0, value);
  } else
    action(service, "start");
  return service_endpoint(service);
}
ServiceEndpoint NodeClient::local_market() {
  if (impl_->endpoint.endpoint.empty())
    throw std::invalid_argument("local Agent required");
  const std::string service = "market-data";
  bool exists = false;
  {
    std::lock_guard lock(impl_->mutex);
    impl_->refresh();
    for (const auto& s : impl_->cached.at("services"))
      if (s.at("id") == service)
        exists = true;
  }
  if (!exists) {
    const auto platform = current_platform();
    const auto root = current_executable().parent_path();
    const char* configured = std::getenv("ASTERION_MARKET_EXECUTABLE");
    const auto executable =
        configured
            ? std::filesystem::path(std::u8string(configured, configured + std::strlen(configured)))
            : root /
                  (platform.os == "windows" ? "asterion-market-data.exe" : "asterion-market-data");
    const char* library = std::getenv("ASTERION_CTP_LIBRARY");
    auto sdk = library
                   ? std::filesystem::path(std::u8string(library, library + std::strlen(library)))
                   : root / ("ctp-md" + std::string(platform.os == "windows" ? ".dll"
                                                    : platform.os == "macos" ? ".dylib"
                                                                             : ".so"));
    if (!std::filesystem::exists(sdk))
      sdk.clear();
    deploy(executable, platform.os, platform.arch, service, 0, {}, "market", sdk);
  } else
    action(service, "start");
  return service_endpoint(service, "market");
}
ServiceEndpoint NodeClient::local_research() {
  if (impl_->endpoint.endpoint.empty())
    throw std::invalid_argument("local Agent required");
  const std::string service = "research";
  bool exists = false;
  {
    std::lock_guard lock(impl_->mutex);
    impl_->refresh();
    for (const auto& s : impl_->cached.at("services"))
      if (s.at("id") == service)
        exists = true;
  }
  if (!exists) {
    const auto platform = current_platform();
    const auto root = current_executable().parent_path();
    auto executable = [&](const char* env, const std::string& name) {
      const char* configured = std::getenv(env);
      return configured ? std::filesystem::path(
                              std::u8string(configured, configured + std::strlen(configured)))
                        : root / (name + (platform.os == "windows" ? ".exe" : ""));
    };
    deploy(executable("ASTERION_TASK_EXECUTABLE", "asterion-task-service"), platform.os,
           platform.arch, service, 0, {}, "research", {},
           executable("ASTERION_BACKTEST_EXECUTABLE", "asterion-backtest"),
           executable("ASTERION_FACTOR_EXECUTABLE", "asterion-factor"),
           executable("ASTERION_DATA_PIPELINE_EXECUTABLE", "asterion-data-pipeline"));
  } else
    action(service, "start");
  return service_endpoint(service, "research");
}
ServiceEndpoint NodeClient::local_strategy(const std::string& service) {
  if (impl_->endpoint.endpoint.empty())
    throw std::invalid_argument("local Agent required");
  validate_id(service);
  bool exists = false;
  {
    std::lock_guard lock(impl_->mutex);
    impl_->refresh();
    for (const auto& s : impl_->cached.at("services"))
      if (s.at("id") == service) {
        if (s.at("kind") != "strategy")
          throw std::invalid_argument("service kind mismatch");
        exists = true;
      }
  }
  if (!exists) {
    const auto platform = current_platform();
    const char* configured = std::getenv("ASTERION_STRATEGY_EXECUTABLE");
    const auto executable =
        configured
            ? std::filesystem::path(std::u8string(configured, configured + std::strlen(configured)))
            : current_executable().parent_path() /
                  (platform.os == "windows" ? "asterion-strategy.exe" : "asterion-strategy");
    deploy(executable, platform.os, platform.arch, service, 0, {}, "strategy");
  } else
    action(service, "start");
  return service_endpoint(service, "strategy");
}
Json NodeClient::firewall(const std::string& service, const std::string& action,
                          const std::string& token) {
  std::lock_guard lock(impl_->mutex);
  wire::Request request;
  auto* operation = request.mutable_firewall();
  operation->set_service_id(service);
  operation->set_action(action);
  operation->set_token(token);
  const auto response = impl_->call(std::move(request));
  const auto& plan = response.firewall();
  Json result{{"id", impl_->endpoint.id},
              {"host", impl_->endpoint.host},
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
              {"verification", plan.verification()}};
  if (plan.state() == "applied") {
    try {
      auto channel = ipc::TlsChannel::connect(
          impl_->endpoint.host, static_cast<unsigned short>(plan.port()), impl_->endpoint.tls, 3s);
      result["verification"] = "tls_reachable";
    } catch (const std::exception&) {
      result["verification"] = "unreachable";
    }
  }
  return result;
}
void NodeClient::action(const std::string& service, const std::string& operation) {
  std::lock_guard lock(impl_->mutex);
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
  impl_->call(std::move(request));
  impl_->refresh_after_acknowledgement();
}
} // namespace asterion::terminal
