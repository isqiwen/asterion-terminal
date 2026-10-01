#include "history_coverage.hpp"
#include <asterion/kernel/logger.hpp>
#include <asterion/kernel/native_plugin.hpp>
#include "task_store.hpp"
#include "history_minutes.hpp"
#include "history_daily.hpp"
#include "history_providers.hpp"
#include "history_archive.hpp"
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
#include <stdexcept>
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
  std::string plugin_directory;
  app.add_option("--plugin-directory", plugin_directory)->check(CLI::ExistingDirectory);
  CLI11_PARSE(app, argc, argv);
  if (argc == 1) {
    std::cerr << "asterion-task-service: supply --session, --directory and a "
                 "private endpoint or TLS listener.\n";
    return 3;
  }
  try {
    if (!plugin_directory.empty())
      asterion::configure_native_plugins(plugin_directory);
    asterion::validate_id(service);
    if (directory.empty())
      throw std::invalid_argument("a task directory is required");
    transport.validate();
    (void)asterion::history_providers::sources();
    asterion::tasks::Store store(std::filesystem::absolute(
        std::filesystem::path(std::u8string(directory.begin(), directory.end()))));
    asterion::history_files::Archive archive(std::filesystem::absolute(directory) / "history");
    const auto started = std::chrono::steady_clock::now();
    const auto instance = asterion::unique_process_id();
    std::mutex mutex;
    bool quiescing = false;
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
    auto respond = [&](const std::string& frame, bool health_only, bool worker,
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
        if (request.has_quiesce()) {
          if (!health_only)
            throw asterion::Error(asterion::ErrorCode::permission_denied,
                                  "upgrade control requires the private health channel");
          std::lock_guard lock(mutex);
          quiescing = true;
          if (request.quiesce().stop()) {
            const auto tasks = store.list();
            for (const auto& task : tasks.tasks())
              if (task.state() == wire::RUNNING || task.state() == wire::CANCEL_REQUESTED)
                throw asterion::Error(asterion::ErrorCode::unavailable,
                                      "upgrade is waiting for running research tasks");
            asterion::service::request_stop();
          }
          response.mutable_health()->set_instance_id(instance);
          return response;
        }
        if (health_only)
          throw std::invalid_argument("health channel only accepts heartbeat");
        if (request.has_dispatch() && !worker)
          throw asterion::Error(asterion::ErrorCode::permission_denied,
                                "task dispatch requires the private worker channel");
        std::unique_lock lock(mutex);
        admitted();
        expire_leases();
        admitted();
        if (quiescing && (request.has_submit() || request.has_retry() || request.has_dispatch() ||
                          (!worker && request.has_claim())))
          throw asterion::Error(asterion::ErrorCode::unavailable,
                                "task service is preparing for upgrade");
        if (request.has_history_datasets()) {
          const auto& q = request.history_datasets();
          lock.unlock();
          const asterion::HistoryStorePort& repository = archive;
          for (const auto& item :
               repository.datasets({q.venue(), q.product(), q.contract_id(), q.source()})) {
            auto* out = response.mutable_history_datasets()->add_items();
            out->set_id(item.id);
            out->set_contract_id(item.contract.key());
            out->set_source(item.source);
            out->set_revision(item.revision);
            out->set_begin(item.begin);
            out->set_end(item.end);
            out->set_interval_minutes(item.interval_minutes);
            out->set_rows(item.rows);
          }
          response.mutable_history_datasets();
        } else if (request.has_history_coverage()) {
          const auto filter = request.history_coverage();
          lock.unlock();
          *response.mutable_history_coverage() = asterion::tasks::history_coverage(archive, filter);
        } else if (request.has_verify_connection()) {
          const auto& query = request.verify_connection();
          lock.unlock();
          auto* result = response.mutable_connection_verification();
          for (const auto& check : asterion::history_providers::verify_connection(
                   query.source(), query.credential(), stop)) {
            auto* out = result->add_checks();
            out->set_scope(check.scope);
            out->set_state(check.state);
          }
        } else if (request.has_history_catalog()) {
          const auto& query = request.history_catalog();
          lock.unlock();
          auto* catalog = response.mutable_history_catalog();
          catalog->set_source(query.source());
          for (const auto& item : asterion::history_providers::catalog(
                   query.source(), query.credential(), query.venue(), query.product(), stop)) {
            auto* row = catalog->add_items();
            row->set_contract_id(item.identity.key());
            row->set_source_instrument(item.source_instrument);
            row->set_name(item.name);
            row->set_list_date(item.list_date);
            row->set_delist_date(item.delist_date);
            if (item.multiplier)
              row->mutable_multiplier()->set_units(item.multiplier->raw());
            if (item.per_unit)
              row->mutable_per_unit()->set_units(item.per_unit->raw());
            if (item.trade_unit)
              row->set_trade_unit(*item.trade_unit);
            if (item.quote_unit)
              row->set_quote_unit(*item.quote_unit);
          }
        } else if (request.has_daily_page()) {
          const auto& query = request.daily_page();
          if (!query.dataset_id().empty() && !query.task_id().empty())
            throw std::invalid_argument("choose dataset or task query");
          const auto record = query.dataset_id().empty() ? asterion::data::v1::HistoryRecord{}
                                                         : archive.get(query.dataset_id());
          const auto input =
              query.dataset_id().empty() ? store.get(query.task_id()).daily() : record.daily();
          const auto result = query.dataset_id().empty() ? store.daily_result(query.task_id())
                                                         : record.daily_result();
          lock.unlock();
          *response.mutable_daily_page() =
              asterion::history_files::read_daily_page(input, result, query);
        } else if (request.has_minute_page()) {
          const auto& query = request.minute_page();
          if (!query.dataset_id().empty() && !query.task_id().empty())
            throw std::invalid_argument("choose dataset or task query");
          const auto record = query.dataset_id().empty() ? asterion::data::v1::HistoryRecord{}
                                                         : archive.get(query.dataset_id());
          const auto input =
              query.dataset_id().empty() ? store.get(query.task_id()).minutes() : record.minutes();
          const auto result = query.dataset_id().empty() ? store.minute_result(query.task_id())
                                                         : record.minute_result();
          lock.unlock(); // Dataset files are read outside the shared task-state lock.
          *response.mutable_minute_page() =
              asterion::history_files::read_minute_page(input, result, query);
        } else if (request.has_bar_dataset()) {
          const auto sources = store.prepare_dataset(request.bar_dataset());
          lock.unlock();
          *response.mutable_bar_dataset() = asterion::tasks::resolve_bar_dataset(sources);
          lock.lock();
          store.confirm_sources(sources);
        } else if (request.has_dispatch())
          *response.mutable_launches() = store.dispatch(request.dispatch());
        else if (request.has_submit()) {
          const auto& p = request.submit();
          if (!p.provider_token().empty() && !p.has_minutes() && !p.has_daily())
            throw std::invalid_argument("credential requires download task");
          if (p.has_daily_factor()) {
            auto submission = store.prepare_daily_factor(p.id(), p.daily_factor());
            lock.unlock();
            submission.verify();
            lock.lock();
            admitted();
            if (quiescing)
              throw asterion::Error(asterion::ErrorCode::unavailable,
                                    "task service is preparing for upgrade");
            *response.mutable_task() = store.submit(std::move(submission));
          } else if (p.has_backtest()) {
            // Clients select downloads; the service resolves and freezes each
            // contract's bars.
            const auto& b = p.backtest();
            if (b.contracts().empty() || b.contracts_size() > 20)
              throw std::invalid_argument("backtest requires 1 to 20 contracts");
            std::vector<asterion::tasks::BarDatasetSources> sources;
            for (const auto& contract : b.contracts())
              sources.push_back(store.prepare_dataset(contract.data()));
            lock.unlock();
            std::vector<asterion::data::v1::BarDataset> datasets;
            for (const auto& source : sources)
              datasets.push_back(asterion::tasks::resolve_bar_dataset(source));
            lock.lock();
            admitted();
            if (quiescing)
              throw asterion::Error(asterion::ErrorCode::unavailable,
                                    "task service is preparing for upgrade");
            for (const auto& source : sources)
              store.confirm_sources(source);
            wire::BacktestInput input;
            input.set_version(8);
            auto* paper = input.mutable_paper();
            *paper->mutable_deposit() = b.deposit();
            *paper->mutable_risk() = b.risk();
            for (int c = 0; c < b.contracts_size(); ++c) {
              auto* contract = paper->add_contracts();
              *contract->mutable_dataset() = std::move(datasets[static_cast<std::size_t>(c)]);
              *contract->mutable_cost_schedule() = b.contracts(c).cost_schedule();
            }
            input.set_dataset_revision(asterion::protocol::dataset_revision(*paper));
            *input.mutable_sma() = b.sma();
            *response.mutable_task() = store.submit(p.id(), input);
          } else if (p.has_factor_request()) {
            const auto sources = store.prepare_dataset(p.factor_request().data());
            lock.unlock();
            auto dataset = asterion::tasks::resolve_bar_dataset(sources);
            lock.lock();
            admitted();
            if (quiescing)
              throw asterion::Error(asterion::ErrorCode::unavailable,
                                    "task service is preparing for upgrade");
            store.confirm_sources(sources);
            const auto& f = p.factor_request();
            wire::FactorInput input;
            input.set_version(5);
            input.set_dataset_revision(dataset.revision());
            *input.mutable_dataset() = std::move(dataset);
            *input.mutable_lookbacks() = f.lookbacks();
            input.set_horizon(f.horizon());
            if (f.has_full_sample())
              input.set_full_sample(f.full_sample());
            else if (f.has_walk_forward())
              *input.mutable_walk_forward() = f.walk_forward();
            else if (f.has_holdout_start())
              input.set_holdout_start(f.holdout_start());
            *response.mutable_task() = store.submit(p.id(), input);
          } else if (p.has_daily())
            *response.mutable_task() = store.submit(p.id(), p.daily(), p.provider_token());
          else if (p.has_minutes())
            *response.mutable_task() = store.submit(p.id(), p.minutes(), p.provider_token());
          else
            throw std::invalid_argument("missing task input");
        } else if (request.has_get())
          *response.mutable_task() = store.get(request.get().id());
        else if (request.has_list()) {
          *response.mutable_tasks() = store.list();
          for (const auto& source : asterion::history_providers::sources()) {
            auto* out = response.mutable_tasks()->add_sources();
            out->set_id(source.id);
            out->set_name(source.name);
            out->set_plugin_id(source.plugin_id);
            out->set_normalization(source.semantics.normalization);
            out->set_timezone(source.semantics.timezone);
            out->set_timestamp_semantics(source.semantics.timestamp_semantics);
            for (const auto& venue : source.venues)
              out->add_venues(venue);
            for (auto interval : source.intervals)
              out->add_intervals(interval);
            out->set_max_requests_per_minute(source.max_requests_per_minute);
            out->set_credential_required(source.credential_required);
            if (source.connection) {
              auto* schema = out->mutable_connection();
              schema->set_credential_label_en(source.connection->credential_label_en);
              schema->set_credential_label_zh(source.connection->credential_label_zh);
              schema->set_credential_required(source.connection->credential_required);
              schema->set_remember_allowed(source.connection->remember_allowed);
              schema->set_credential_max_length(source.connection->credential_max_length);
              schema->set_requests_per_minute_default(
                  source.connection->requests_per_minute_default);
              schema->set_requests_per_minute_max(source.connection->requests_per_minute_max);
            }
          }
        } else if (request.has_cancel())
          *response.mutable_task() = store.cancel(request.cancel().id());
        else if (request.has_retry())
          *response.mutable_task() = store.retry(request.retry().id());
        else if (request.has_result()) {
          const auto& id = request.result().id();
          if (store.get(id).kind() == wire::DAILY_FACTOR)
            *response.mutable_daily_factor() = store.daily_factor_result(id);
          else if (store.get(id).kind() == wire::DAILY_DOWNLOAD)
            *response.mutable_daily() = store.daily_result(id);
          else if (store.get(id).kind() == wire::MINUTE_DOWNLOAD)
            *response.mutable_minutes() = store.minute_result(id);
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
          try {
            store.download_attempt(*attempt);
          } catch (const std::exception& error) {
            store.fail(request.claim().id(), attempt->token(), error.what());
            leases.erase(request.claim().id());
            throw;
          }
        } else if (request.has_progress()) {
          const auto& p = request.progress();
          store.progress(p.id(), p.token(), p.completed());
          leases.at(p.id()).expires =
              std::chrono::steady_clock::now() + std::chrono::seconds(worker_timeout);
          *response.mutable_task() = store.get(p.id());
        } else if (request.has_finish()) {
          const auto& p = request.finish();
          auto completion = store.prepare_finish(p);
          lock.unlock();
          completion.verify();
          lock.lock();
          admitted();
          expire_leases();
          store.finish(std::move(completion));
          leases.erase(p.id());
          *response.mutable_task() = store.get(p.id());
        } else if (request.has_fail()) {
          const auto& p = request.fail();
          if (!asterion::parse_error_code(p.error_code()))
            throw std::invalid_argument("invalid task failure error code");
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
    auto handle = [&](asterion::service::Connection& channel, std::stop_token stop, bool worker) {
      const auto deadline = channel.accepted_at() + 10s;
      const auto left = deadline - std::chrono::steady_clock::now();
      if (left <= std::chrono::steady_clock::duration::zero())
        throw asterion::Error(asterion::ErrorCode::unavailable, "task request admission timed out");
      const auto frame = channel.receive(std::chrono::ceil<std::chrono::milliseconds>(left));
      channel.send(respond(frame, false, worker, deadline, stop).SerializeAsString(), 3s);
    };
    asterion::service::HealthChannel health(health_endpoint, [&](const std::string& frame) {
      return respond(frame, true, false, std::chrono::steady_clock::now() + 1s, std::stop_token{})
          .SerializeAsString();
    });
    // Private worker progress has its own host and capacity; public slow peers
    // cannot consume the slots needed to complete/cancel running work.
    std::unique_ptr<asterion::service::ServiceHost> worker_host;
    std::jthread worker_server;
    if (!worker_endpoint.empty()) {
      asterion::service::HostOptions worker_options;
      worker_options.drain = 10s;
      worker_options.workers = 4;
      worker_host = std::make_unique<asterion::service::ServiceHost>(
          asterion::service::Transport{worker_endpoint, {}, 0, {}},
          [&](auto& channel, auto stop) { handle(channel, stop, true); }, worker_options);
      worker_server = std::jthread([&] {
        if (!worker_host->run())
          std::_Exit(0);
      });
    }
    // Lease expiry runs on the public accept thread between polls; task state
    // remains serialized by the mutex.
    asterion::service::HostOptions options;
    options.drain = 10s;
    options.handshake = 3s;
    options.tick = [&] {
      std::lock_guard lock(mutex);
      expire_leases();
    };
    asterion::service::ServiceHost host(
        transport, [&](auto& channel, auto stop) { handle(channel, stop, false); }, options);
    if (!host.run())
      std::_Exit(0);
    worker_server = {};
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Task service failed: " << error.what() << '\n';
    asterion::log_process_event("task-service", asterion::LogLevel::error, "service.failed",
                                {{"message", error.what()}});
    return 1;
  }
}
