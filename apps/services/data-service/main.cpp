#include "data_store.hpp"
#include "download_budget.hpp"
#include "daily_factor_source.hpp"
#include "history_coverage.hpp"
#include "history_daily.hpp"
#include "history_minutes.hpp"
#include "history_update.hpp"
#include "history_providers.hpp"
#include <CLI/CLI.hpp>
#include <asterion/kernel/logger.hpp>
#include <asterion/kernel/native_plugin.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/rpc_host.hpp>
#include <asterion/kernel/trace.hpp>
#include <asterion/kernel/thread_pool.hpp>
#include <asterion/protocol/rpc_diagnostics.hpp>
#include <iostream>
#include <map>
#include <deque>
#include <asterion/kernel/process/artifact.hpp>
using namespace std::chrono_literals;
int main(int argc, char** argv) {
  CLI::App app{"Asterion historical data service"};
  app.set_version_flag("--version", "asterion-data-service " ASTERION_PRODUCT_VERSION);
  std::string directory, service, task_instance, health_endpoint, worker_endpoint, plugins;
  std::uint64_t owner_pid = 0;
  unsigned file_workers = 2;
  asterion::service::Transport transport;
  app.add_option("--directory", directory)->required()->check(CLI::ExistingDirectory);
  app.add_option("--session", service)->required();
  app.add_option("--task-instance", task_instance)->required();
  app.add_option("--endpoint", transport.endpoint);
  app.add_option("--bind", transport.bind);
  app.add_option("--port", transport.port)->check(CLI::Range(1, 65535));
  app.add_option("--tls-ca", transport.tls.ca_file);
  app.add_option("--tls-cert", transport.tls.certificate_file);
  app.add_option("--tls-key", transport.tls.private_key_file);
  app.add_option("--worker-endpoint", worker_endpoint)->required();
  app.add_option("--health-endpoint", health_endpoint);
  app.add_option("--owner-pid", owner_pid);
  app.add_option("--file-workers", file_workers)->check(CLI::Range(1, 2));
  app.add_option("--plugin-directory", plugins)->check(CLI::ExistingDirectory);
  argv = app.ensure_utf8(argv);
  CLI11_PARSE(app, argc, argv);
  try {
    namespace wire = asterion::data::v1;
    if (!plugins.empty())
      asterion::configure_native_plugins(plugins);
    asterion::validate_id(service);
    transport.validate();
    asterion::service::install_stop_signals();
    const auto process = asterion::unique_process_id();
    const auto started = std::chrono::steady_clock::now();
    bool initialized = false, degraded = false, quiescing = false;
    auto health_response = [&](const wire::DataRequest& request) {
      wire::DataResponse response;
      response.set_version(1);
      response.set_service_id(service);
      response.set_correlation_id(request.correlation_id());
      auto* health = response.mutable_health();
      health->set_instance_id(service);
      health->set_task_instance(task_instance);
      health->set_process_id(process);
      health->set_version(ASTERION_PRODUCT_VERSION);
      health->set_initialized(initialized);
      health->set_recovery_required(degraded);
      health->set_uptime_ms(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count());
      return response;
    };
    auto parse = [&](const std::string& frame) {
      wire::DataRequest request;
      if (!request.ParseFromString(frame))
        throw std::invalid_argument("invalid data Protobuf");
      asterion::protocol::validate_message(request);
      asterion::validate_id(request.correlation_id());
      if (request.version() != 1 || request.service_id() != service)
        throw std::invalid_argument("data protocol version or service mismatch");
      return request;
    };
    std::unique_ptr<asterion::data::Store> store;
    std::unique_ptr<asterion::data::DownloadBudget> budgets;
    asterion::ThreadPool writer(1, 8), readers(file_workers, 8);
    std::future<void> opening;
    std::size_t named_dataset_count = 0;
    // Preparation owns large reads and provider calls. The returned immutable
    // commit is handed to the serial writer after preparation releases its slot.
    auto prepare = [&](const wire::DataRequest& request, wire::DataResponse& response,
                       std::stop_token stop, bool& creates_dataset,
                       std::string& publication_dataset) -> std::function<void()> {
      if (request.has_authorize_download()) {
        auto verified = store->verify_authorization(request.authorize_download());
        return [&, verified = std::move(verified)] {
          *response.mutable_download_authorization() = store->authorize(verified);
        };
      } else if (request.has_download_authorization()) {
        *response.mutable_download_authorization() =
            store->authorization(request.download_authorization().id());
      } else if (request.has_download_credentials()) {
        *response.mutable_download_credentials() =
            store->credentials(request.download_credentials());
      } else if (request.has_save_dataset()) {
        auto verified = store->verify_dataset(request.save_dataset());
        creates_dataset = verified.creates_entry();
        return [&, verified = std::move(verified)] {
          store->save(verified);
          *response.mutable_saved_dataset() = request.save_dataset();
        };
      } else if (request.has_prepare_download()) {
        auto verified = store->verify_download(request.prepare_download());
        return [&, verified = std::move(verified)] {
          *response.mutable_prepared_download() = store->prepare(verified);
        };
      } else if (request.has_allocate_download()) {
        auto verified = store->verify_allocation(request.allocate_download());
        return [&, verified = std::move(verified)] {
          *response.mutable_download_directory() = store->allocate(verified);
        };
      } else if (request.has_publish_download()) {
        auto verified = store->verify_publication(request.publish_download());
        publication_dataset = verified.dataset_id();
        return [&, verified = std::move(verified)] {
          *response.mutable_published_download() = store->publish(verified);
        };
      } else if (request.has_published_download())
        *response.mutable_published_download() = store->published(request.published_download());
      else if (request.has_history_usage())
        *response.mutable_history_usage() = store->history_usage(request.history_usage().id());
      else if (request.has_record())
        *response.mutable_record() = store->archive().get(request.record().id());
      else if (request.has_saved_dataset())
        *response.mutable_saved_dataset() =
            store->archive().named_dataset(request.saved_dataset().id());
      else if (request.has_saved_datasets())
        *response.mutable_saved_datasets() = store->archive().named_datasets();
      else if (request.has_datasets()) {
        const auto& filter = request.datasets();
        auto* result = response.mutable_datasets();
        for (const auto& item : store->archive().datasets(
                 {filter.venue(), filter.product(), filter.contract_id(), filter.source()})) {
          auto* row = result->add_items();
          row->set_id(item.id);
          row->set_contract_id(item.contract.key());
          row->set_source(item.source);
          row->set_revision(item.revision);
          row->set_begin(item.begin);
          row->set_end(item.end);
          row->set_interval_minutes(item.interval_minutes);
          row->set_rows(item.rows);
        }
      } else if (request.has_minute_page()) {
        const auto& query = request.minute_page();
        if (query.dataset_id().empty() || !query.task_id().empty())
          throw std::invalid_argument("data query requires a published dataset identity");
        const auto record = store->archive().get(query.dataset_id());
        *response.mutable_minute_page() = asterion::history_files::read_minute_page(
            record.minutes(), record.minute_result(), query);
      } else if (request.has_daily_page()) {
        const auto& query = request.daily_page();
        if (query.dataset_id().empty() || !query.task_id().empty())
          throw std::invalid_argument("data query requires a published dataset identity");
        const auto record = store->archive().get(query.dataset_id());
        *response.mutable_daily_page() =
            asterion::history_files::read_daily_page(record.daily(), record.daily_result(), query);
      } else if (request.has_bar_dataset())
        *response.mutable_bar_dataset() =
            asterion::data::resolve_bar_dataset(store->sources(request.bar_dataset()));
      else if (request.has_daily_factor_dataset())
        *response.mutable_daily_factor_dataset() = asterion::data::daily_factor_dataset(
            store->archive().get(request.daily_factor_dataset().id()));
      else if (request.has_dominant_series()) {
        if (request.dominant_series().months_size() > 20)
          throw std::invalid_argument("backtest requires 1 to 20 contracts");
        std::vector<asterion::data::BarDatasetSources> months;
        for (const auto& month : request.dominant_series().months())
          months.push_back(store->sources(month));
        auto resolved = asterion::data::resolve_dominant_series(months);
        auto* preview = response.mutable_dominant_series();
        for (const auto index : resolved.months)
          preview->add_months(static_cast<unsigned>(index));
        for (auto& dataset : resolved.datasets)
          *preview->add_datasets() = std::move(dataset);
        *preview->mutable_schedule() = std::move(resolved.schedule);
      } else if (request.has_coverage())
        *response.mutable_coverage() =
            asterion::data::history_coverage(store->archive(), request.coverage());
      else if (request.has_update_plan())
        *response.mutable_update_plan() = asterion::data::history_update_plan(
            store->archive(), request.update_plan(),
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
      else if (request.has_verify_connection()) {
        auto* result = response.mutable_connection_verification();
        for (const auto& check : asterion::history_providers::verify_connection(
                 request.verify_connection().source(), request.verify_connection().credential(),
                 stop)) {
          auto* row = result->add_checks();
          row->set_scope(check.scope);
          row->set_state(check.state);
        }
      } else if (request.has_catalog()) {
        const auto& query = request.catalog();
        auto* result = response.mutable_catalog();
        result->set_source(query.source());
        for (const auto& item : asterion::history_providers::catalog(
                 query.source(), query.credential(), query.venue(), query.product(), stop)) {
          auto* row = result->add_items();
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
      } else if (request.has_sources()) {
        auto* result = response.mutable_sources();
        for (const auto& source : asterion::history_providers::sources()) {
          auto* out = result->add_sources();
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
            schema->set_requests_per_minute_default(source.connection->requests_per_minute_default);
            schema->set_requests_per_minute_max(source.connection->requests_per_minute_max);
          }
        }
      } else
        throw std::invalid_argument("missing data operation");
      return {};
    };
    using Host = asterion::service::RpcHost;
    struct Work {
      wire::DataRequest request;
      wire::DataResponse response;
      std::function<void()> commit;
      std::future<void> completion;
      std::string frame, publication_dataset;
      wire::StoredDownloadAuthorization authorization;
      std::optional<asterion::data::DownloadBudget::Configuration> budget_configuration;
      enum class Phase { queued, preparing, prepared, writing, complete };
      Phase phase = Phase::queued;
      std::vector<std::string> objects;
      bool reads_catalog = false, writes_catalog = false, creates_dataset = false;
      bool reading_catalog = false, awaiting_catalog_write = false, writing_catalog = false;
    };
    // Only the I/O owner touches ordering and visibility. A request retains its
    // place until durable completion, including when its client stops reading.
    std::map<std::string, std::deque<const Work*>> object_order;
    std::size_t catalog_readers = 0, catalog_waiting_writers = 0;
    bool catalog_writing = false;
    auto is_turn = [&](const Work& work) {
      for (const auto& key : work.objects)
        if (object_order.at(key).front() != &work)
          return false;
      return true;
    };
    auto release = [&](Work& work) {
      for (const auto& key : work.objects) {
        auto& queue = object_order.at(key);
        std::erase(queue, &work);
        if (queue.empty())
          object_order.erase(key);
      }
      work.objects.clear();
      if (std::exchange(work.reading_catalog, false))
        --catalog_readers;
      if (std::exchange(work.awaiting_catalog_write, false))
        --catalog_waiting_writers;
      if (std::exchange(work.writing_catalog, false))
        catalog_writing = false;
    };
    auto require_ready = [&] {
      if (degraded)
        throw asterion::Error(asterion::ErrorCode::unavailable,
                              "data persistence failed; restart data service");
    };
    const auto fail = [](wire::DataResponse& response, const std::exception& error) {
      response.mutable_error()->set_code(
          std::string(asterion::error_name(asterion::classify(error))));
      response.mutable_error()->set_message(error.what());
    };
    auto write = [&](const std::shared_ptr<Work>& work) {
      auto commit = std::exchange(work->commit, {});
      work->completion = writer.submit([work, commit = std::move(commit)](std::stop_token) {
        asterion::TraceScope trace(work->request.correlation_id());
        commit();
        work->frame = work->response.SerializeAsString();
      });
    };
    auto budget_reply = [&](const std::shared_ptr<Work>& work) -> Host::Reply {
      return [&, work]() -> std::optional<std::string> {
        const bool configuring = work->request.has_configure_download_budget();
        try {
          if (!work->completion.valid()) {
            // One configuration is decided and committed at a time. Pending
            // callers retain only their bounded RPC slot; permit reads continue.
            require_ready();
            if (!is_turn(*work))
              return {};
            work->completion = readers.submit([&, work, configuring](std::stop_token) {
              asterion::TraceScope trace(work->request.correlation_id());
              if (configuring)
                *work->response.mutable_download_budget() =
                    store->verify_budget(work->request.configure_download_budget());
              else
                work->authorization =
                    store->download_authorization(work->request.acquire_download_permit());
            });
            return {};
          }
          if (work->completion.wait_for(0ms) != std::future_status::ready)
            return {};
          work->completion.get();
          if (!work->budget_configuration)
            require_ready();
          if (configuring) {
            if (!work->budget_configuration) {
              const auto& input = work->request.configure_download_budget();
              auto configuration =
                  budgets->configure(work->response.download_budget().provider_id(),
                                     input.credential(), input.requests_per_minute());
              work->completion = writer.submit(
                  [&, configuration, trace_id = work->request.correlation_id()](std::stop_token) {
                    asterion::TraceScope trace(trace_id);
                    budgets->persist(configuration);
                  });
              work->budget_configuration = std::move(configuration);
              return {};
            }
            budgets->committed(*work->budget_configuration);
          } else {
            *work->response.mutable_download_permit() =
                budgets->acquire(work->authorization.authorization().provider_id(),
                                 work->authorization.credential());
          }
        } catch (const std::exception& error) {
          if (work->budget_configuration) {
            budgets->persistence_failed();
            degraded = true;
          }
          fail(work->response, error);
        }
        release(*work);
        asterion::protocol::log_rpc_result("data-service", work->request, work->response, false,
                                           {{"service_id", service}});
        return work->response.SerializeAsString();
      };
    };
    auto handle = [&](std::string frame, bool worker) -> Host::Reply {
      auto work = std::make_shared<Work>();
      auto& request = work->request;
      auto& response = work->response;
      try {
        request = parse(frame);
        response = health_response(request);
        if (request.has_heartbeat())
          return
              [frame = response.SerializeAsString()] { return std::optional<std::string>(frame); };
        response.clear_result();
        if (!initialized)
          throw asterion::Error(asterion::ErrorCode::unavailable, "data service is initializing");
        require_ready();
        const bool mutation =
            request.has_configure_download_budget() || request.has_acquire_download_permit() ||
            request.has_authorize_download() || request.has_download_authorization() ||
            request.has_download_credentials() || request.has_save_dataset() ||
            request.has_allocate_download() || request.has_prepare_download() ||
            request.has_publish_download();
        const bool private_operation =
            request.has_allocate_download() || request.has_prepare_download() ||
            request.has_publish_download() || request.has_published_download() ||
            request.has_download_authorization() || request.has_download_credentials() ||
            request.has_acquire_download_permit();
        if (private_operation && !worker)
          throw asterion::Error(asterion::ErrorCode::permission_denied,
                                "download coordination requires the private data channel");
        if (mutation && quiescing)
          throw asterion::Error(asterion::ErrorCode::unavailable,
                                "data service is preparing for upgrade");
        const auto attempt = [&](const wire::DownloadIdentity& identity) {
          work->objects.push_back("download/" +
                                  asterion::sha256_bytes(identity.SerializeAsString()));
        };
        if (request.has_authorize_download())
          work->objects.push_back("authorization/" +
                                  asterion::sha256_bytes(service + "/" + task_instance + "/" +
                                                         request.authorize_download().task_id()));
        else if (request.has_download_authorization())
          work->objects.push_back("authorization/" + request.download_authorization().id());
        else if (request.has_allocate_download()) {
          attempt(request.allocate_download().identity());
          work->objects.push_back("authorization/" +
                                  request.allocate_download().authorization_id());
        } else if (request.has_prepare_download())
          attempt(request.prepare_download().identity());
        else if (request.has_publish_download())
          attempt(request.publish_download().identity());
        else if (request.has_published_download())
          attempt(request.published_download());
        else if (request.has_download_credentials())
          attempt(request.download_credentials());
        else if (request.has_acquire_download_permit())
          attempt(request.acquire_download_permit());
        else if (request.has_save_dataset())
          work->objects.push_back("dataset/" + request.save_dataset().id());
        else if (request.has_configure_download_budget())
          work->objects.push_back("budget-configuration");
        const auto history = [&](const std::string& id) {
          work->objects.push_back("history/" + id);
        };
        const auto selection = [&](const wire::BarDatasetRequest& input) {
          for (const auto& id : input.source_dataset_ids())
            history(id);
          for (const auto& id : input.settlement_dataset_ids())
            history(id);
        };
        if (request.has_record())
          history(request.record().id());
        else if (request.has_minute_page())
          history(request.minute_page().dataset_id());
        else if (request.has_daily_page())
          history(request.daily_page().dataset_id());
        else if (request.has_daily_factor_dataset())
          history(request.daily_factor_dataset().id());
        else if (request.has_bar_dataset())
          selection(request.bar_dataset());
        else if (request.has_dominant_series())
          for (const auto& month : request.dominant_series().months())
            selection(month);
        else if (request.has_save_dataset())
          for (const auto& input : request.save_dataset().selections())
            selection(input);
        else if (request.has_saved_dataset())
          work->objects.push_back("dataset/" + request.saved_dataset().id());
        else if (request.has_update_plan()) {
          history(request.update_plan().dataset_id());
          if (!request.update_plan().calendar_dataset_id().empty())
            history(request.update_plan().calendar_dataset_id());
        }
        std::ranges::sort(work->objects);
        work->objects.erase(std::unique(work->objects.begin(), work->objects.end()),
                            work->objects.end());
        for (const auto& key : work->objects)
          object_order[key].push_back(work.get());
        if (request.has_configure_download_budget() || request.has_acquire_download_permit())
          return budget_reply(work);
        work->writes_catalog = request.has_save_dataset() || request.has_publish_download();
        // Enumeration shares the directory publication barrier. Reads of known
        // immutable versions use object identities and remain available while a
        // different version is being committed.
        work->reads_catalog = request.has_history_usage() || request.has_saved_datasets() ||
                              request.has_datasets() || request.has_coverage();
      } catch (const std::exception& error) {
        response.set_version(1);
        response.set_service_id(service);
        response.set_correlation_id(request.correlation_id());
        fail(response, error);
        work->frame = response.SerializeAsString();
        work->phase = Work::Phase::complete;
      }
      return [&, work]() -> std::optional<std::string> {
        try {
          if (work->phase == Work::Phase::queued) {
            require_ready();
            if (!is_turn(*work) ||
                (work->reads_catalog && (catalog_writing || catalog_waiting_writers)))
              return {};
            work->completion = readers.submit([&, work](std::stop_token stop) {
              asterion::TraceScope trace(work->request.correlation_id());
              work->commit = prepare(work->request, work->response, stop, work->creates_dataset,
                                     work->publication_dataset);
              if (!work->commit)
                work->frame = work->response.SerializeAsString();
            });
            if (work->reads_catalog) {
              ++catalog_readers;
              work->reading_catalog = true;
            }
            work->phase = Work::Phase::preparing;
            return {};
          }
          if (work->phase == Work::Phase::preparing) {
            if (work->completion.wait_for(0ms) != std::future_status::ready)
              return {};
            work->completion.get();
            if (std::exchange(work->reading_catalog, false))
              --catalog_readers;
            if (work->commit) {
              if (!work->publication_dataset.empty()) {
                auto key = "history/" + work->publication_dataset;
                object_order[key].push_back(work.get());
                work->objects.push_back(std::move(key));
              }
              work->phase = Work::Phase::prepared;
              if (work->writes_catalog) {
                ++catalog_waiting_writers;
                work->awaiting_catalog_write = true;
              }
            } else {
              require_ready();
              work->phase = Work::Phase::complete;
            }
          }
          if (work->phase == Work::Phase::prepared) {
            require_ready();
            if (!is_turn(*work) || (work->writes_catalog && (catalog_readers || catalog_writing)))
              return {};
            if (work->creates_dataset && named_dataset_count >= 1000)
              throw asterion::Error(asterion::ErrorCode::resource_exhausted,
                                    "saved dataset capacity reached");
            // Quiesce closes admission; accepted preparation may still commit.
            write(work);
            if (work->writes_catalog) {
              --catalog_waiting_writers;
              work->awaiting_catalog_write = false;
              catalog_writing = work->writing_catalog = true;
            }
            work->phase = Work::Phase::writing;
            return {};
          }
          if (work->phase == Work::Phase::writing) {
            if (work->completion.wait_for(0ms) != std::future_status::ready)
              return {};
            work->completion.get();
            if (work->creates_dataset)
              ++named_dataset_count;
            work->phase = Work::Phase::complete;
          }
        } catch (const std::exception& error) {
          if (work->phase == Work::Phase::writing)
            degraded = true;
          fail(work->response, error);
          work->frame = work->response.SerializeAsString();
        }
        release(*work);
        asterion::protocol::log_rpc_result("data-service", work->request, work->response, false,
                                           {{"service_id", service}});
        return std::move(work->frame);
      };
    };
    Host::Options options;
    options.owner_pid = owner_pid;
    options.connections = 12;
    options.receive = 10s;
    options.send = 5s;
    options.local_endpoints.push_back(
        {worker_endpoint,
         [&](const Host::Peer&, std::string frame) { return handle(std::move(frame), true); }, 6,
         false, 1024 * 1024, 128 * 1024 * 1024});
    if (!health_endpoint.empty())
      options.local_endpoints.push_back(
          {health_endpoint,
           [&](const Host::Peer&, std::string frame) -> Host::Reply {
             const auto request = parse(frame);
             if (request.has_quiesce()) {
               quiescing = true;
               if (request.quiesce().stop())
                 asterion::service::request_stop();
             } else if (!request.has_heartbeat())
               throw std::invalid_argument("health channel only accepts heartbeat");
             return [frame = health_response(request).SerializeAsString()] {
               return std::optional<std::string>(frame);
             };
           },
           4, true});
    options.advance = [&](Host::Stage stage) {
      if (opening.valid() && opening.wait_for(0ms) == std::future_status::ready) {
        opening.get();
        initialized = true;
      }
      if (stage != Host::Stage::running)
        quiescing = true;
      // All admitted preparations and commits have completed when business
      // replies drained. The I/O owner never waits on a running file operation.
      return stage == Host::Stage::stopping_resources && !opening.valid();
    };
    asterion::Progress io_progress;
    Host host(
        transport,
        [&](const Host::Peer&, std::string frame) { return handle(std::move(frame), false); },
        std::move(options), io_progress);
    opening = writer.submit([&](std::stop_token) {
      store = std::make_unique<asterion::data::Store>(std::filesystem::absolute(directory), service,
                                                      task_instance);
      named_dataset_count = store->archive().named_datasets().items_size();
      budgets = std::make_unique<asterion::data::DownloadBudget>(
          std::filesystem::absolute(directory) / "budgets");
    });
    if (!host.run())
      std::_Exit(0);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Data service failed: " << error.what() << '\n';
    asterion::log_process_event("data-service", asterion::LogLevel::error, "service.failed",
                                {{"message", error.what()}});
    return 1;
  }
}
