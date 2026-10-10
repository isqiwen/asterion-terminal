#include "data_host.hpp"
#include "daily_factor_source.hpp"
#include "history_coverage.hpp"
#include "history_daily.hpp"
#include "history_minutes.hpp"
#include "history_providers.hpp"
#include "history_update.hpp"
#include <asterion/kernel/logger.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/trace.hpp>
#include <asterion/protocol/rpc_diagnostics.hpp>
#include <algorithm>
namespace asterion::data {
using namespace std::chrono_literals;
namespace wire = v1;
namespace {
void fail(wire::DataResponse& response, const std::exception& error) {
  response.mutable_error()->set_code(std::string(error_name(classify(error))));
  response.mutable_error()->set_message(error.what());
}
} // namespace
DataHost::DataHost(Configuration configuration)
    : configuration_(std::move(configuration)), process_(unique_process_id()),
      readers_(configuration_.file_workers, 8, service::wake_io_owner) {}
void DataHost::open() {
  opening_ = writer_.submit([this](std::stop_token) {
    const auto directory = std::filesystem::absolute(configuration_.directory);
    store_ =
        std::make_unique<Store>(directory, configuration_.service, configuration_.task_instance);
    named_dataset_count_ = store_->archive().named_datasets().items_size();
    budgets_ = std::make_unique<DownloadBudget>(directory / "budgets");
  });
}
wire::DataResponse DataHost::health_response(const wire::DataRequest& request) const {
  wire::DataResponse response;
  response.set_version(1);
  response.set_service_id(configuration_.service);
  response.set_correlation_id(request.correlation_id());
  auto* health = response.mutable_health();
  health->set_instance_id(configuration_.service);
  health->set_task_instance(configuration_.task_instance);
  health->set_process_id(process_);
  health->set_version(ASTERION_PRODUCT_VERSION);
  health->set_initialized(initialized_);
  health->set_recovery_required(degraded_);
  health->set_uptime_ms(std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - started_)
                            .count());
  return response;
}
wire::DataRequest DataHost::parse(const std::string& frame) const {
  wire::DataRequest request;
  if (!request.ParseFromString(frame))
    throw std::invalid_argument("invalid data Protobuf");
  protocol::validate_message(request);
  validate_id(request.correlation_id());
  if (request.version() != 1 || request.service_id() != configuration_.service)
    throw std::invalid_argument("data protocol version or service mismatch");
  return request;
}
void DataHost::require_ready() const {
  if (degraded_)
    throw Error(ErrorCode::unavailable, "data persistence failed; restart data service");
}
bool DataHost::is_turn(const Work& work) const {
  for (const auto& key : work.objects)
    if (object_order_.at(key).front() != &work)
      return false;
  return true;
}
void DataHost::release(Work& work) {
  for (const auto& key : work.objects) {
    auto& queue = object_order_.at(key);
    std::erase(queue, &work);
    if (queue.empty())
      object_order_.erase(key);
  }
  work.objects.clear();
  if (std::exchange(work.reading_catalog, false))
    --catalog_readers_;
  if (std::exchange(work.awaiting_catalog_write, false))
    --catalog_waiting_writers_;
  if (std::exchange(work.writing_catalog, false))
    catalog_writing_ = false;
}
std::function<void()> DataHost::prepare(const wire::DataRequest& request,
                                        wire::DataResponse& response, std::stop_token stop,
                                        bool& creates_dataset, std::string& publication_dataset) {
  if (request.has_authorize_download()) {
    auto verified = store_->verify_authorization(request.authorize_download());
    return [&, verified = std::move(verified)] {
      *response.mutable_download_authorization() = store_->authorize(verified);
    };
  } else if (request.has_download_authorization()) {
    *response.mutable_download_authorization() =
        store_->authorization(request.download_authorization().id());
  } else if (request.has_download_credentials()) {
    *response.mutable_download_credentials() = store_->credentials(request.download_credentials());
  } else if (request.has_save_dataset()) {
    auto verified = store_->verify_dataset(request.save_dataset());
    creates_dataset = verified.creates_entry();
    return [&, verified = std::move(verified)] {
      store_->save(verified);
      *response.mutable_saved_dataset() = request.save_dataset();
    };
  } else if (request.has_prepare_download()) {
    auto verified = store_->verify_download(request.prepare_download());
    return [&, verified = std::move(verified)] {
      *response.mutable_prepared_download() = store_->prepare(verified);
    };
  } else if (request.has_allocate_download()) {
    auto verified = store_->verify_allocation(request.allocate_download());
    return [&, verified = std::move(verified)] {
      *response.mutable_download_directory() = store_->allocate(verified);
    };
  } else if (request.has_publish_download()) {
    auto verified = store_->verify_publication(request.publish_download());
    publication_dataset = verified.dataset_id();
    return [&, verified = std::move(verified)] {
      *response.mutable_published_download() = store_->publish(verified);
    };
  } else if (request.has_published_download())
    *response.mutable_published_download() = store_->published(request.published_download());
  else if (request.has_history_usage())
    *response.mutable_history_usage() = store_->history_usage(request.history_usage().id());
  else if (request.has_record())
    *response.mutable_record() = store_->archive().get(request.record().id());
  else if (request.has_saved_dataset())
    *response.mutable_saved_dataset() =
        store_->archive().named_dataset(request.saved_dataset().id());
  else if (request.has_saved_datasets())
    *response.mutable_saved_datasets() = store_->archive().named_datasets();
  else if (request.has_datasets()) {
    const auto& filter = request.datasets();
    auto* result = response.mutable_datasets();
    for (const auto& item : store_->archive().datasets(
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
    const auto record = store_->archive().get(query.dataset_id());
    *response.mutable_minute_page() =
        history_files::read_minute_page(record.minutes(), record.minute_result(), query);
  } else if (request.has_daily_page()) {
    const auto& query = request.daily_page();
    if (query.dataset_id().empty() || !query.task_id().empty())
      throw std::invalid_argument("data query requires a published dataset identity");
    const auto record = store_->archive().get(query.dataset_id());
    *response.mutable_daily_page() =
        history_files::read_daily_page(record.daily(), record.daily_result(), query);
  } else if (request.has_bar_dataset())
    *response.mutable_bar_dataset() = resolve_bar_dataset(store_->sources(request.bar_dataset()));
  else if (request.has_daily_factor_dataset())
    *response.mutable_daily_factor_dataset() =
        daily_factor_dataset(store_->archive().get(request.daily_factor_dataset().id()));
  else if (request.has_dominant_series()) {
    if (request.dominant_series().months_size() > 20)
      throw std::invalid_argument("backtest requires 1 to 20 contracts");
    std::vector<BarDatasetSources> months;
    for (const auto& month : request.dominant_series().months())
      months.push_back(store_->sources(month));
    auto resolved = resolve_dominant_series(months);
    auto* preview = response.mutable_dominant_series();
    for (const auto index : resolved.months)
      preview->add_months(static_cast<unsigned>(index));
    for (auto& dataset : resolved.datasets)
      *preview->add_datasets() = std::move(dataset);
    *preview->mutable_schedule() = std::move(resolved.schedule);
  } else if (request.has_coverage())
    *response.mutable_coverage() = history_coverage(store_->archive(), request.coverage());
  else if (request.has_update_plan())
    *response.mutable_update_plan() =
        history_update_plan(store_->archive(), request.update_plan(),
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count());
  else if (request.has_verify_connection()) {
    auto* result = response.mutable_connection_verification();
    for (const auto& check :
         history_providers::verify_connection(request.verify_connection().source(),
                                              request.verify_connection().credential(), stop)) {
      auto* row = result->add_checks();
      row->set_scope(check.scope);
      row->set_state(check.state);
    }
  } else if (request.has_catalog()) {
    const auto& query = request.catalog();
    auto* result = response.mutable_catalog();
    result->set_source(query.source());
    for (const auto& item : history_providers::catalog(query.source(), query.credential(),
                                                       query.venue(), query.product(), stop)) {
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
    for (const auto& source : history_providers::sources()) {
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
}
void DataHost::write(const std::shared_ptr<Work>& work) {
  auto commit = std::exchange(work->commit, {});
  work->completion = writer_.submit([work, commit = std::move(commit)](std::stop_token) {
    TraceScope trace(work->request.correlation_id());
    commit();
    work->frame = work->response.SerializeAsString();
  });
}
std::optional<std::string> DataHost::budget_step(const std::shared_ptr<Work>& work) {
  const bool configuring = work->request.has_configure_download_budget();
  try {
    if (!work->completion.valid()) {
      // One configuration is decided and committed at a time. Pending
      // callers retain only their bounded RPC slot; permit reads continue.
      require_ready();
      if (!is_turn(*work))
        return {};
      work->completion = readers_.submit([&, work, configuring](std::stop_token) {
        TraceScope trace(work->request.correlation_id());
        if (configuring)
          *work->response.mutable_download_budget() =
              store_->verify_budget(work->request.configure_download_budget());
        else
          work->authorization =
              store_->download_authorization(work->request.acquire_download_permit());
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
        auto configuration = budgets_->configure(work->response.download_budget().provider_id(),
                                                 input.credential(), input.requests_per_minute());
        work->completion = writer_.submit(
            [&, configuration, trace_id = work->request.correlation_id()](std::stop_token) {
              TraceScope trace(trace_id);
              budgets_->persist(configuration);
            });
        work->budget_configuration = std::move(configuration);
        return {};
      }
      budgets_->committed(*work->budget_configuration);
    } else {
      *work->response.mutable_download_permit() = budgets_->acquire(
          work->authorization.authorization().provider_id(), work->authorization.credential());
    }
  } catch (const std::exception& error) {
    if (work->budget_configuration) {
      budgets_->persistence_failed();
      degraded_ = true;
    }
    fail(work->response, error);
  }
  release(*work);
  protocol::log_rpc_result("data-service", work->request, work->response, false,
                           {{"service_id", configuration_.service}});
  return work->response.SerializeAsString();
}
void DataHost::admit(Work& admitted, bool worker) {
  auto* const work = &admitted;
  const auto& request = work->request;
  if (!initialized_)
    throw Error(ErrorCode::unavailable, "data service is initializing");
  require_ready();
  const bool mutation = request.has_configure_download_budget() ||
                        request.has_acquire_download_permit() || request.has_authorize_download() ||
                        request.has_download_authorization() ||
                        request.has_download_credentials() || request.has_save_dataset() ||
                        request.has_allocate_download() || request.has_prepare_download() ||
                        request.has_publish_download();
  const bool private_operation =
      request.has_allocate_download() || request.has_prepare_download() ||
      request.has_publish_download() || request.has_published_download() ||
      request.has_download_authorization() || request.has_download_credentials() ||
      request.has_acquire_download_permit();
  if (private_operation && !worker)
    throw Error(ErrorCode::permission_denied,
                "download coordination requires the private data channel");
  if (mutation && quiescing_)
    throw Error(ErrorCode::unavailable, "data service is preparing for upgrade");
  const auto attempt = [&](const wire::DownloadIdentity& identity) {
    work->objects.push_back("download/" + sha256_bytes(identity.SerializeAsString()));
  };
  if (request.has_authorize_download())
    work->objects.push_back("authorization/" +
                            sha256_bytes(configuration_.service + "/" +
                                         configuration_.task_instance + "/" +
                                         request.authorize_download().task_id()));
  else if (request.has_download_authorization())
    work->objects.push_back("authorization/" + request.download_authorization().id());
  else if (request.has_allocate_download()) {
    attempt(request.allocate_download().identity());
    work->objects.push_back("authorization/" + request.allocate_download().authorization_id());
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
  const auto history = [&](const std::string& id) { work->objects.push_back("history/" + id); };
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
  work->objects.erase(std::unique(work->objects.begin(), work->objects.end()), work->objects.end());
  for (const auto& key : work->objects)
    object_order_[key].push_back(work);
  work->writes_catalog = request.has_save_dataset() || request.has_publish_download();
  // Enumeration shares the directory publication barrier. Reads of known
  // immutable versions use object identities and remain available while a
  // different version is being committed.
  work->reads_catalog = request.has_history_usage() || request.has_saved_datasets() ||
                        request.has_datasets() || request.has_coverage();
}
service::RpcHost::Reply DataHost::accept(std::string frame, bool worker) {
  auto work = std::make_shared<Work>();
  auto& request = work->request;
  auto& response = work->response;
  try {
    request = parse(frame);
    response = health_response(request);
    if (request.has_heartbeat())
      return [frame = response.SerializeAsString()] { return std::optional<std::string>(frame); };
    response.clear_result();
    admit(*work, worker);
    if (request.has_configure_download_budget() || request.has_acquire_download_permit())
      return [this, work]() -> std::optional<std::string> { return budget_step(work); };
  } catch (const std::exception& error) {
    response.set_version(1);
    response.set_service_id(configuration_.service);
    response.set_correlation_id(request.correlation_id());
    fail(response, error);
    work->frame = response.SerializeAsString();
    work->phase = Work::Phase::complete;
  }
  return [this, work]() -> std::optional<std::string> { return step(work); };
}
std::optional<std::string> DataHost::step(const std::shared_ptr<Work>& work) {
  try {
    if (work->phase == Work::Phase::queued) {
      require_ready();
      if (!is_turn(*work) ||
          (work->reads_catalog && (catalog_writing_ || catalog_waiting_writers_)))
        return {};
      work->completion = readers_.submit([&, work](std::stop_token stop) {
        TraceScope trace(work->request.correlation_id());
        work->commit = prepare(work->request, work->response, stop, work->creates_dataset,
                               work->publication_dataset);
        if (!work->commit)
          work->frame = work->response.SerializeAsString();
      });
      if (work->reads_catalog) {
        ++catalog_readers_;
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
        --catalog_readers_;
      if (work->commit) {
        if (!work->publication_dataset.empty()) {
          auto key = "history/" + work->publication_dataset;
          object_order_[key].push_back(work.get());
          work->objects.push_back(std::move(key));
        }
        work->phase = Work::Phase::prepared;
        if (work->writes_catalog) {
          ++catalog_waiting_writers_;
          work->awaiting_catalog_write = true;
        }
      } else {
        require_ready();
        work->phase = Work::Phase::complete;
      }
    }
    if (work->phase == Work::Phase::prepared) {
      require_ready();
      if (!is_turn(*work) || (work->writes_catalog && (catalog_readers_ || catalog_writing_)))
        return {};
      if (work->creates_dataset && named_dataset_count_ >= 1000)
        throw Error(ErrorCode::resource_exhausted, "saved dataset capacity reached");
      // Quiesce closes admission; accepted preparation may still commit.
      write(work);
      if (work->writes_catalog) {
        --catalog_waiting_writers_;
        work->awaiting_catalog_write = false;
        catalog_writing_ = work->writing_catalog = true;
      }
      work->phase = Work::Phase::writing;
      return {};
    }
    if (work->phase == Work::Phase::writing) {
      if (work->completion.wait_for(0ms) != std::future_status::ready)
        return {};
      work->completion.get();
      if (work->creates_dataset)
        ++named_dataset_count_;
      work->phase = Work::Phase::complete;
    }
  } catch (const std::exception& error) {
    if (work->phase == Work::Phase::writing)
      degraded_ = true;
    fail(work->response, error);
    work->frame = work->response.SerializeAsString();
  }
  release(*work);
  protocol::log_rpc_result("data-service", work->request, work->response, false,
                           {{"service_id", configuration_.service}});
  return std::move(work->frame);
}
service::RpcHost::Reply DataHost::health(const std::string& frame) {
  const auto request = parse(frame);
  if (request.has_quiesce()) {
    quiescing_ = true;
    if (request.quiesce().stop())
      service::request_stop();
  } else if (!request.has_heartbeat())
    throw std::invalid_argument("health channel only accepts heartbeat");
  return [frame = health_response(request).SerializeAsString()] {
    return std::optional<std::string>(frame);
  };
}
bool DataHost::advance(service::RpcHost::Stage stage) {
  if (opening_.valid() && opening_.wait_for(0ms) == std::future_status::ready) {
    opening_.get();
    initialized_ = true;
  }
  if (stage != service::RpcHost::Stage::running)
    quiescing_ = true;
  // All admitted preparations and commits have completed when business
  // replies drained. The I/O owner never waits on a running file operation.
  return stage == service::RpcHost::Stage::stopping_resources && !opening_.valid();
}
} // namespace asterion::data
