#include <asterion/kernel/logger.hpp>
#include <asterion/kernel/native_plugin.hpp>
#include "task_store.hpp"
#include <asterion/protocol/rpc_diagnostics.hpp>
#include <asterion/protocol/data_client.hpp>
#include <asterion/protocol/data.hpp>
#include <asterion/protocol/factor.hpp>
#include <asterion/kernel/polled_task.hpp>
#include <asterion/kernel/ipc/rpc_client.hpp>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/trace.hpp>
#include <asterion/kernel/thread_pool.hpp>
#include "verification_slots.hpp"
#include <CLI/CLI.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/rpc_host.hpp>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <map>

#include <stdexcept>
using namespace std::chrono_literals;
using asterion::PolledTask;
using asterion::PollFuture;
using asterion::PollUntil;
int main(int argc, char** argv) {
  CLI::App app{"Asterion durable task service (Agent-dispatched backtest workers)"};
  app.set_version_flag("--version", "asterion-task-service " ASTERION_PRODUCT_VERSION);
  std::string directory, service, health_endpoint, worker_endpoint, data_instance, data_endpoint;
  unsigned worker_timeout = 30;
  unsigned file_workers = 2;
  app.add_option("--file-workers", file_workers)->check(CLI::Range(1, 2));
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
  app.add_option("--data-instance", data_instance)->required();
  app.add_option("--data-endpoint", data_endpoint)->required();
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
    asterion::validate_id(data_instance);
    if (service == data_instance || data_endpoint.empty())
      throw std::invalid_argument(
          "task service requires a distinct data instance and private endpoint");
    if (directory.empty())
      throw std::invalid_argument("a task directory is required");
    transport.validate();
    const auto started = std::chrono::steady_clock::now();
    const auto process = asterion::unique_process_id();
    asterion::service::install_stop_signals();
    namespace wire = asterion::task::v1;
    std::unique_ptr<asterion::tasks::Store> storage;
    std::future<void> opening;
    asterion::tasks::VerificationSlots verification_slots;
    asterion::tasks::VerificationSlots input_slots;
    bool quiescing = false;
    bool degraded = false;
    bool initialized = false;
    asterion::ipc::RpcClient data(data_endpoint, 8, asterion::PayloadBudget{128 * 1024 * 1024});
    struct Lease {
      std::string token;
      std::chrono::steady_clock::time_point expires;
    };
    std::map<std::string, Lease> leases;
    std::atomic<bool> journal_failed{false};
    struct Writing {
      asterion::tasks::Store::Change change;
      std::future<void> result;
      std::exception_ptr error;
      bool done = false;
    };
    std::map<std::string, std::shared_ptr<Writing>> writing;
    constexpr std::size_t journal_capacity = 16;
    asterion::ThreadPool journal(1, journal_capacity);
    asterion::ThreadPool files(file_workers, 8);
    auto collect_writes = [&] {
      for (auto it = writing.begin(); it != writing.end();) {
        auto& work = *it->second;
        if (work.result.wait_for(0s) != std::future_status::ready) {
          ++it;
          continue;
        }
        try {
          work.result.get();
          storage->confirm(work.change);
          const auto state = work.change.task().state();
          if (state != wire::RUNNING && state != wire::CANCEL_REQUESTED)
            leases.erase(it->first);
        } catch (...) {
          work.error = std::current_exception();
          storage->persistence_failed();
          degraded = true;
        }
        work.done = true;
        it = writing.erase(it);
      }
    };
    // Active controls wait only for their own journal change. Cold database
    // access waits for the owned connection without blocking the I/O thread.
    auto wait_for_store = [&](std::string id = {}) -> PolledTask<> {
      co_await PollUntil{[&] {
        return degraded || (!writing.contains(id) && (storage->is_active(id) || writing.empty()));
      }};
      if (degraded)
        throw asterion::Error(asterion::ErrorCode::recovery_required,
                              "task store requires recovery");
    };
    auto write = [&](asterion::tasks::Store::Change change) {
      auto work = std::make_shared<Writing>(Writing{std::move(change), {}, {}, false});
      const auto id = work->change.task().id();
      if (!writing.emplace(id, work).second)
        throw std::logic_error("task already has a pending change");
      try {
        work->result = journal.submit([&, work](std::stop_token) {
          if (journal_failed)
            throw std::runtime_error("task store requires recovery");
          try {
            work->change.persist();
          } catch (...) {
            journal_failed = true;
            throw;
          }
        });
      } catch (...) {
        writing.erase(id);
        throw;
      }
      return work;
    };
    auto commit =
        [&](asterion::tasks::Store::Change change) -> PolledTask<asterion::tasks::Store::Change> {
      if (!change.needs_write())
        co_return storage->commit(std::move(change));
      auto work = write(std::move(change));
      co_await PollUntil{[&] { return work->done; }};
      if (work->error)
        std::rethrow_exception(work->error);
      co_return std::move(work->change);
    };
    auto file_work = [&](auto work) {
      return PollFuture{files.submit([work = std::move(work)](std::stop_token) { work(); })};
    };
    auto data_call = [&](asterion::data::v1::DataRequest request, std::chrono::milliseconds timeout,
                         std::string parent = {}) -> PolledTask<asterion::data::v1::DataResponse> {
      request.set_version(1);
      request.set_service_id(data_instance);
      {
        asterion::TraceScope trace(parent);
        request.set_correlation_id(asterion::next_correlation_id());
      }
      auto bytes = co_await PollFuture{data.request(request.SerializeAsString(), timeout)};
      asterion::data::v1::DataResponse response;
      co_await file_work(
          [&] { response = asterion::protocol::decode_data_response(request, *bytes); });
      co_return std::move(response);
    };
    bool publication_wake = true;
    auto publish = [&]() -> PolledTask<> {
      // Only original durable decisions are retried. No worker is rerun.
      const auto decisions = storage->pending_publications();
      for (const auto& decision : decisions) {
        try {
          asterion::data::v1::DataRequest request;
          *request.mutable_publish_download() = decision;
          const auto response = co_await data_call(std::move(request), 5s);
          co_await wait_for_store(decision.identity().task_id());
          co_await commit(storage->confirm_publication(response.published_download()));
        } catch (const asterion::Error& error) {
          if (error.code() != asterion::ErrorCode::unavailable &&
              error.code() != asterion::ErrorCode::resource_exhausted)
            throw;
        }
      }
    };
    std::optional<PolledTask<>> publication;
    auto next_publication = std::chrono::steady_clock::now();
    // The I/O owner observes lease expiry even while file, RPC or journal work waits.
    auto expire_leases = [&] {
      collect_writes();
      if (degraded)
        return;
      for (auto it = leases.begin(); it != leases.end();) {
        if (writing.size() >= journal_capacity)
          break;
        if (std::chrono::steady_clock::now() >= it->second.expires &&
            !writing.contains(it->first)) {
          write(storage->interrupt(it->first, it->second.token,
                                   "worker heartbeat expired; explicit retry required"));
          it = leases.erase(it);
        } else
          ++it;
      }
    };
    auto respond =
        [&](std::string frame, bool health_only, bool worker,
            std::chrono::steady_clock::time_point deadline) -> PolledTask<wire::TaskResponse> {
      wire::TaskRequest request;
      wire::TaskResponse response;
      response.set_version(1);
      response.set_service_id(service);
      try {
        auto admitted = [&] {
          if ((!health_only && asterion::service::stop_requested()) ||
              std::chrono::steady_clock::now() >= deadline)
            throw asterion::Error(asterion::ErrorCode::unavailable,
                                  "task request admission timed out");
        };
        const auto verification_deadline =
            std::chrono::steady_clock::now() + asterion::protocol::task_verification_timeout;
        const auto input_deadline =
            std::chrono::steady_clock::now() + asterion::protocol::task_input_timeout;
        auto input_in_time = [&] {
          if (asterion::service::stop_requested() ||
              std::chrono::steady_clock::now() >= input_deadline)
            throw asterion::Error(asterion::ErrorCode::unavailable,
                                  "task input preparation timed out");
        };
        auto data_input = [&](asterion::data::v1::DataRequest query)
            -> PolledTask<asterion::data::v1::DataResponse> {
          input_in_time();
          const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
              input_deadline - std::chrono::steady_clock::now());
          auto reply = co_await data_call(std::move(query), remaining, request.correlation_id());
          input_in_time();
          co_return reply;
        };
        auto verified_in_time = [&] {
          if (asterion::service::stop_requested() ||
              std::chrono::steady_clock::now() >= verification_deadline)
            throw asterion::Error(asterion::ErrorCode::unavailable,
                                  "task result verification timed out");
        };
        admitted();
        auto parse = [&] {
          if (!request.ParseFromString(frame))
            throw std::invalid_argument("invalid task Protobuf");
          asterion::protocol::validate_message(request);
        };
        if (frame.size() > 65536)
          co_await file_work(parse);
        else
          parse();
        std::string{}.swap(frame);
        response.set_correlation_id(request.correlation_id());
        asterion::validate_id(request.correlation_id());
        if (request.version() != 1 || request.service_id() != service)
          throw std::invalid_argument("task protocol version or service mismatch");
        if (request.has_heartbeat()) {
          auto* health = response.mutable_health();
          health->set_instance_id(service);
          health->set_process_id(process);
          health->set_data_instance(data_instance);
          health->set_version(ASTERION_PRODUCT_VERSION);
          health->set_recovery_required(degraded);
          health->set_initialized(initialized);
          health->set_uptime_ms(
              static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::steady_clock::now() - started)
                                             .count()));
          co_return response;
        }
        if (request.has_quiesce()) {
          if (!health_only)
            throw asterion::Error(asterion::ErrorCode::permission_denied,
                                  "upgrade control requires the private health channel");
          quiescing = true;
          if (request.quiesce().stop()) {
            if (!initialized || !writing.empty() || storage->has_submission())
              throw asterion::Error(asterion::ErrorCode::unavailable,
                                    "upgrade is waiting for running task tasks");
            const auto tasks = storage->active_tasks();
            for (const auto& task : tasks)
              if (task.state() == wire::RUNNING || task.state() == wire::CANCEL_REQUESTED ||
                  task.state() == wire::PUBLISHING)
                throw asterion::Error(asterion::ErrorCode::unavailable,
                                      "upgrade is waiting for running task tasks");
            asterion::service::request_stop();
          }
          response.mutable_health()->set_instance_id(service);
          response.mutable_health()->set_process_id(process);
          response.mutable_health()->set_data_instance(data_instance);
          co_return response;
        }
        if (!initialized)
          throw asterion::Error(asterion::ErrorCode::unavailable, "task service is initializing");
        if (health_only)
          throw std::invalid_argument("health channel only accepts heartbeat");
        if (request.has_dispatch() && !worker)
          throw asterion::Error(asterion::ErrorCode::permission_denied,
                                "task dispatch requires the private worker channel");
        admitted();
        expire_leases();
        admitted();
        std::string task_id;
        if (request.has_get())
          task_id = request.get().id();
        else if (request.has_cancel())
          task_id = request.cancel().id();
        else if (request.has_result())
          task_id = request.result().id();
        else if (request.has_claim())
          task_id = request.claim().id();
        else if (request.has_progress())
          task_id = request.progress().id();
        else if (request.has_finish())
          task_id = request.finish().id();
        else if (request.has_fail())
          task_id = request.fail().id();
        else if (request.has_cancel_ack())
          task_id = request.cancel_ack().id();
        const std::string* report_token = nullptr;
        if (request.has_progress())
          report_token = &request.progress().token();
        else if (request.has_finish())
          report_token = &request.finish().token();
        else if (request.has_fail())
          report_token = &request.fail().token();
        else if (request.has_cancel_ack())
          report_token = &request.cancel_ack().token();
        auto ready = [&]() -> PolledTask<std::chrono::steady_clock::time_point> {
          co_await wait_for_store(task_id);
          const auto observed = std::chrono::steady_clock::now();
          if (report_token) {
            // Expiration is a state-owner fact, independent of whether its journal
            // record has obtained a writer slot or completed its durable barrier.
            const auto lease = leases.find(task_id);
            if (lease == leases.end() || lease->second.token != *report_token ||
                observed >= lease->second.expires)
              throw std::invalid_argument("stale or inactive task attempt");
          }
          co_return observed;
        };
        co_await ready();
        admitted();
        auto submit = [&](auto make_input) -> PolledTask<asterion::tasks::Store::Change> {
          std::optional<asterion::tasks::Store::Submission> input;
          co_await file_work([&] { input.emplace(make_input()); });
          co_await ready();
          input_in_time();
          if (quiescing)
            throw asterion::Error(asterion::ErrorCode::unavailable,
                                  "task service is preparing for upgrade");
          storage->admit_submission(*input);
          try {
            co_await file_work([&] { input->prepare_files(); });
            // Admission has reserved the task's capacity and order. Complete
            // that accepted submission even if its client deadline has passed.
            co_await ready();
            co_return co_await commit(storage->register_submission(*input));
          } catch (...) {
            if (storage->abandon_submission(*input))
              degraded = true;
            throw;
          }
        };
        auto read_result = [&](std::string id) -> PolledTask<wire::TaskResponse> {
          auto slot = verification_slots.acquire(false, id);
          auto read = storage->prepare_result(id);
          co_await file_work([&] { read.verify(); });
          co_await ready();
          verified_in_time();
          co_return storage->confirm_result(std::move(read));
        };
        if (quiescing && (request.has_submit() || request.has_retry() || request.has_dispatch() ||
                          (!worker && request.has_claim())))
          throw asterion::Error(asterion::ErrorCode::unavailable,
                                "task service is preparing for upgrade");
        std::optional<asterion::tasks::VerificationSlots::Lease> preparation;
        if (request.has_submit() &&
            (request.submit().has_backtest() || request.submit().has_factor_request() ||
             request.submit().has_daily_factor() || request.submit().has_download_authorization()))
          preparation.emplace(input_slots.acquire(false, request.correlation_id()));
        if (request.has_history_usage()) {
          auto slot = input_slots.acquire(false, request.history_usage().id());
          auto read = storage->prepare_history_usage(request.history_usage().id());
          while (storage->next_history_page(read)) {
            co_await file_work([&] { read.load_page(); });
            co_await ready();
          }
          *response.mutable_history_usage() = read.take();
        } else if (request.has_dispatch()) {
          bool data_available = true;
          const auto tasks = storage->active_tasks();
          const bool needs_data = std::ranges::any_of(
              tasks, [](const auto& task) { return task.state() == wire::QUEUED; });
          if (needs_data) {

            try {
              asterion::data::v1::DataRequest heartbeat;
              heartbeat.mutable_heartbeat();
              const auto reply =
                  co_await data_call(std::move(heartbeat), 1s, request.correlation_id());
              if (!reply.has_health() || reply.health().instance_id() != data_instance ||
                  reply.health().task_instance() != service)
                throw std::invalid_argument("data and task service bindings disagree");
              data_available = reply.health().initialized() && !reply.health().recovery_required();
            } catch (const asterion::Error& error) {
              if (error.code() != asterion::ErrorCode::unavailable)
                throw;
              data_available = false;
            }

            co_await ready();
            admitted();
            if (quiescing)
              throw asterion::Error(asterion::ErrorCode::unavailable,
                                    "task service is preparing for upgrade");
          }
          *response.mutable_launches() = storage->dispatch(request.dispatch(), data_available);
        } else if (request.has_submit()) {
          const auto& p = request.submit();
          if (p.has_download_authorization()) {

            asterion::data::v1::DataRequest query;
            query.mutable_download_authorization()->set_id(p.download_authorization());
            const auto prepared = co_await data_input(std::move(query));
            if (prepared.download_authorization().task_id() != p.id())
              throw std::invalid_argument("invalid task download authorization");

            co_await ready();
            input_in_time();
            if (quiescing)
              throw std::invalid_argument("task service is preparing for upgrade");
            *response.mutable_task() =
                (co_await submit([&] {
                  return storage->submission(prepared.download_authorization());
                })).task();
          } else if (p.has_daily_factor()) {

            asterion::data::v1::DataRequest query;
            query.mutable_daily_factor_dataset()->set_id(p.daily_factor().source_dataset_id());
            auto prepared = co_await data_input(std::move(query));
            const auto& parameters = p.daily_factor();
            asterion::factor::v1::DailyFactorInput input;
            input.set_version(1);
            input.set_lookback(parameters.lookback());
            input.set_horizon(parameters.horizon());
            if (parameters.has_full_sample())
              input.set_full_sample(parameters.full_sample());
            else if (parameters.has_holdout_start())
              input.set_holdout_start(parameters.holdout_start());
            *input.mutable_dataset() = std::move(*prepared.mutable_daily_factor_dataset());

            co_await ready();
            input_in_time();
            if (quiescing)
              throw asterion::Error(asterion::ErrorCode::unavailable,
                                    "task service is preparing for upgrade");
            *response.mutable_task() =
                (co_await submit([&] {
                  input.set_dataset_revision(
                      asterion::protocol::daily_factor_revision(input.dataset()));
                  return storage->submission(p.id(), std::move(input));
                })).task();
          } else if (p.has_backtest()) {
            // Data owns historical resolution; Task owns this exact calculation
            // definition and its copied immutable input after acceptance.
            const auto& b = p.backtest();
            if (b.contracts().empty() || b.contracts_size() > 20)
              throw std::invalid_argument("backtest requires 1 to 20 contracts");
            // Months of a dominant series are resolved together by Data.
            std::vector<bool> in_series(static_cast<std::size_t>(b.contracts_size()));
            for (const auto& series : b.series()) {
              if (series.contracts_size() < 2)
                throw std::invalid_argument(
                    "a dominant series requires at least two month contracts");
              for (const auto index : series.contracts()) {
                if (index >= in_series.size() || in_series[index])
                  throw std::invalid_argument("invalid dominant series contracts");
                in_series[index] = true;
              }
            }

            // The frozen contracts: ordinary ones in request order, then the
            // dominant months of each series.
            std::vector<std::pair<std::size_t, asterion::data::v1::BarDataset>> datasets;
            std::size_t total_bars = 0;
            const auto keep = [&](std::size_t index, asterion::data::v1::BarDataset dataset) {
              total_bars += static_cast<std::size_t>(dataset.bars_size());
              if (total_bars > asterion::protocol::max_dataset_bars)
                throw std::invalid_argument("dataset exceeds 200000 bars; narrow the date range");
              datasets.emplace_back(index, std::move(dataset));
            };
            for (std::size_t c = 0; c < in_series.size(); ++c) {
              if (in_series[c])
                continue;
              asterion::data::v1::DataRequest query;
              *query.mutable_bar_dataset() = b.contracts(static_cast<int>(c)).data();
              auto prepared = co_await data_input(std::move(query));
              keep(c, std::move(*prepared.mutable_bar_dataset()));
            }
            std::vector<asterion::backtest::v1::DominantSchedule> schedules;
            for (const auto& series : b.series()) {
              asterion::data::v1::DataRequest query;
              for (const auto index : series.contracts())
                *query.mutable_dominant_series()->add_months() = b.contracts(index).data();
              auto prepared = co_await data_input(std::move(query));
              auto& resolved = *prepared.mutable_dominant_series();
              if (resolved.months_size() != resolved.datasets_size())
                throw std::invalid_argument("dominant series response has inconsistent months");
              for (auto& roll : *resolved.mutable_schedule()->mutable_rolls())
                roll.set_contract(roll.contract() + static_cast<unsigned>(datasets.size()));
              for (int m = 0; m < resolved.months_size(); ++m) {
                if (resolved.months(m) >= static_cast<unsigned>(series.contracts_size()))
                  throw std::invalid_argument("dominant series response has an unknown month");
                keep(series.contracts(resolved.months(m)),
                     std::move(*resolved.mutable_datasets(m)));
              }
              schedules.push_back(std::move(*resolved.mutable_schedule()));
            }
            asterion::backtest::v1::BacktestInput input;
            input.set_version(8);
            auto* paper = input.mutable_paper();
            *paper->mutable_deposit() = b.deposit();
            *paper->mutable_risk() = b.risk();
            for (auto& [request, dataset] : datasets) {
              auto* contract = paper->add_contracts();
              *contract->mutable_dataset() = std::move(dataset);
              *contract->mutable_cost_schedule() =
                  b.contracts(static_cast<int>(request)).cost_schedule();
            }
            for (auto& schedule : schedules)
              *input.add_series() = std::move(schedule);

            *input.mutable_sma() = b.sma();

            co_await ready();
            input_in_time();
            if (quiescing)
              throw asterion::Error(asterion::ErrorCode::unavailable,
                                    "task service is preparing for upgrade");
            *response.mutable_task() =
                (co_await submit([&] {
                  input.set_dataset_revision(asterion::protocol::dataset_revision(input.paper()));
                  return storage->submission(p.id(), std::move(input));
                })).task();
          } else if (p.has_factor_request()) {

            asterion::data::v1::DataRequest query;
            *query.mutable_bar_dataset() = p.factor_request().data();
            auto prepared = co_await data_input(std::move(query));
            auto dataset = std::move(*prepared.mutable_bar_dataset());
            const auto& f = p.factor_request();
            asterion::factor::v1::FactorInput input;
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

            co_await ready();
            input_in_time();
            if (quiescing)
              throw asterion::Error(asterion::ErrorCode::unavailable,
                                    "task service is preparing for upgrade");
            *response.mutable_task() = (co_await submit([&] {
                                         return storage->submission(p.id(), std::move(input));
                                       })).task();
          } else
            throw std::invalid_argument("missing task input");
        } else if (request.has_get()) {
          auto slot = input_slots.acquire(false, request.get().id());
          auto read = storage->prepare_input(request.get().id());
          co_await file_work([&] { read.load(); });
          co_await ready();
          input_in_time();
          *response.mutable_task() = storage->confirm_input(std::move(read));
        } else if (request.has_list()) {
          *response.mutable_tasks() =
              storage->list(request.list().limit(), request.list().before_sequence());
        } else if (request.has_cancel())
          *response.mutable_task() =
              (co_await commit(storage->cancel(request.cancel().id()))).task();
        else if (request.has_retry())
          *response.mutable_task() = (co_await commit(storage->retry(request.retry().id()))).task();
        else if (request.has_result()) {
          response = co_await read_result(request.result().id());
          response.set_version(1);
          response.set_service_id(service);
          response.set_correlation_id(request.correlation_id());
        } else if (request.has_claim()) {
          if (storage->describe(request.claim().id()).kind() != request.claim().kind())
            throw std::invalid_argument("worker kind does not match task");
          // Load before committing an attempt. Slow file parsing must not
          // consume an active lease or block unrelated worker controls.
          auto slot = input_slots.acquire(true, request.claim().id());
          auto read = storage->prepare_input(request.claim().id());
          co_await file_work([&] { read.load_for_claim(); });
          co_await ready();
          input_in_time();
          if (quiescing && !worker)
            throw asterion::Error(asterion::ErrorCode::unavailable,
                                  "task service is preparing for upgrade");
          auto* attempt = response.mutable_attempt();
          const auto token = (co_await commit(storage->claim(read))).token();
          std::exception_ptr allocation_error;
          try {
            *attempt = storage->confirm_attempt(std::move(read));
            attempt->set_token(token);
            attempt->set_data_endpoint(data_endpoint);
            attempt->set_data_instance(data_instance);
            if (attempt->task().has_minutes() || attempt->task().has_daily()) {
              asterion::data::v1::DataRequest allocation;
              auto* download = allocation.mutable_allocate_download();
              download->set_authorization_id(attempt->task().download_authorization());
              auto* identity = download->mutable_identity();
              identity->set_data_instance(data_instance);
              identity->set_task_instance(service);
              identity->set_task_id(attempt->task().id());
              identity->set_attempt(attempt->task().attempt());
              if (attempt->task().has_daily())
                *download->mutable_daily() = attempt->task().daily();
              else
                *download->mutable_minutes() = attempt->task().minutes();

              asterion::data::v1::DataResponse allocated;
              allocated = co_await data_call(std::move(allocation), 10s, request.correlation_id());
              co_await ready();
              if (storage->describe(request.claim().id()).state() != wire::RUNNING)
                throw asterion::Error(asterion::ErrorCode::cancelled,
                                      "download cancelled before allocation completed");
              attempt->set_output_directory(allocated.download_directory().directory());
            }
            leases[request.claim().id()] = {attempt->token(),
                                            std::chrono::steady_clock::now() +
                                                std::chrono::seconds(worker_timeout)};
          } catch (...) {
            allocation_error = std::current_exception();
          }
          if (allocation_error) {
            co_await wait_for_store(request.claim().id());
            std::string reason;
            try {
              std::rethrow_exception(allocation_error);
            } catch (const std::exception& error) {
              reason = error.what();
            }
            co_await commit(storage->fail(request.claim().id(), token, reason));
            std::rethrow_exception(allocation_error);
          }
        } else if (request.has_progress()) {
          const auto& p = request.progress();
          // A report waiting behind another durable change cannot revive an
          // expired attempt. Liveness is observed now, not at a later disk ack.
          expire_leases();
          const auto observed = co_await ready();
          auto change = storage->progress(p.id(), p.token(), p.completed());
          leases.at(p.id()).expires = observed + std::chrono::seconds(worker_timeout);
          auto applied = co_await commit(std::move(change));
          *response.mutable_task() = applied.task();
        } else if (request.has_finish()) {
          const auto& p = request.finish();
          auto slot = verification_slots.acquire(true, p.id());
          auto completion = storage->prepare_finish(p);
          co_await file_work([&] { completion.prepare_payload(); });
          co_await ready();
          verified_in_time();
          expire_leases();
          co_await ready();
          if ((p.has_minutes() || p.has_daily()) &&
              storage->describe(p.id()).state() != wire::CANCEL_REQUESTED) {
            asterion::data::v1::DataRequest preparation;
            *preparation.mutable_prepare_download() = completion.download_preparation();

            const auto prepared = co_await data_call(std::move(preparation),
                                                     asterion::protocol::task_verification_timeout,
                                                     request.correlation_id());

            co_await ready();
            verified_in_time();
            expire_leases();
            co_await ready();
            auto applied = co_await commit(
                storage->prepare_publication(std::move(completion), prepared.prepared_download()));
            *response.mutable_task() = applied.task();
            if (applied.task().state() == wire::PUBLISHING) {
              publication_wake = true;
            }
          } else
            *response.mutable_task() =
                (co_await commit(storage->finish(std::move(completion)))).task();
        } else if (request.has_fail()) {
          const auto& p = request.fail();
          if (!asterion::parse_error_code(p.error_code()))
            throw std::invalid_argument("invalid task failure error code");
          *response.mutable_task() =
              (co_await commit(storage->fail(p.id(), p.token(), p.error()))).task();
        } else if (request.has_cancel_ack()) {
          const auto& p = request.cancel_ack();
          *response.mutable_task() =
              (co_await commit(storage->acknowledge_cancel(p.id(), p.token()))).task();
        } else
          throw std::invalid_argument("missing task operation");
        if (response.has_task() && !request.has_get())
          response.mutable_task()->clear_definition();
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
      std::string_view task;
      std::optional<std::uint32_t> attempt;
      if (response.has_task()) {
        task = response.task().id();
        attempt = response.task().attempt();
      } else if (response.has_attempt()) {
        task = response.attempt().task().id();
        attempt = response.attempt().task().attempt();
      } else if (response.has_result_task()) {
        task = response.result_task().id();
        attempt = response.result_task().attempt();
      } else if (request.has_submit())
        task = request.submit().id();
      else if (request.has_get())
        task = request.get().id();
      else if (request.has_cancel())
        task = request.cancel().id();
      else if (request.has_retry())
        task = request.retry().id();
      else if (request.has_result())
        task = request.result().id();
      else if (request.has_claim())
        task = request.claim().id();
      else if (request.has_progress())
        task = request.progress().id();
      else if (request.has_finish())
        task = request.finish().id();
      else if (request.has_fail())
        task = request.fail().id();
      else if (request.has_cancel_ack())
        task = request.cancel_ack().id();
      asterion::protocol::log_rpc_result("task-service", request, response,
                                         request.has_list() || request.has_dispatch() ||
                                             request.has_progress() || request.has_heartbeat(),
                                         {{"service_id", service}, {"task_id", task}}, attempt);
      co_return response;
    };
    using Host = asterion::service::RpcHost;
    auto serve = [&](std::string frame, bool health_only, bool worker) -> PolledTask<std::string> {
      auto response = co_await respond(std::move(frame), health_only, worker,
                                       std::chrono::steady_clock::now() + 10s);
      if (!response.has_result_task() && !response.has_attempt() &&
          (!response.has_task() ||
           response.task().definition_case() == wire::Task::DEFINITION_NOT_SET))
        co_return response.SerializeAsString();
      std::string reply;
      co_await file_work([&] { reply = response.SerializeAsString(); });
      co_return reply;
    };
    auto handle = [&](std::string frame, bool health_only, bool worker) -> Host::Reply {
      // RpcHost retains a pending reply through disconnects. The frame and every
      // suspended local stay alive until accepted file/journal work has finished.
      auto operation =
          std::make_shared<PolledTask<std::string>>(serve(std::move(frame), health_only, worker));
      return [operation]() -> std::optional<std::string> {
        if (!operation->poll())
          return {};
        return operation->take();
      };
    };
    Host::Options options;
    options.owner_pid = owner_pid;
    options.connections = 12;
    options.handshake = 3s;
    options.receive = 10s;
    options.send = 3s;
    options.request_bytes = asterion::ipc::Channel::max_frame;
    options.payload_bytes = 128 * 1024 * 1024;
    if (!worker_endpoint.empty())
      options.local_endpoints.push_back({worker_endpoint,
                                         [&](const Host::Peer&, std::string frame) {
                                           return handle(std::move(frame), false, true);
                                         },
                                         8, false, asterion::ipc::Channel::max_frame,
                                         128 * 1024 * 1024});
    if (!health_endpoint.empty())
      options.local_endpoints.push_back({health_endpoint,
                                         [&](const Host::Peer&, std::string frame) {
                                           return handle(std::move(frame), true, false);
                                         },
                                         4, true, 65536, 4 * 65536});
    options.advance = [&](Host::Stage stage) {
      if (opening.valid() && opening.wait_for(0s) == std::future_status::ready) {
        opening.get();
        initialized = true;
      }
      if (stage != Host::Stage::running)
        quiescing = true;
      data.poll();
      if (initialized) {
        collect_writes();
        if (stage == Host::Stage::running)
          expire_leases();
        if (!publication && !degraded && stage == Host::Stage::running &&
            (publication_wake || std::chrono::steady_clock::now() >= next_publication)) {
          publication.emplace(publish());
          publication_wake = false;
        }
        if (publication && publication->poll()) {
          try {
            publication->take();
          } catch (const std::exception& error) {
            degraded = true;
            asterion::log_process_event("task-service", asterion::LogLevel::error,
                                        "publication.failed", {{"message", error.what()}});
          }
          publication.reset();
          next_publication = std::chrono::steady_clock::now() + 1s;
        }
      }
      // Accepted work retains its coroutine frame through all file/RPC waits,
      // even after a client disconnect. Never destroy locals still used by a pool.
      return stage == Host::Stage::stopping_resources && !opening.valid() && writing.empty() &&
             !publication;
    };
    asterion::Progress io_progress;
    Host host(
        transport,
        [&](const Host::Peer&, std::string frame) {
          return handle(std::move(frame), false, false);
        },
        std::move(options), io_progress);
    opening = files.submit([&](std::stop_token) {
      storage = std::make_unique<asterion::tasks::Store>(
          std::filesystem::absolute(directory), asterion::tasks::Identity{service, data_instance});
    });
    try {
      if (!host.run())
        std::_Exit(0);
    } catch (const std::exception& error) {
      asterion::log_process_event("task-service", asterion::LogLevel::error, "service.failed",
                                  {{"message", error.what()}});
      std::cerr << "Task service failed: " << error.what() << '\n';
      std::_Exit(1);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Task service failed: " << error.what() << '\n';
    asterion::log_process_event("task-service", asterion::LogLevel::error, "service.failed",
                                {{"message", error.what()}});
    return 1;
  }
}
