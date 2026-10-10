#include <asterion/kernel/ipc/rpc_client.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/owner.hpp>
#include <asterion/kernel/trace.hpp>
#include <asterion/kernel/logger.hpp>
#include <asterion/protocol/task_client.hpp>
#include "task_input.hpp"
#include <asterion/protocol/rpc_diagnostics.hpp>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <thread>
namespace asterion::protocol {
using namespace std::chrono_literals;
int run_task_worker(const std::string& endpoint, const std::string& host, unsigned short port,
                    const ipc::TlsIdentity& tls, const std::string& service,
                    const std::string& task, task::v1::TaskKind kind, const TaskRunner& runner,
                    std::uint64_t owner_pid) {
  if (kind != task::v1::BACKTEST && kind != task::v1::FACTOR && kind != task::v1::MINUTE_DOWNLOAD &&
      kind != task::v1::DAILY_DOWNLOAD)
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
  std::unique_ptr<ProcessOwner> owner;
  if (owner_pid)
    owner = std::make_unique<ProcessOwner>(owner_pid);
  // One business request and one reserved progress request; no extra I/O thread.
  auto client =
      remote
          ? std::make_unique<ipc::RpcClient>(host, port, tls, 2, PayloadBudget{128 * 1024 * 1024})
          : std::make_unique<ipc::RpcClient>(endpoint, 2, PayloadBudget{128 * 1024 * 1024});
  const auto worker_trace = next_correlation_id();
  TraceScope context(worker_trace);
  namespace wire = task::v1;
  struct Pending {
    wire::TaskRequest request;
    std::future<Payload> reply;
  };
  auto begin = [&](wire::TaskRequest request) {
    TraceScope call_context(request.has_progress() ? std::string_view{} : worker_trace);
    request.set_version(1);
    request.set_service_id(service);
    request.set_correlation_id(next_correlation_id());
    auto reply = client->request(request.SerializeAsString(),
                                 request.has_finish()  ? task_verification_timeout + 5s
                                 : request.has_claim() ? task_input_timeout + 5s
                                                       : 5s);
    return Pending{std::move(request), std::move(reply)};
  };
  auto response = [&](Pending pending) {
    const auto& request = pending.request;
    wire::TaskResponse result;
    if (!result.ParseFromString(*pending.reply.get()))
      throw std::runtime_error("invalid task service response");
    protocol::validate_message(result);
    if (result.version() != 1 || result.service_id() != service ||
        result.correlation_id() != request.correlation_id())
      throw std::runtime_error("task response identity mismatch");
    log_rpc_result("worker", request, result, request.has_progress(),
                   {{"service_id", service}, {"task_id", task}});
    if (result.has_error())
      throw_remote_error(result.error().code(), result.error().message());
    if (request.has_claim() ? !result.has_attempt() : !result.has_task())
      throw std::runtime_error("unexpected task response");
    return result;
  };
  auto tick = [&] {
    if (owner && !owner->alive())
      std::_Exit(4); // A stuck trusted runner cannot be safely destroyed in-process.
    client->poll(10ms);
  };
  auto ready = [](const auto& future) { return future.wait_for(0ms) == std::future_status::ready; };
  auto call = [&](wire::TaskRequest request) {
    auto pending = begin(std::move(request));
    while (!ready(pending.reply))
      tick();
    return response(std::move(pending));
  };
  wire::TaskRequest claim;
  claim.mutable_claim()->set_id(task);
  claim.mutable_claim()->set_kind(kind);
  auto claimed = call(std::move(claim));
  auto attempt = std::move(*claimed.mutable_attempt());
  if (attempt.task().id() != task || attempt.task().state() != wire::RUNNING ||
      attempt.token().empty() || attempt.task().kind() != kind)
    throw std::runtime_error("invalid claimed task");
  try {
    log_process_event("worker", LogLevel::info, "task.claimed",
                      {{"trace_id", worker_trace},
                       {"service_id", service},
                       {"task_id", task},
                       {"attempt", attempt.task().attempt()}});
  } catch (...) {
    log_process_failure("worker", "trace.failed", ErrorCode::internal_error, 1);
  }
  const auto token = attempt.token();
  std::stop_source cancel;
  std::atomic<unsigned> completed{0};
  std::packaged_task<wire::TaskFinish()> execute([&, attempt = std::move(attempt)]() mutable {
    TraceScope execution_context(worker_trace);
    resolve_task_input(attempt, cancel.get_token());
    return runner(attempt, cancel.get_token(), [&](auto done, auto total) {
      if (done > total || done > std::numeric_limits<unsigned>::max() || done < completed.load())
        throw std::invalid_argument("invalid task progress");
      completed = static_cast<unsigned>(done);
    });
  });
  auto result = execute.get_future();
  std::jthread execution(std::move(execute));
  bool cancellation_requested = false, heartbeat_ended = false;
  std::exception_ptr heartbeat_error;
  std::optional<Pending> heartbeat;
  auto next_heartbeat = std::chrono::steady_clock::now();
  auto progress = [&] {
    tick();
    if (heartbeat_ended)
      return;
    try {
      if (heartbeat && ready(heartbeat->reply)) {
        const auto state = response(std::move(*heartbeat)).task().state();
        heartbeat.reset();
        if (state == wire::CANCEL_REQUESTED) {
          cancellation_requested = true;
          cancel.request_stop();
        } else if (state == wire::SUCCEEDED || state == wire::CANCELLED ||
                   state == wire::PUBLISHING) {
          // The service may commit the finish while its response is still in flight.
          heartbeat_ended = true;
          return;
        } else if (state != wire::RUNNING)
          throw std::runtime_error("task attempt is no longer active");
        next_heartbeat = std::chrono::steady_clock::now() + 250ms;
      }
      if (!heartbeat && std::chrono::steady_clock::now() >= next_heartbeat) {
        wire::TaskRequest request;
        auto* p = request.mutable_progress();
        p->set_id(task);
        p->set_token(token);
        p->set_completed(completed.load());
        heartbeat.emplace(begin(std::move(request)));
      }
    } catch (...) {
      heartbeat_error = std::current_exception();
      heartbeat_ended = true;
      cancel.request_stop();
    }
  };
  try {
    while (!ready(result))
      progress();
    if (heartbeat_error)
      std::rethrow_exception(heartbeat_error);
    wire::TaskRequest finish;
    auto* f = finish.mutable_finish();
    *f = result.get();
    execution.join();
    f->set_id(task);
    f->set_token(token);
    auto pending = begin(std::move(finish));
    while (!ready(pending.reply))
      progress(); // Verification must not suspend lease renewal or parent supervision.
    const auto state = response(std::move(pending)).task().state();
    if (state == wire::CANCELLED)
      return 2;
    if (state == wire::PUBLISHING) {
      std::cout << "Accepted download for publication " << task << '\n';
      return 0;
    }
    if (state != wire::SUCCEEDED)
      throw std::runtime_error("task completion was not confirmed");
    std::cout << "Completed task " << task << '\n';
    return 0;
  } catch (const std::exception& error) {
    cancel.request_stop();
    // Keep supervision running until the execution owner has relinquished its
    // state. A nonreturning plugin is terminated with the process by its Agent.
    if (result.valid())
      while (!ready(result))
        tick();
    if (execution.joinable())
      execution.join();
    // A lost finish response may already have committed. The attempt token fences
    // this one failure report; neither the finish nor the algorithm is retried.
    try {
      wire::TaskRequest report;
      if (cancellation_requested) {
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
      static_cast<void>(call(std::move(report)));
    } catch (const std::exception&) {
    }
    if (cancellation_requested)
      return 2;
    throw;
  }
}
} // namespace asterion::protocol
