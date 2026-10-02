#include "sqlite_journal.hpp"
#include <asterion/kernel/native_plugin.hpp>
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
#include <set>
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
  std::optional<wire::Status> cached;
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
  unsigned upgrade_protocol = 0;
  wire::Response call(wire::Request request) {
    request.set_version(1);
    request.set_correlation_id("node." + std::to_string(++sequence));
    const auto timeout = request.has_status() ? 3s : request.has_firewall() ? 30s : 10s;
    std::string stage = "connect";
    bool operation_rejected = false;
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
      if (response.has_error()) {
        // An authenticated, correlated rejection is an operation result, not
        // evidence that the node is unreachable. Status failures still make
        // the cached health unavailable; no rejection refreshes its timestamp.
        operation_rejected = !request.has_status();
        throw_remote_error(response.error().code(), response.error().message());
      }
      if (request.has_status()     ? !response.has_status()
          : request.has_upgrade()  ? !response.has_upgrade()
          : request.has_firewall() ? !response.has_firewall()
                                   : !response.has_accepted())
        throw Error(ErrorCode::unavailable, "unexpected node response");
      return response;
    } catch (const Error& e) {
      const auto diagnostic = "Agent " + operation_name + " " + stage + ": " + e.what();
      if (!operation_rejected) {
        std::lock_guard state_lock(state_mutex);
        connected = false;
        error = diagnostic;
      }
      throw Error(e.code(), diagnostic);
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
    upgrade_protocol = status.upgrade_protocol();
    std::lock_guard state_lock(state_mutex);
    cached = status;
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
Json NodeClient::coordinate_upgrade(const std::string& operation, const std::string& action) {
  std::lock_guard lock(impl_->mutex);
  wire::Request request;
  if (impl_->upgrade_protocol != 1)
    throw Error(ErrorCode::unavailable, "installed Agent does not support coordinated upgrades");
  request.mutable_upgrade()->set_operation_id(operation);
  request.mutable_upgrade()->set_action(action);
  const auto response = impl_->call(std::move(request));
  return {{"phase", response.upgrade().phase()}, {"detail", response.upgrade().detail()}};
}
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
NodeSnapshot NodeClient::inspect_status() const {
  std::lock_guard lock(impl_->state_mutex);
  return {.id = impl_->endpoint.id,
          .host = impl_->endpoint.host,
          .port = impl_->endpoint.port,
          .online = impl_->connected && std::chrono::steady_clock::now() - impl_->last_seen <= 15s,
          .last_heartbeat_ms = impl_->last_heartbeat,
          .latency_ms = impl_->latency,
          .error = impl_->error,
          .health = impl_->cached};
}
std::vector<HistoryService> NodeClient::history_inventory() {
  std::lock_guard lock(impl_->mutex);
  impl_->refresh();
  const auto status = inspect_status();
  std::vector<HistoryService> result;
  const bool local = !impl_->endpoint.endpoint.empty();
  for (const auto& service : status.health->services()) {
    if (service.kind() != wire::PAPER_TRADING && service.kind() != wire::TASK_SERVICE)
      continue;
    if (result.size() >= 1000)
      throw std::invalid_argument("historical service inspection limit exceeded");
    if (!local && service.port() > 65535)
      throw std::invalid_argument("invalid historical service address");
    ServiceEndpoint address{local ? "" : impl_->endpoint.host, service.id(),
                            local ? std::uint16_t(0) : static_cast<std::uint16_t>(service.port()),
                            local ? ipc::TlsIdentity{} : impl_->endpoint.tls,
                            local ? service.endpoint() : ""};
    result.push_back({service.kind(), service.directory(), service.state(), std::move(address)});
  }
  return result;
}
Json NodeClient::status() const {
  return node_snapshot_json(inspect_status());
}
ServiceEndpoint NodeClient::service_endpoint(const std::string& service, wire::ServiceKind kind) {
  std::lock_guard lock(impl_->mutex);
  impl_->refresh();
  for (const auto& s : impl_->cached->services())
    if (s.id() == service) {
      if (s.kind() != kind)
        throw std::invalid_argument("service kind mismatch");
      if (s.state() != "running")
        throw std::invalid_argument("service process is not running");
      return {impl_->endpoint.host, service, static_cast<std::uint16_t>(s.port()),
              impl_->endpoint.tls, s.endpoint()};
    }
  throw std::invalid_argument("unknown managed service");
}
void NodeClient::deploy(const ServiceDeployment& deployment) {
  const auto& programs = deployment.programs;
  const auto& path = programs.executable;
  const auto& os = deployment.platform.os;
  const auto& arch = deployment.platform.arch;
  const auto& service = deployment.service;
  const auto kind = deployment.kind;
  if (!path.is_absolute())
    throw std::invalid_argument("artifact path must be absolute");
  std::lock_guard lock(impl_->mutex);
  impl_->refresh();
  if (impl_->cached->os() != os || impl_->cached->arch() != arch)
    throw std::invalid_argument("selected artifact platform does not match node");
  for (const auto& existing : impl_->cached->services())
    if (existing.id() == service)
      throw std::invalid_argument(
          "service already exists; its program and ledger are not replaced");
  auto upload_artifact = [&](const std::filesystem::path& path) {
    return impl_->upload_artifact(path, os, arch);
  };
  if (kind != wire::PAPER_TRADING && kind != wire::MARKET_DATA && kind != wire::TASK_SERVICE &&
      kind != wire::STRATEGY && kind != wire::LIVE_TRADING)
    throw std::invalid_argument("invalid service kind");
  const auto hash = upload_artifact(path);
  const auto library =
      programs.provider.empty() ? std::string{} : upload_artifact(programs.provider);
  const auto worker_hash =
      programs.worker.empty() ? std::string{} : upload_artifact(programs.worker);
  const auto factor_hash =
      programs.factor.empty() ? std::string{} : upload_artifact(programs.factor);
  const auto data_hash = programs.data.empty() ? std::string{} : upload_artifact(programs.data);
  wire::Request deploy;
  auto* d = deploy.mutable_deploy();
  d->set_service_id(service);
  d->set_sha256(hash);
  d->set_port(deployment.port);
  d->set_directory(deployment.directory);
  d->set_kind(kind);
  d->set_provider_artifact(library);
  d->set_worker_artifact(worker_hash);
  d->set_factor_artifact(factor_hash);
  d->set_data_artifact(data_hash);
  d->set_catalog_artifact(programs.catalog.empty() ? std::string{}
                                                   : upload_artifact(programs.catalog));
  const bool risk = kind == wire::PAPER_TRADING || kind == wire::LIVE_TRADING;
  if ((kind == wire::TASK_SERVICE || risk) && deployment.plugins) {
    for (const auto& artifact : *deployment.plugins) {
      const auto uploaded = upload_artifact(artifact.path);
      if (uploaded != artifact.sha256)
        throw std::invalid_argument("native plugin catalog changed; inspect again");
      d->add_plugin_artifacts(uploaded);
    }
  } else if (kind == wire::TASK_SERVICE || risk) {
    auto folder = path.parent_path() / "plugins";
    if (os == current_platform().os && arch == current_platform().arch)
      folder = native_plugin_directory();
    if (!std::filesystem::is_directory(folder))
      throw std::invalid_argument("native plugin directory is missing from service bundle");
    for (const auto& entry : std::filesystem::directory_iterator(folder))
      if (entry.path().extension() == (os == "macos" ? ".dylib" : ".so") &&
          (kind == wire::TASK_SERVICE || entry.path().stem() == "asterion-order-limits"))
        d->add_plugin_artifacts(upload_artifact(entry.path()));
    if (risk && d->plugin_artifacts_size() != 1)
      throw std::invalid_argument("pre-trade risk plugin is unavailable");
    std::sort(d->mutable_plugin_artifacts()->begin(), d->mutable_plugin_artifacts()->end());
  }
  impl_->call(std::move(deploy));
  impl_->refresh_after_acknowledgement();
}
void NodeClient::update(const ServiceUpdate& update) {
  const auto& service = update.service;
  const auto& revision = update.expected_revision;
  const auto& os = update.platform.os;
  const auto& arch = update.platform.arch;
  std::lock_guard lock(impl_->mutex);
  impl_->refresh();
  std::vector<std::string> plugins;
  bool found = false;
  for (const auto& s : impl_->cached->services())
    if (s.id() == service) {
      found = true;
      plugins = std::vector<std::string>(s.plugin_artifacts().begin(), s.plugin_artifacts().end());
      if (s.desired_running() != false || s.state() != "stopped" || s.revision() != revision)
        throw std::invalid_argument("stop and inspect the current service before updating");
    }
  if (!found || impl_->cached->os() != os || impl_->cached->arch() != arch)
    throw std::invalid_argument("update target mismatch");
  auto upload = [&](const std::filesystem::path& path) {
    return path.empty() ? std::string{} : impl_->upload_artifact(path, os, arch);
  };
  wire::Request request;
  auto* u = request.mutable_update();
  u->set_service_id(service);
  u->set_expected_revision(revision);
  u->set_artifact(upload(update.programs.executable));
  u->set_provider_artifact(upload(update.programs.provider));
  u->set_worker_artifact(upload(update.programs.worker));
  u->set_factor_artifact(upload(update.programs.factor));
  u->set_data_artifact(upload(update.programs.data));
  u->set_catalog_artifact(upload(update.programs.catalog));
  for (const auto& hash : plugins)
    u->add_plugin_artifacts(hash);
  impl_->call(request);
  impl_->refresh_after_acknowledgement();
}
void NodeClient::configure_plugins(const std::string& service, const std::string& revision,
                                   const std::vector<std::string>& hashes) {
  if (impl_->endpoint.endpoint.empty())
    throw std::invalid_argument("native plugin selection requires the local node");
  std::lock_guard lock(impl_->mutex);
  impl_->refresh();
  const wire::Service* current = nullptr;
  for (const auto& item : impl_->cached->services())
    if (item.id() == service)
      current = &item;
  if (!current || current->kind() != wire::TASK_SERVICE)
    throw std::invalid_argument("service does not support managed native plugins");
  if (current->desired_running() || current->state() != "stopped" || current->active_workers() != 0)
    throw std::invalid_argument("stop the service before configuring plugins");
  if (current->revision() != revision)
    throw std::invalid_argument("service configuration changed; inspect again");
  const auto catalog = local_plugin_catalog();
  const std::vector<std::string> installed(current->plugin_artifacts().begin(),
                                           current->plugin_artifacts().end());
  const auto selection = catalog.select_research(hashes, installed);
  const auto platform = current_platform();
  for (const auto& artifact : selection.uploads)
    if (impl_->upload_artifact(artifact.path, platform.os, platform.arch) != artifact.sha256)
      throw std::invalid_argument("native plugin catalog changed; inspect again");
  wire::Request request;
  auto* configuration = request.mutable_configure_plugins();
  configuration->set_service_id(service);
  configuration->set_expected_revision(revision);
  for (const auto& hash : selection.hashes)
    configuration->add_artifacts(hash);
  impl_->call(request);
  impl_->refresh_after_acknowledgement();
}
ServiceEndpoint NodeClient::local_session(const std::filesystem::path& directory,
                                          wire::ServiceKind kind) {
  if (kind != wire::PAPER_TRADING && kind != wire::LIVE_TRADING)
    throw std::invalid_argument("invalid trading session kind");
  if (impl_->endpoint.endpoint.empty())
    throw std::invalid_argument("local Agent required");
  if (!directory.is_absolute() || !std::filesystem::is_directory(directory) ||
      std::filesystem::is_symlink(directory))
    throw std::invalid_argument("choose an existing absolute trading record directory");
  // Reported here: a service that refuses the directory only shows as unreachable.
  check_journal_directory(directory, kind == wire::LIVE_TRADING
                                         ? std::set<std::string>{"plugins", "ctp-flow"}
                                         : std::set<std::string>{"plugins"});
  const auto path = std::filesystem::canonical(directory).u8string();
  const std::string value(path.begin(), path.end());
  std::string service;
  {
    std::lock_guard lock(impl_->mutex);
    impl_->refresh();
    for (const auto& s : impl_->cached->services())
      if (s.directory() == value) {
        if (s.kind() != kind)
          throw std::invalid_argument(kind == wire::LIVE_TRADING
                                          ? "this directory holds a paper session"
                                          : "this directory holds a live session");
        service = s.id();
        break;
      }
  }
  if (service.empty()) {
    service = (kind == wire::LIVE_TRADING ? "live-" : "paper-") + unique_process_id();
    deploy({.service = service,
            .kind = kind,
            .platform = current_platform(),
            .programs = local_service_programs(kind),
            .directory = value});
  } else
    action(service, "start");
  return service_endpoint(service, kind);
}
ServiceEndpoint NodeClient::local_market() {
  if (impl_->endpoint.endpoint.empty())
    throw std::invalid_argument("local Agent required");
  const std::string service = "market-data";
  bool exists = false;
  {
    std::lock_guard lock(impl_->mutex);
    impl_->refresh();
    for (const auto& s : impl_->cached->services())
      if (s.id() == service)
        exists = true;
  }
  if (!exists) {
    auto programs = local_service_programs(wire::MARKET_DATA);
    // Market service can be created before its optional CTP libraries are installed.
    if (!std::filesystem::exists(programs.provider))
      programs.provider.clear();
    if (!std::filesystem::exists(programs.catalog))
      programs.catalog.clear();
    deploy({.service = service,
            .kind = wire::MARKET_DATA,
            .platform = current_platform(),
            .programs = programs});
  } else
    action(service, "start");
  return service_endpoint(service, wire::MARKET_DATA);
}
ServiceEndpoint
NodeClient::local_research(const std::optional<std::vector<std::string>>& selected_plugins) {
  if (impl_->endpoint.endpoint.empty())
    throw std::invalid_argument("local Agent required");
  const std::string service = "research";
  bool exists = false;
  {
    std::lock_guard lock(impl_->mutex);
    impl_->refresh();
    for (const auto& s : impl_->cached->services())
      if (s.id() == service)
        exists = true;
  }
  if (exists && selected_plugins)
    throw std::invalid_argument("research service already exists; configure its plugins");
  std::optional<std::vector<PluginArtifact>> selection;
  if (selected_plugins) {
    const auto catalog = local_plugin_catalog();
    selection = catalog.select_research(*selected_plugins).uploads;
    for (const auto& artifact : *selection)
      if (sha256_file(artifact.path) != artifact.sha256)
        throw std::invalid_argument("native plugin catalog changed; inspect again");
  }
  if (!exists) {
    deploy({.service = service,
            .kind = wire::TASK_SERVICE,
            .platform = current_platform(),
            .programs = local_service_programs(wire::TASK_SERVICE),
            .plugins = selection});
  } else
    action(service, "start");
  return service_endpoint(service, wire::TASK_SERVICE);
}
ServiceEndpoint NodeClient::local_strategy(const std::string& service) {
  if (impl_->endpoint.endpoint.empty())
    throw std::invalid_argument("local Agent required");
  validate_id(service);
  bool exists = false;
  {
    std::lock_guard lock(impl_->mutex);
    impl_->refresh();
    for (const auto& s : impl_->cached->services())
      if (s.id() == service) {
        if (s.kind() != wire::STRATEGY)
          throw std::invalid_argument("service kind mismatch");
        exists = true;
      }
  }
  if (!exists) {
    deploy({.service = service,
            .kind = wire::STRATEGY,
            .platform = current_platform(),
            .programs = local_service_programs(wire::STRATEGY)});
  } else
    action(service, "start");
  return service_endpoint(service, wire::STRATEGY);
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
