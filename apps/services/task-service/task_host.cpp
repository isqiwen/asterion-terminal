#include "task_host.hpp"
#include <asterion/kernel/logger.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/trace.hpp>
#include <asterion/protocol/data.hpp>
#include <asterion/protocol/data_client.hpp>
#include <asterion/protocol/factor.hpp>
#include <asterion/protocol/rpc_diagnostics.hpp>
#include <algorithm>
#include <stdexcept>
namespace asterion::tasks {
using namespace std::chrono_literals;
namespace wire = task::v1;
namespace {
// The existing task an operation names. Submissions and retries are admitted
// by the store itself and wait for no earlier change of their own.
std::string named_task(const wire::TaskRequest& request) {
  if (request.has_get())
    return request.get().id();
  if (request.has_cancel())
    return request.cancel().id();
  if (request.has_result())
    return request.result().id();
  if (request.has_claim())
    return request.claim().id();
  if (request.has_progress())
    return request.progress().id();
  if (request.has_finish())
    return request.finish().id();
  if (request.has_fail())
    return request.fail().id();
  if (request.has_cancel_ack())
    return request.cancel_ack().id();
  return {};
}
// The attempt token of a worker report.
const std::string* report_token(const wire::TaskRequest& request) {
  if (request.has_progress())
    return &request.progress().token();
  if (request.has_finish())
    return &request.finish().token();
  if (request.has_fail())
    return &request.fail().token();
  if (request.has_cancel_ack())
    return &request.cancel_ack().token();
  return nullptr;
}
// New work is not admitted once an upgrade has asked the service to quiesce.
void refuse_during_upgrade(bool quiescing) {
  if (quiescing)
    throw Error(ErrorCode::unavailable, "task service is preparing for upgrade");
}
} // namespace
struct TaskHost::Call {
  wire::TaskRequest request;
  wire::TaskResponse response;
  Lane lane = Lane::client;
  Clock::time_point deadline, verification_deadline, input_deadline;
  std::string task_id;
  const std::string* report_token = nullptr;
  void admitted() const {
    if ((lane != Lane::health && service::stop_requested()) || Clock::now() >= deadline)
      throw Error(ErrorCode::unavailable, "task request admission timed out");
  }
  void input_in_time() const {
    if (service::stop_requested() || Clock::now() >= input_deadline)
      throw Error(ErrorCode::unavailable, "task input preparation timed out");
  }
  void verified_in_time() const {
    if (service::stop_requested() || Clock::now() >= verification_deadline)
      throw Error(ErrorCode::unavailable, "task result verification timed out");
  }
};
TaskHost::TaskHost(Configuration configuration)
    : configuration_(std::move(configuration)), process_(unique_process_id()),
      data_(configuration_.data_endpoint, 8, PayloadBudget{128 * 1024 * 1024}),
      files_(configuration_.file_workers, 8, service::wake_io_owner) {}
void TaskHost::open() {
  opening_ = files_.submit([this](std::stop_token) {
    storage_ =
        std::make_unique<Store>(std::filesystem::absolute(configuration_.directory),
                                Identity{configuration_.service, configuration_.data_instance});
  });
}
void TaskHost::collect_writes() {
  for (auto it = writing_.begin(); it != writing_.end();) {
    auto& work = *it->second;
    if (work.result.wait_for(0s) != std::future_status::ready) {
      ++it;
      continue;
    }
    try {
      work.result.get();
      storage_->confirm(work.change);
      const auto state = work.change.task().state();
      if (state != wire::RUNNING && state != wire::CANCEL_REQUESTED)
        leases_.erase(it->first);
    } catch (...) {
      work.error = std::current_exception();
      storage_->persistence_failed();
      degraded_ = true;
    }
    work.done = true;
    it = writing_.erase(it);
  }
}
PolledTask<> TaskHost::wait_for_store(std::string id) {
  co_await PollUntil{[&] {
    return degraded_ || (!writing_.contains(id) && (storage_->is_active(id) || writing_.empty()));
  }};
  if (degraded_)
    throw Error(ErrorCode::recovery_required, "task store requires recovery");
}
std::shared_ptr<TaskHost::Writing> TaskHost::write(Store::Change change) {
  auto work = std::make_shared<Writing>(Writing{std::move(change), {}, {}, false});
  const auto id = work->change.task().id();
  if (!writing_.emplace(id, work).second)
    throw std::logic_error("task already has a pending change");
  try {
    work->result = journal_.submit([this, work](std::stop_token) {
      if (journal_failed_)
        throw std::runtime_error("task store requires recovery");
      try {
        work->change.persist();
      } catch (...) {
        journal_failed_ = true;
        throw;
      }
    });
  } catch (...) {
    writing_.erase(id);
    throw;
  }
  return work;
}
PolledTask<Store::Change> TaskHost::commit(Store::Change change) {
  if (!change.needs_write())
    co_return storage_->commit(std::move(change));
  auto work = write(std::move(change));
  co_await PollUntil{[&] { return work->done; }};
  if (work->error)
    std::rethrow_exception(work->error);
  co_return std::move(work->change);
}
PolledTask<data::v1::DataResponse> TaskHost::data_call(data::v1::DataRequest request,
                                                       std::chrono::milliseconds timeout,
                                                       std::string parent) {
  request.set_version(1);
  request.set_service_id(configuration_.data_instance);
  {
    TraceScope trace(parent);
    request.set_correlation_id(next_correlation_id());
  }
  auto bytes = co_await PollFuture{data_.request(request.SerializeAsString(), timeout)};
  data::v1::DataResponse response;
  co_await file_work([&] { response = protocol::decode_data_response(request, *bytes); });
  co_return std::move(response);
}
PolledTask<> TaskHost::publish() {
  const auto decisions = storage_->pending_publications();
  for (const auto& decision : decisions) {
    try {
      data::v1::DataRequest request;
      *request.mutable_publish_download() = decision;
      const auto response = co_await data_call(std::move(request), 5s);
      co_await wait_for_store(decision.identity().task_id());
      co_await commit(storage_->confirm_publication(response.published_download()));
    } catch (const Error& error) {
      if (error.code() != ErrorCode::unavailable && error.code() != ErrorCode::resource_exhausted)
        throw;
    }
  }
}
void TaskHost::expire_leases() {
  collect_writes();
  if (degraded_)
    return;
  for (auto it = leases_.begin(); it != leases_.end();) {
    if (writing_.size() >= journal_capacity)
      break;
    if (Clock::now() >= it->second.expires && !writing_.contains(it->first)) {
      write(storage_->interrupt(it->first, it->second.token,
                                "worker heartbeat expired; explicit retry required"));
      it = leases_.erase(it);
    } else
      ++it;
  }
}
PolledTask<TaskHost::Clock::time_point> TaskHost::ready(const Call& call) {
  co_await wait_for_store(call.task_id);
  const auto observed = Clock::now();
  if (call.report_token) {
    // Expiration is a state-owner fact, independent of whether its journal
    // record has obtained a writer slot or completed its durable barrier.
    const auto lease = leases_.find(call.task_id);
    if (lease == leases_.end() || lease->second.token != *call.report_token ||
        observed >= lease->second.expires)
      throw std::invalid_argument("stale or inactive task attempt");
  }
  co_return observed;
}
PolledTask<data::v1::DataResponse> TaskHost::data_input(const Call& call,
                                                        data::v1::DataRequest query) {
  call.input_in_time();
  const auto remaining =
      std::chrono::ceil<std::chrono::milliseconds>(call.input_deadline - Clock::now());
  auto reply = co_await data_call(std::move(query), remaining, call.request.correlation_id());
  call.input_in_time();
  co_return reply;
}
template <class F> PolledTask<Store::Change> TaskHost::submit(const Call& call, F make_input) {
  std::optional<Store::Submission> input;
  co_await file_work([&] { input.emplace(make_input()); });
  co_await ready(call);
  call.input_in_time();
  refuse_during_upgrade(quiescing_);
  storage_->admit_submission(*input);
  try {
    co_await file_work([&] { input->prepare_files(); });
    // Admission has reserved the task's capacity and order. Complete
    // that accepted submission even if its client deadline has passed.
    co_await ready(call);
    co_return co_await commit(storage_->register_submission(*input));
  } catch (...) {
    if (storage_->abandon_submission(*input))
      degraded_ = true;
    throw;
  }
}
void TaskHost::quiesce(Call& call) {
  if (call.lane != Lane::health)
    throw Error(ErrorCode::permission_denied,
                "upgrade control requires the private health channel");
  quiescing_ = true;
  if (call.request.quiesce().stop()) {
    const auto waiting = [] {
      return Error(ErrorCode::unavailable, "upgrade is waiting for running task tasks");
    };
    if (!initialized_ || !writing_.empty() || storage_->has_submission())
      throw waiting();
    for (const auto& task : storage_->active_tasks())
      if (task.state() == wire::RUNNING || task.state() == wire::CANCEL_REQUESTED ||
          task.state() == wire::PUBLISHING)
        throw waiting();
    service::request_stop();
  }
  auto* health = call.response.mutable_health();
  health->set_instance_id(configuration_.service);
  health->set_process_id(process_);
  health->set_data_instance(configuration_.data_instance);
}
PolledTask<> TaskHost::history_usage(Call& call) {
  const auto& id = call.request.history_usage().id();
  auto slot = input_slots_.acquire(false, id);
  auto read = storage_->prepare_history_usage(id);
  while (storage_->next_history_page(read)) {
    co_await file_work([&] { read.load_page(); });
    co_await ready(call);
  }
  *call.response.mutable_history_usage() = read.take();
}
PolledTask<> TaskHost::dispatch(Call& call) {
  bool data_available = true;
  const auto tasks = storage_->active_tasks();
  if (std::ranges::any_of(tasks, [](const auto& task) { return task.state() == wire::QUEUED; })) {
    try {
      data::v1::DataRequest heartbeat;
      heartbeat.mutable_heartbeat();
      const auto reply =
          co_await data_call(std::move(heartbeat), 1s, call.request.correlation_id());
      if (!reply.has_health() || reply.health().instance_id() != configuration_.data_instance ||
          reply.health().task_instance() != configuration_.service)
        throw std::invalid_argument("data and task service bindings disagree");
      data_available = reply.health().initialized() && !reply.health().recovery_required();
    } catch (const Error& error) {
      if (error.code() != ErrorCode::unavailable)
        throw;
      data_available = false;
    }
    co_await ready(call);
    call.admitted();
    refuse_during_upgrade(quiescing_);
  }
  *call.response.mutable_launches() = storage_->dispatch(call.request.dispatch(), data_available);
}
PolledTask<> TaskHost::submit_download(Call& call) {
  const auto& p = call.request.submit();
  data::v1::DataRequest query;
  query.mutable_download_authorization()->set_id(p.download_authorization());
  const auto prepared = co_await data_input(call, std::move(query));
  if (prepared.download_authorization().task_id() != p.id())
    throw std::invalid_argument("invalid task download authorization");
  co_await ready(call);
  call.input_in_time();
  refuse_during_upgrade(quiescing_);
  *call.response.mutable_task() = (co_await submit(call, [&] {
                                    return storage_->submission(prepared.download_authorization());
                                  })).task();
}
// Data owns historical resolution; Task owns this exact calculation definition
// and its copied immutable input after acceptance.
PolledTask<> TaskHost::submit_backtest(Call& call) {
  const auto& p = call.request.submit();
  const auto& b = p.backtest();
  if (b.contracts().empty() || b.contracts_size() > 20)
    throw std::invalid_argument("backtest requires 1 to 20 contracts");
  // Months of a dominant series are resolved together by Data.
  std::vector<bool> in_series(static_cast<std::size_t>(b.contracts_size()));
  for (const auto& series : b.series()) {
    if (series.contracts_size() < 2)
      throw std::invalid_argument("a dominant series requires at least two month contracts");
    for (const auto index : series.contracts()) {
      if (index >= in_series.size() || in_series[index])
        throw std::invalid_argument("invalid dominant series contracts");
      in_series[index] = true;
    }
  }
  // The frozen contracts: ordinary ones in request order, then the dominant
  // months of each series.
  std::vector<std::pair<std::size_t, data::v1::BarDataset>> datasets;
  std::size_t total_bars = 0;
  const auto keep = [&](std::size_t index, data::v1::BarDataset dataset) {
    total_bars += static_cast<std::size_t>(dataset.bars_size());
    if (total_bars > protocol::max_dataset_bars)
      throw std::invalid_argument("dataset exceeds 200000 bars; narrow the date range");
    datasets.emplace_back(index, std::move(dataset));
  };
  for (std::size_t c = 0; c < in_series.size(); ++c) {
    if (in_series[c])
      continue;
    data::v1::DataRequest query;
    *query.mutable_bar_dataset() = b.contracts(static_cast<int>(c)).data();
    auto prepared = co_await data_input(call, std::move(query));
    keep(c, std::move(*prepared.mutable_bar_dataset()));
  }
  std::vector<data::v1::DominantSchedule> schedules;
  for (const auto& series : b.series()) {
    data::v1::DataRequest query;
    for (const auto index : series.contracts())
      *query.mutable_dominant_series()->add_months() = b.contracts(index).data();
    auto prepared = co_await data_input(call, std::move(query));
    auto& resolved = *prepared.mutable_dominant_series();
    if (resolved.months_size() != resolved.datasets_size())
      throw std::invalid_argument("dominant series response has inconsistent months");
    for (auto& roll : *resolved.mutable_schedule()->mutable_rolls())
      roll.set_contract(roll.contract() + static_cast<unsigned>(datasets.size()));
    for (int m = 0; m < resolved.months_size(); ++m) {
      if (resolved.months(m) >= static_cast<unsigned>(series.contracts_size()))
        throw std::invalid_argument("dominant series response has an unknown month");
      keep(series.contracts(resolved.months(m)), std::move(*resolved.mutable_datasets(m)));
    }
    schedules.push_back(std::move(*resolved.mutable_schedule()));
  }
  backtest::v1::BacktestInput input;
  input.set_version(9);
  auto* paper = input.mutable_paper();
  *paper->mutable_deposit() = b.deposit();
  *paper->mutable_risk() = b.risk();
  for (auto& [index, dataset] : datasets) {
    auto* contract = paper->add_contracts();
    *contract->mutable_dataset() = std::move(dataset);
    *contract->mutable_cost_schedule() = b.contracts(static_cast<int>(index)).cost_schedule();
  }
  for (auto& schedule : schedules)
    *input.add_series() = std::move(schedule);
  *input.mutable_strategies() = b.strategies();
  protocol::set_backtest_holdout(input, b.holdout_from());
  co_await ready(call);
  call.input_in_time();
  refuse_during_upgrade(quiescing_);
  *call.response.mutable_task() =
      (co_await submit(call, [&] {
        input.set_dataset_revision(protocol::dataset_revision(input.paper()));
        return storage_->submission(p.id(), std::move(input));
      })).task();
}
PolledTask<> TaskHost::submit_factor(Call& call) {
  const auto& p = call.request.submit();
  const auto& f = p.factor_request();
  auto input = protocol::factor_input(f);
  for (const auto& source : f.series()) {
    auto prepared = co_await data_input(call, protocol::factor_series_query(source));
    protocol::add_factor_series(input, protocol::factor_series(source, std::move(prepared)));
  }
  co_await ready(call);
  call.input_in_time();
  refuse_during_upgrade(quiescing_);
  *call.response.mutable_task() =
      (co_await submit(call, [&] {
        // Hashing a daily series is file-worker work, like the rest of the submission.
        input.set_dataset_revision(protocol::factor_revision(input.series()));
        return storage_->submission(p.id(), std::move(input));
      })).task();
}
PolledTask<> TaskHost::claim(Call& call) {
  const auto& request = call.request;
  const auto& id = request.claim().id();
  if (storage_->describe(id).kind() != request.claim().kind())
    throw std::invalid_argument("worker kind does not match task");
  // Load before committing an attempt. Slow file parsing must not consume an
  // active lease or block unrelated worker controls.
  auto slot = input_slots_.acquire(true, id);
  auto read = storage_->prepare_input(id);
  co_await file_work([&] { read.load_for_claim(); });
  co_await ready(call);
  call.input_in_time();
  if (call.lane != Lane::worker)
    refuse_during_upgrade(quiescing_);
  auto* attempt = call.response.mutable_attempt();
  const auto token = (co_await commit(storage_->claim(read))).token();
  std::exception_ptr allocation_error;
  try {
    *attempt = storage_->confirm_attempt(std::move(read));
    attempt->set_token(token);
    attempt->set_data_endpoint(configuration_.data_endpoint);
    attempt->set_data_instance(configuration_.data_instance);
    if (attempt->task().has_minutes() || attempt->task().has_daily()) {
      data::v1::DataRequest allocation;
      auto* download = allocation.mutable_allocate_download();
      download->set_authorization_id(attempt->task().download_authorization());
      auto* identity = download->mutable_identity();
      identity->set_data_instance(configuration_.data_instance);
      identity->set_task_instance(configuration_.service);
      identity->set_task_id(attempt->task().id());
      identity->set_attempt(attempt->task().attempt());
      if (attempt->task().has_daily())
        *download->mutable_daily() = attempt->task().daily();
      else
        *download->mutable_minutes() = attempt->task().minutes();
      const auto allocated =
          co_await data_call(std::move(allocation), 10s, request.correlation_id());
      co_await ready(call);
      if (storage_->describe(id).state() != wire::RUNNING)
        throw Error(ErrorCode::cancelled, "download cancelled before allocation completed");
      attempt->set_output_directory(allocated.download_directory().directory());
    }
    leases_[id] = {attempt->token(), Clock::now() + configuration_.worker_timeout};
  } catch (...) {
    allocation_error = std::current_exception();
  }
  if (allocation_error) {
    co_await wait_for_store(id);
    std::string reason;
    try {
      std::rethrow_exception(allocation_error);
    } catch (const std::exception& error) {
      reason = error.what();
    }
    co_await commit(storage_->fail(id, token, reason));
    std::rethrow_exception(allocation_error);
  }
}
PolledTask<> TaskHost::finish(Call& call) {
  const auto& p = call.request.finish();
  auto slot = verification_slots_.acquire(true, p.id());
  auto completion = storage_->prepare_finish(p);
  co_await file_work([&] { completion.prepare_payload(); });
  co_await ready(call);
  call.verified_in_time();
  expire_leases();
  co_await ready(call);
  if ((p.has_minutes() || p.has_daily()) &&
      storage_->describe(p.id()).state() != wire::CANCEL_REQUESTED) {
    data::v1::DataRequest preparation;
    *preparation.mutable_prepare_download() = completion.download_preparation();
    const auto prepared = co_await data_call(
        std::move(preparation), protocol::task_verification_timeout, call.request.correlation_id());
    co_await ready(call);
    call.verified_in_time();
    expire_leases();
    co_await ready(call);
    auto applied = co_await commit(
        storage_->prepare_publication(std::move(completion), prepared.prepared_download()));
    *call.response.mutable_task() = applied.task();
    if (applied.task().state() == wire::PUBLISHING)
      publication_wake_ = true;
  } else
    *call.response.mutable_task() =
        (co_await commit(storage_->finish(std::move(completion)))).task();
}
PolledTask<> TaskHost::handle(Call& call, std::string frame) {
  auto& request = call.request;
  auto& response = call.response;
  call.admitted();
  const auto parse = [&] {
    if (!request.ParseFromString(frame))
      throw std::invalid_argument("invalid task Protobuf");
    protocol::validate_message(request);
  };
  if (frame.size() > 65536)
    co_await file_work(parse);
  else
    parse();
  std::string{}.swap(frame);
  response.set_correlation_id(request.correlation_id());
  validate_id(request.correlation_id());
  if (request.version() != 1 || request.service_id() != configuration_.service)
    throw std::invalid_argument("task protocol version or service mismatch");
  if (request.has_heartbeat()) {
    auto* health = response.mutable_health();
    health->set_instance_id(configuration_.service);
    health->set_process_id(process_);
    health->set_data_instance(configuration_.data_instance);
    health->set_version(ASTERION_PRODUCT_VERSION);
    health->set_recovery_required(degraded_);
    health->set_initialized(initialized_);
    health->set_uptime_ms(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started_).count()));
    co_return;
  }
  if (request.has_quiesce()) {
    quiesce(call);
    co_return;
  }
  if (!initialized_)
    throw Error(ErrorCode::unavailable, "task service is initializing");
  if (call.lane == Lane::health)
    throw std::invalid_argument("health channel only accepts heartbeat");
  if (request.has_dispatch() && call.lane != Lane::worker)
    throw Error(ErrorCode::permission_denied, "task dispatch requires the private worker channel");
  call.admitted();
  expire_leases();
  call.admitted();
  call.task_id = named_task(request);
  call.report_token = report_token(request);
  co_await ready(call);
  call.admitted();
  if (request.has_submit() || request.has_retry() || request.has_dispatch() ||
      (call.lane != Lane::worker && request.has_claim()))
    refuse_during_upgrade(quiescing_);
  std::optional<VerificationSlots::Lease> preparation;
  if (request.has_submit() &&
      (request.submit().has_backtest() || request.submit().has_factor_request() ||
       request.submit().has_download_authorization()))
    preparation.emplace(input_slots_.acquire(false, request.correlation_id()));
  if (request.has_history_usage())
    co_await history_usage(call);
  else if (request.has_dispatch())
    co_await dispatch(call);
  else if (request.has_submit()) {
    const auto& p = request.submit();
    if (p.has_download_authorization())
      co_await submit_download(call);
    else if (p.has_backtest())
      co_await submit_backtest(call);
    else if (p.has_factor_request())
      co_await submit_factor(call);
    else
      throw std::invalid_argument("missing task input");
  } else if (request.has_get()) {
    auto slot = input_slots_.acquire(false, request.get().id());
    auto read = storage_->prepare_input(request.get().id());
    co_await file_work([&] { read.load(); });
    co_await ready(call);
    call.input_in_time();
    *response.mutable_task() = storage_->confirm_input(std::move(read));
  } else if (request.has_list())
    *response.mutable_tasks() =
        storage_->list(request.list().limit(), request.list().before_sequence());
  else if (request.has_cancel())
    *response.mutable_task() = (co_await commit(storage_->cancel(request.cancel().id()))).task();
  else if (request.has_retry())
    *response.mutable_task() = (co_await commit(storage_->retry(request.retry().id()))).task();
  else if (request.has_result()) {
    const auto& id = request.result().id();
    auto slot = verification_slots_.acquire(false, id);
    auto read = storage_->prepare_result(id);
    co_await file_work([&] { read.verify(); });
    co_await ready(call);
    call.verified_in_time();
    response = storage_->confirm_result(std::move(read));
    response.set_version(1);
    response.set_service_id(configuration_.service);
    response.set_correlation_id(request.correlation_id());
  } else if (request.has_claim())
    co_await claim(call);
  else if (request.has_progress()) {
    const auto& p = request.progress();
    // A report waiting behind another durable change cannot revive an expired
    // attempt. Liveness is observed now, not at a later disk acknowledgement.
    expire_leases();
    const auto observed = co_await ready(call);
    auto change = storage_->progress(p.id(), p.token(), p.completed());
    leases_.at(p.id()).expires = observed + configuration_.worker_timeout;
    *response.mutable_task() = (co_await commit(std::move(change))).task();
  } else if (request.has_finish())
    co_await finish(call);
  else if (request.has_fail()) {
    const auto& p = request.fail();
    if (!parse_error_code(p.error_code()))
      throw std::invalid_argument("invalid task failure error code");
    *response.mutable_task() =
        (co_await commit(storage_->fail(p.id(), p.token(), p.error()))).task();
  } else if (request.has_cancel_ack()) {
    const auto& p = request.cancel_ack();
    *response.mutable_task() =
        (co_await commit(storage_->acknowledge_cancel(p.id(), p.token()))).task();
  } else
    throw std::invalid_argument("missing task operation");
  if (response.has_task() && !request.has_get())
    response.mutable_task()->clear_definition();
}
PolledTask<wire::TaskResponse> TaskHost::respond(std::string frame, Lane lane,
                                                 Clock::time_point deadline) {
  Call call;
  call.lane = lane;
  call.deadline = deadline;
  call.verification_deadline = Clock::now() + protocol::task_verification_timeout;
  call.input_deadline = Clock::now() + protocol::task_input_timeout;
  const auto& request = call.request;
  auto& response = call.response;
  response.set_version(1);
  response.set_service_id(configuration_.service);
  try {
    co_await handle(call, std::move(frame));
  } catch (const Error& error) {
    response.mutable_error()->set_code(std::string(error_name(error.code())));
    response.mutable_error()->set_message(error.what());
  } catch (const std::invalid_argument& error) {
    response.mutable_error()->set_code("invalid_request");
    response.mutable_error()->set_message(error.what());
  } catch (const std::out_of_range&) {
    response.mutable_error()->set_code("not_found");
    response.mutable_error()->set_message("unknown task");
  } catch (const std::exception& error) {
    degraded_ = true;
    response.mutable_error()->set_code("recovery_required");
    response.mutable_error()->set_message(error.what());
  }
  std::string task;
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
  else if (request.has_retry())
    task = request.retry().id();
  else
    task = named_task(request);
  protocol::log_rpc_result("task-service", request, response,
                           request.has_list() || request.has_dispatch() || request.has_progress() ||
                               request.has_heartbeat(),
                           {{"service_id", configuration_.service}, {"task_id", task}}, attempt);
  co_return std::move(response);
}
PolledTask<std::string> TaskHost::serve(std::string frame, Lane lane) {
  auto response = co_await respond(std::move(frame), lane, Clock::now() + 10s);
  if (!response.has_result_task() && !response.has_attempt() &&
      (!response.has_task() || response.task().definition_case() == wire::Task::DEFINITION_NOT_SET))
    co_return response.SerializeAsString();
  std::string reply;
  co_await file_work([&] { reply = response.SerializeAsString(); });
  co_return reply;
}
service::RpcHost::Reply TaskHost::accept(std::string frame, Lane lane) {
  auto operation = std::make_shared<PolledTask<std::string>>(serve(std::move(frame), lane));
  return [operation]() -> std::optional<service::RpcHost::Message> {
    if (!operation->poll())
      return {};
    return operation->take();
  };
}
bool TaskHost::advance(service::RpcHost::Stage stage) {
  using Stage = service::RpcHost::Stage;
  if (opening_.valid() && opening_.wait_for(0s) == std::future_status::ready) {
    opening_.get();
    initialized_ = true;
  }
  if (stage != Stage::running)
    quiescing_ = true;
  data_.poll();
  if (initialized_) {
    collect_writes();
    if (stage == Stage::running)
      expire_leases();
    if (!publication_ && !degraded_ && stage == Stage::running &&
        (publication_wake_ || Clock::now() >= next_publication_)) {
      publication_.emplace(publish());
      publication_wake_ = false;
    }
    if (publication_ && publication_->poll()) {
      try {
        publication_->take();
      } catch (const std::exception& error) {
        degraded_ = true;
        log_process_event("task-service", LogLevel::error, "publication.failed",
                          {{"message", error.what()}});
      }
      publication_.reset();
      next_publication_ = Clock::now() + 1s;
    }
  }
  // Accepted work retains its coroutine frame through all file and RPC waits,
  // even after a client disconnect. Never destroy locals still used by a pool.
  return stage == Stage::stopping_resources && !opening_.valid() && writing_.empty() &&
         !publication_;
}
} // namespace asterion::tasks
