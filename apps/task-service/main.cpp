#include "task_store.hpp"
#include <CLI/CLI.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/service_host.hpp>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>
using namespace std::chrono_literals;
int main(int argc, char** argv) {
  CLI::App app{"Asterion durable task service (Agent-dispatched backtest workers)"};
  app.set_version_flag("--version", "asterion-task-service " ASTERION_PRODUCT_VERSION);
  std::string directory, service, health_endpoint, worker_endpoint;
  unsigned worker_timeout = 30;
  app.add_option("--worker-timeout", worker_timeout,
                 "Seconds without worker progress before interruption")
      ->check(CLI::Range(1, 300));
  std::uint64_t owner_pid = 0;
  asterion::service::Transport transport;
  app.add_option("--directory", directory)->check(CLI::ExistingDirectory);
  app.add_option("--endpoint", transport.endpoint);
  app.add_option("--session", service);
  app.add_option("--bind", transport.bind);
  app.add_option("--port", transport.port)->check(CLI::Range(1, 65535));
  app.add_option("--tls-ca", transport.tls.ca_file);
  app.add_option("--tls-cert", transport.tls.certificate_file);
  app.add_option("--tls-key", transport.tls.private_key_file);
  app.add_option("--worker-endpoint", worker_endpoint, "Private same-machine worker IPC");
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
    if (directory.empty())
      throw std::invalid_argument("a task directory is required");
    transport.validate();
    asterion::tasks::Store store(std::filesystem::absolute(
        std::filesystem::path(std::u8string(directory.begin(), directory.end()))));
    const auto started = std::chrono::steady_clock::now();
    const auto instance = asterion::unique_process_id();
    std::mutex mutex;
    std::atomic<bool> degraded{false};
    asterion::service::install_stop_signals();
    asterion::service::OwnerWatch owner(owner_pid);
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
    auto respond = [&](const std::string& frame, bool health_only,
                       std::chrono::steady_clock::time_point deadline, std::stop_token stop) {
      wire::TaskRequest request;
      wire::TaskResponse response;
      response.set_version(1);
      response.set_service_id(service);
      try {
        auto admitted = [&] {
          if (stop.stop_requested() || std::chrono::steady_clock::now() >= deadline)
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
          throw std::invalid_argument("task protocol version or service mismatch");
        if (request.has_heartbeat()) {
          auto* health = response.mutable_health();
          health->set_instance_id(instance);
          health->set_version(ASTERION_PRODUCT_VERSION);
          health->set_recovery_required(degraded);
          health->set_uptime_ms(
              static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
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
          const auto& p = request.submit();
          if (p.has_calendar())
            *response.mutable_task() = store.submit(p.id(), p.calendar());
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
          const auto& id = request.result().id();
          if (store.get(id).kind() == wire::CALENDAR_IMPORT)
            *response.mutable_calendar_publication() = store.calendar_publication(id);
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
          auto* attempt = response.mutable_attempt();
          attempt->set_token(store.claim(request.claim().id()));
          leases[request.claim().id()] = {attempt->token(),
                                          std::chrono::steady_clock::now() +
                                              std::chrono::seconds(worker_timeout)};
          *attempt->mutable_task() = store.get(request.claim().id());
        } else if (request.has_progress()) {
          const auto& p = request.progress();
          store.progress(p.id(), p.token(), p.completed());
          leases.at(p.id()).expires =
              std::chrono::steady_clock::now() + std::chrono::seconds(worker_timeout);
          *response.mutable_task() = store.get(p.id());
        } else if (request.has_finish()) {
          const auto& p = request.finish();
          if (p.has_calendar_publication())
            store.finish(p.id(), p.token(), p.calendar_publication());
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
          const auto& p = request.fail();
          store.fail(p.id(), p.token(), p.error());
          leases.erase(p.id());
          *response.mutable_task() = store.get(p.id());
        } else if (request.has_cancel_ack()) {
          const auto& p = request.cancel_ack();
          store.acknowledge_cancel(p.id(), p.token());
          leases.erase(p.id());
          *response.mutable_task() = store.get(p.id());
        } else
          throw std::invalid_argument("missing task operation");
        if (response.has_task() && !request.has_get())
          response.mutable_task()->clear_input();
      } catch (const asterion::Error& error) {
        response.mutable_error()->set_code(std::string(asterion::error_name(error.code())));
        response.mutable_error()->set_message(error.what());
      } catch (const std::invalid_argument& error) {
        response.mutable_error()->set_code("invalid_request");
        response.mutable_error()->set_message(error.what());
      } catch (const std::out_of_range&) {
        response.mutable_error()->set_code("not_found");
        response.mutable_error()->set_message("unknown task");
      } catch (const std::exception& error) {
        degraded = true;
        response.mutable_error()->set_code("recovery_required");
        response.mutable_error()->set_message(error.what());
      }
      return response;
    };
    // One request per connection. The whole admission (queueing, TLS and the
    // request read) is bounded from the moment the transport was accepted.
    auto handle = [&](asterion::service::Connection& channel, std::stop_token stop) {
      const auto deadline = channel.accepted_at() + 10s;
      const auto left = deadline - std::chrono::steady_clock::now();
      if (left <= std::chrono::steady_clock::duration::zero())
        throw asterion::Error(asterion::ErrorCode::unavailable, "task request admission timed out");
      const auto frame = channel.receive(std::chrono::ceil<std::chrono::milliseconds>(left));
      channel.send(respond(frame, false, deadline, stop).SerializeAsString(), 3s);
    };
    asterion::service::HealthChannel health(health_endpoint, [&](const std::string& frame) {
      return respond(frame, true, std::chrono::steady_clock::now() + 1s, std::stop_token{})
          .SerializeAsString();
    });
    // Private worker progress has its own host and capacity; public slow peers
    // cannot consume the slots needed to complete/cancel running work.
    std::unique_ptr<asterion::service::ServiceHost> worker_host;
    std::jthread worker_server;
    if (!worker_endpoint.empty()) {
      asterion::service::HostOptions worker_options;
      worker_options.workers = 4;
      worker_host = std::make_unique<asterion::service::ServiceHost>(
          asterion::service::Transport{worker_endpoint, {}, 0, {}}, handle, worker_options);
      worker_server = std::jthread([&] {
        if (!worker_host->run())
          std::_Exit(0);
      });
    }
    // Lease expiry runs on the public accept thread between polls; task state
    // remains serialized by the mutex.
    asterion::service::HostOptions options;
    options.handshake = 3s;
    options.tick = [&] {
      std::lock_guard lock(mutex);
      expire_leases();
    };
    asterion::service::ServiceHost host(transport, handle, options);
    if (!host.run())
      std::_Exit(0);
    worker_server = {};
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Task service failed: " << error.what() << '\n';
    return 1;
  }
}
