#include "task_store.hpp"
#include <CLI/CLI.hpp>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/ipc/tls_channel.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/owner.hpp>
#include <asterion/kernel/thread_pool.hpp>
#include <algorithm>
#include <atomic>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>
using namespace std::chrono_literals;
int main(int argc, char **argv) {
  CLI::App app{
      "Asterion durable task service (Agent-dispatched backtest workers)"};
  app.set_version_flag("--version",
                       "asterion-task-service " ASTERION_PRODUCT_VERSION);
  std::string directory, endpoint, service, bind, health_endpoint,
      worker_endpoint;
  unsigned worker_timeout = 30;
  app.add_option("--worker-timeout", worker_timeout,
                 "Seconds without worker progress before interruption")
      ->check(CLI::Range(1, 300));
  unsigned short port = 0;
  std::uint64_t owner_pid = 0;
  asterion::ipc::TlsIdentity tls;
  app.add_option("--directory", directory)->check(CLI::ExistingDirectory);
  app.add_option("--endpoint", endpoint);
  app.add_option("--session", service);
  app.add_option("--bind", bind);
  app.add_option("--port", port)->check(CLI::Range(1, 65535));
  app.add_option("--tls-ca", tls.ca_file);
  app.add_option("--tls-cert", tls.certificate_file);
  app.add_option("--tls-key", tls.private_key_file);
  app.add_option("--worker-endpoint", worker_endpoint,
                 "Private same-machine worker IPC");
  app.add_option("--health-endpoint", health_endpoint);
  app.add_option("--owner-pid", owner_pid);
  argv = app.ensure_utf8(argv);
  CLI11_PARSE(app, argc, argv);
  if (argc == 1) {
    std::cerr << "asterion-task-service: supply --session, --directory and a "
                 "private endpoint or TLS listener.\n";
    return 3;
  }
  try {
    asterion::validate_id(service);
    const bool remote = !bind.empty();
    if (directory.empty() ||
        (remote ? (!endpoint.empty() || !port || tls.ca_file.empty() ||
                   tls.certificate_file.empty() || tls.private_key_file.empty())
                : (endpoint.empty() || port || !tls.ca_file.empty() ||
                   !tls.certificate_file.empty() ||
                   !tls.private_key_file.empty())))
      throw std::invalid_argument("choose local endpoint OR TCP bind/port with "
                                  "all TLS files and a task directory");
    asterion::tasks::Store store(
        std::filesystem::absolute(std::filesystem::path(
            std::u8string(directory.begin(), directory.end()))));
    const auto started = std::chrono::steady_clock::now();
    const auto instance = asterion::unique_process_id();
    std::mutex mutex;
    std::atomic<bool> degraded{false};
    std::unique_ptr<asterion::ProcessOwner> owner;
    if (owner_pid)
      owner = std::make_unique<asterion::ProcessOwner>(owner_pid);
    namespace wire = asterion::research::v1;
    struct Lease {
      std::string token;
      std::chrono::steady_clock::time_point expires;
    };
    std::map<std::string, Lease> leases;
    // Called only while holding the task-state mutex. Expiration is checked
    // before worker reports as well as from the accept loop, so a late report
    // cannot revive a lease merely because maintenance was delayed.
    auto expire_leases = [&] {
      for (auto it = leases.begin(); it != leases.end();) {
        if (std::chrono::steady_clock::now() >= it->second.expires) {
          store.interrupt(it->first, it->second.token,
                          "worker heartbeat expired; explicit retry required");
          it = leases.erase(it);
        } else
          ++it;
      }
    };
    auto respond = [&](const std::string &frame, bool health_only,
                       std::chrono::steady_clock::time_point deadline,
                       std::stop_token stop) {
      wire::TaskRequest request;
      wire::TaskResponse response;
      response.set_version(1);
      response.set_service_id(service);
      try {
        auto admitted = [&] {
          if (stop.stop_requested() ||
              std::chrono::steady_clock::now() >= deadline)
            throw asterion::Error(asterion::ErrorCode::unavailable,
                                  "task request admission timed out");
        };
        admitted();
        if (!request.ParseFromString(frame))
          throw std::invalid_argument("invalid task Protobuf");
        asterion::protocol::validate_message(request);
        response.set_correlation_id(request.correlation_id());
        asterion::validate_id(request.correlation_id());
        if (request.version() != 1 || request.service_id() != service)
          throw std::invalid_argument(
              "task protocol version or service mismatch");
        if (request.has_heartbeat()) {
          auto *health = response.mutable_health();
          health->set_instance_id(instance);
          health->set_version(ASTERION_PRODUCT_VERSION);
          health->set_recovery_required(degraded);
          health->set_uptime_ms(static_cast<std::uint64_t>(
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - started)
                  .count()));
          return response;
        }
        if (health_only)
          throw std::invalid_argument("health channel only accepts heartbeat");
        std::lock_guard lock(mutex);
        admitted();
        expire_leases();
        admitted();
        if (request.has_submit()) {
          const auto &p = request.submit();
          if(p.has_calendar()) *response.mutable_task()=store.submit(p.id(),p.calendar());
          else if (p.has_data())
            *response.mutable_task() = store.submit(p.id(), p.data());
          else if (p.has_factor())
            *response.mutable_task() = store.submit(p.id(), p.factor());
          else if (p.has_input())
            *response.mutable_task() = store.submit(p.id(), p.input());
          else
            throw std::invalid_argument("missing task input");
        } else if (request.has_get())
          *response.mutable_task() = store.get(request.get().id());
        else if (request.has_list())
          *response.mutable_tasks() = store.list();
        else if (request.has_cancel())
          *response.mutable_task() = store.cancel(request.cancel().id());
        else if (request.has_retry())
          *response.mutable_task() = store.retry(request.retry().id());
        else if (request.has_result()) {
          const auto &id = request.result().id();
          if(store.get(id).kind()==wire::CALENDAR_IMPORT) *response.mutable_calendar_publication()=store.calendar_publication(id);
          else if (store.get(id).kind() == wire::DATA_IMPORT)
            *response.mutable_publication() = store.publication(id);
          else if (store.get(id).kind() == wire::FACTOR)
            *response.mutable_factor() = store.factor_result(id);
          else
            *response.mutable_backtest() = store.result(id);
          *response.mutable_result_task() = store.get(id);
        } else if (request.has_claim()) {
          if (store.get(request.claim().id()).kind() != request.claim().kind())
            throw std::invalid_argument("worker kind does not match task");
          auto *attempt = response.mutable_attempt();
          attempt->set_token(store.claim(request.claim().id()));
          leases[request.claim().id()] = {
              attempt->token(), std::chrono::steady_clock::now() +
                                    std::chrono::seconds(worker_timeout)};
          *attempt->mutable_task() = store.get(request.claim().id());
        } else if (request.has_progress()) {
          const auto &p = request.progress();
          store.progress(p.id(), p.token(), p.completed());
          leases.at(p.id()).expires = std::chrono::steady_clock::now() +
                                      std::chrono::seconds(worker_timeout);
          *response.mutable_task() = store.get(p.id());
        } else if (request.has_finish()) {
          const auto &p = request.finish();
          if(p.has_calendar_publication()) store.finish(p.id(),p.token(),p.calendar_publication());
          else if (p.has_publication())
            store.finish(p.id(), p.token(), p.publication());
          else if (p.has_factor())
            store.finish(p.id(), p.token(), p.factor());
          else if (p.has_result())
            store.finish(p.id(), p.token(), p.result());
          else
            throw std::invalid_argument("missing task result");
          leases.erase(p.id());
          *response.mutable_task() = store.get(p.id());
        } else if (request.has_fail()) {
          const auto &p = request.fail();
          store.fail(p.id(), p.token(), p.error());
          leases.erase(p.id());
          *response.mutable_task() = store.get(p.id());
        } else if (request.has_cancel_ack()) {
          const auto &p = request.cancel_ack();
          store.acknowledge_cancel(p.id(), p.token());
          leases.erase(p.id());
          *response.mutable_task() = store.get(p.id());
        } else
          throw std::invalid_argument("missing task operation");
        if (response.has_task() && !request.has_get())
          response.mutable_task()->clear_input();
      } catch (const asterion::Error &error) {
        response.mutable_error()->set_code(
            std::string(asterion::error_name(error.code())));
        response.mutable_error()->set_message(error.what());
      } catch (const std::invalid_argument &error) {
        response.mutable_error()->set_code("invalid_request");
        response.mutable_error()->set_message(error.what());
      } catch (const std::out_of_range &) {
        response.mutable_error()->set_code("not_found");
        response.mutable_error()->set_message("unknown task");
      } catch (const std::exception &error) {
        degraded = true;
        response.mutable_error()->set_code("recovery_required");
        response.mutable_error()->set_message(error.what());
      }
      return response;
    };
    auto dispatch = [&](asterion::ThreadPool &pool, auto pending) {
      const auto deadline = std::chrono::steady_clock::now() + 10s;
      auto peer = std::make_shared<decltype(pending)>(std::move(pending));
      static_cast<void>(pool.submit([&, peer, deadline](std::stop_token stop) {
        try {
          auto remaining = [&] {
            const auto left = deadline - std::chrono::steady_clock::now();
            if (stop.stop_requested() ||
                left <= std::chrono::steady_clock::duration::zero())
              throw asterion::Error(asterion::ErrorCode::unavailable,
                                    "task request admission timed out");
            return std::chrono::ceil<std::chrono::milliseconds>(left);
          };
          auto channel = [&] {
            if constexpr (requires { std::move(*peer).handshake(3s); })
              return std::move(*peer).handshake(
                  std::min(remaining(), std::chrono::milliseconds{3000}));
            else
              return std::move(*peer);
          }();
          const auto frame = channel.receive(remaining());
          channel.send(
              respond(frame, false, deadline, stop).SerializeAsString(), 3s);
        } catch (const std::exception &) {
          // Close only this peer. Never replay a query or mutation.
        }
      }));
    };
    std::unique_ptr<asterion::ipc::Listener> health;
    if (!health_endpoint.empty())
      health = std::make_unique<asterion::ipc::Listener>(health_endpoint);
    std::jthread supervisor([&](std::stop_token stop) {
      while (!stop.stop_requested()) {
        if (owner && !owner->alive())
          std::_Exit(4);
        if (!health) {
          std::this_thread::sleep_for(100ms);
          continue;
        }
        try {
          auto channel = health->accept(100ms);
          const auto deadline = std::chrono::steady_clock::now() + 1s;
          channel.send(respond(channel.receive(1s), true, deadline, stop)
                           .SerializeAsString(),
                       1s);
        } catch (const std::exception &) {
        }
      }
    });
    std::unique_ptr<asterion::ipc::Listener> workers;
    if (!worker_endpoint.empty())
      workers = std::make_unique<asterion::ipc::Listener>(worker_endpoint);
    // Private worker progress has its own capacity; public slow peers cannot
    // consume the slots needed to complete/cancel already-running work.
    asterion::ThreadPool worker_clients(4, 8);
    std::jthread worker_server([&](std::stop_token stop) {
      while (workers && !stop.stop_requested()) {
        try {
          dispatch(worker_clients, workers->accept(200ms));
        } catch (const asterion::Error &) {
        }
      }
    });
    std::unique_ptr<asterion::ipc::Listener> local;
    std::unique_ptr<asterion::ipc::TlsListener> tcp;
    if (remote)
      tcp = std::make_unique<asterion::ipc::TlsListener>(bind, port, tls);
    else
      local = std::make_unique<asterion::ipc::Listener>(endpoint);
    // Listener ownership stays on this thread. Each pool joins before the
    // store/mutex/leases are destroyed. Task state remains serialized.
    asterion::ThreadPool clients(8, 8);
    for (;;) {
      try {
        if (remote)
          dispatch(clients, tcp->accept_pending(200ms));
        else
          dispatch(clients, local->accept(200ms));
      } catch (const asterion::Error &) {
        // Idle poll or bounded overload: destroy the unqueued connection.
      }
      std::lock_guard lock(mutex);
      expire_leases();
    }
  } catch (const std::exception &error) {
    std::cerr << "Task service failed: " << error.what() << '\n';
    return 1;
  }
}
