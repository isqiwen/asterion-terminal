#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/protocol/task_client.hpp>
#include <iostream>
#include <stdexcept>
namespace asterion::protocol {
using namespace std::chrono_literals;
int run_task_worker(const std::string& endpoint, const std::string& host, unsigned short port,
                    const ipc::TlsIdentity& tls, const std::string& service,
                    const std::string& task, research::v1::TaskKind kind,
                    const TaskRunner& runner) {
  if (kind != research::v1::BACKTEST && kind != research::v1::FACTOR &&
      kind != research::v1::DAILY_FACTOR && kind != research::v1::DATA_IMPORT &&
      kind != research::v1::CALENDAR_IMPORT && kind != research::v1::MINUTE_DOWNLOAD &&
      kind != research::v1::DAILY_DOWNLOAD)
    throw std::invalid_argument("unsupported worker kind");
  validate_id(service);
  validate_id(task);
  const bool remote = !host.empty();
  if (remote ? (!endpoint.empty() || !port || tls.ca_file.empty() || tls.certificate_file.empty() ||
                tls.private_key_file.empty())
             : (endpoint.empty() || port || !tls.ca_file.empty() || !tls.certificate_file.empty() ||
                !tls.private_key_file.empty()))
    throw std::invalid_argument(
        "worker requires local endpoint OR TCP host/port with TLS identity");
  namespace wire = research::v1;
  auto call = [&](wire::TaskRequest request) {
    request.set_version(1);
    request.set_service_id(service);
    request.set_correlation_id(unique_process_id());
    auto exchange = [&](auto channel) {
      channel.send(request.SerializeAsString(), 5s);
      return channel.receive(5s);
    };
    const auto raw = remote ? exchange(ipc::TlsChannel::connect(host, port, tls, 5s))
                            : exchange(ipc::Channel::connect(endpoint, 5s));
    wire::TaskResponse response;
    if (!response.ParseFromString(raw))
      throw std::runtime_error("invalid task service response");
    protocol::validate_message(response);
    if (response.version() != 1 || response.service_id() != service ||
        response.correlation_id() != request.correlation_id())
      throw std::runtime_error("task response identity mismatch");
    if (response.has_error())
      throw_remote_error(response.error().code(), response.error().message());
    if (request.has_claim() ? !response.has_attempt() : !response.has_task())
      throw std::runtime_error("unexpected task response");
    return response;
  };
  wire::TaskRequest claim;
  claim.mutable_claim()->set_id(task);
  claim.mutable_claim()->set_kind(kind);
  const auto attempt = call(claim).attempt();
  if (attempt.task().id() != task || attempt.task().state() != wire::RUNNING ||
      attempt.token().empty() || attempt.task().kind() != kind)
    throw std::runtime_error("invalid claimed task");
  const auto token = attempt.token();
  std::stop_source cancel;
  auto last = std::chrono::steady_clock::now() - 1s;
  try {
    const auto result = runner(attempt, cancel.get_token(), [&](auto completed, auto total) {
      const auto now = std::chrono::steady_clock::now();
      if (now - last < 100ms && completed != total)
        return;
      wire::TaskRequest progress;
      auto* p = progress.mutable_progress();
      p->set_id(task);
      p->set_token(token);
      p->set_completed(static_cast<unsigned>(completed));
      const auto state = call(progress).task().state();
      if (state == wire::CANCEL_REQUESTED)
        cancel.request_stop();
      else if (state != wire::RUNNING)
        throw std::runtime_error("task attempt is no longer active");
      last = now;
    });
    wire::TaskRequest finish;
    auto* f = finish.mutable_finish();
    *f = result;
    f->set_id(task);
    f->set_token(token);
    const auto state = call(finish).task().state();
    if (state == wire::CANCELLED)
      return 2;
    if (state != wire::SUCCEEDED)
      throw std::runtime_error("task completion was not confirmed");
    std::cout << "Completed task " << task << '\n';
    return 0;
  } catch (const std::exception& error) {
    // Never retry a business command blindly. A lost completion response may
    // already have committed a result; fencing prevents this report undoing it.
    try {
      wire::TaskRequest report;
      if (cancel.stop_requested()) {
        auto* p = report.mutable_cancel_ack();
        p->set_id(task);
        p->set_token(token);
      } else {
        auto* p = report.mutable_fail();
        p->set_id(task);
        p->set_token(token);
        p->set_error(std::string(error.what()).substr(0, 1024));
        p->set_error_code(std::string(error_name(classify(error))));
      }
      static_cast<void>(call(report));
    } catch (const std::exception&) {
    }
    if (cancel.stop_requested())
      return 2;
    throw;
  }
}
} // namespace asterion::protocol
