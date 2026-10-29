#include "research_host_internal.hpp"

namespace asterion::tasks {

ResearchHost::ResearchHost(Configuration configuration)
    : configuration_(std::move(configuration)),
      process_(unique_process_id()),
      data_(configuration_.discovery_endpoint),
      files_(configuration_.file_workers, 8, service::wake_io_owner),
      computation_(
          configuration_.task_concurrency,
          configuration_.task_concurrency,
          service::wake_io_owner)
{}

void ResearchHost::open()
{
    opening_ = files_.submit([this](std::stop_token) {
        storage_ = std::make_unique<Store>(
            research::task_domain(),
            std::filesystem::absolute(configuration_.directory),
            Identity{configuration_.service});
    });
}

void ResearchHost::collect_metadata()
{
    if (!metadata_work_ || metadata_work_->result.wait_for(0s) != std::future_status::ready)
        return;
    auto work = metadata_work_;
    try {
        work->result.get();
    } catch (...) {
        work->error = std::current_exception();
    }
    work->done = true;
    metadata_work_.reset();
}

void ResearchHost::collect_writes()
{
    for (auto it = writing_.begin(); it != writing_.end();) {
        auto& work = *it->second;
        if (work.result.wait_for(0s) != std::future_status::ready) {
            ++it;
            continue;
        }
        try {
            work.result.get();
            storage_->confirm(work.change);
        } catch (...) {
            work.error = std::current_exception();
            storage_->persistence_failed();
            degraded_ = true;
        }
        work.done = true;
        it = writing_.erase(it);
    }
}

PolledTask<> ResearchHost::wait_for_store(std::string id, Clock::time_point deadline)
{
    co_await PollUntil{[&] {
        return Clock::now() >= deadline || degraded_
            || (!metadata_work_ && !writing_.contains(id)
                && (storage_->is_active(id) || writing_.empty()));
    }};
    if (Clock::now() >= deadline)
        throw Error(ErrorCode::unavailable, "task request admission timed out");
    if (degraded_)
        throw Error(ErrorCode::recovery_required, "task store requires recovery");
}

std::shared_ptr<ResearchHost::Writing> ResearchHost::write(Store::Change change)
{
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

PolledTask<Store::Change> ResearchHost::commit(Store::Change change)
{
    if (!change.needs_write())
        co_return storage_->commit(std::move(change));
    auto work = write(std::move(change));
    co_await PollUntil{[&] {
        return work->done;
    }};
    if (work->error)
        std::rethrow_exception(work->error);
    co_return std::move(work->change);
}

PolledTask<data::proto::DataResponse> ResearchHost::data_call(
    const ResearchDataRoute& route,
    data::proto::DataRequest request,
    std::chrono::milliseconds timeout,
    std::string parent)
{
    request.set_version(1);
    request.set_service_id(route.owner.service_id());
    {
        TraceScope trace(parent);
        request.set_correlation_id(next_correlation_id());
    }
    auto bytes = co_await data_.read(route, request.SerializeAsString(), timeout);
    data::proto::DataResponse response;
    co_await file_work([&] {
        response = protocol::decode_data_response(request, *bytes);
    });
    co_return std::move(response);
}

PolledTask<ResearchHost::Clock::time_point>
ResearchHost::ready(const Call& call, Clock::time_point deadline)
{
    co_await wait_for_store(call.task_id, deadline);
    co_return Clock::now();
}

PolledTask<data::proto::DataResponse>
ResearchHost::data_input(const Call& call, data::proto::DataRequest query)
{
    call.input_in_time();
    const auto remaining =
        std::chrono::ceil<std::chrono::milliseconds>(call.input_deadline - Clock::now());
    if (!call.data_route)
        call.data_route = co_await data_.resolve(
            call.request.submit().data_owner(),
            remaining,
            call.request.correlation_id());
    call.input_in_time();
    const auto read_timeout =
        std::chrono::ceil<std::chrono::milliseconds>(call.input_deadline - Clock::now());
    auto reply = co_await data_call(
        *call.data_route,
        std::move(query),
        read_timeout,
        call.request.correlation_id());
    call.input_in_time();
    co_return reply;
}

void ResearchHost::quiesce(Call& call)
{
    if (call.lane != Lane::health)
        throw Error(
            ErrorCode::permission_denied,
            "upgrade control requires the private health channel");
    quiescing_ = true;
    if (call.request.quiesce().stop()) {
        const auto waiting = [] {
            return Error(ErrorCode::unavailable, "upgrade is waiting for running task tasks");
        };
        if (!initialized_ || metadata_work_ || !writing_.empty() || !executions_.empty()
            || storage_->has_submission())
            throw waiting();
        for (const auto& task : storage_->active_tasks())
            if (task.state() == wire::RUNNING || task.state() == wire::CANCEL_REQUESTED
                || task.state() == wire::PUBLISHING)
                throw waiting();
        service::request_stop();
    }
    auto* health = call.response.mutable_health();
    health->set_instance_id(configuration_.service);
    health->set_process_id(process_);
}

PolledTask<> ResearchHost::history_usage(Call& call)
{
    const auto& id = call.request.history_usage().id();
    auto slot = input_slots_.acquire(false, id);
    auto read = storage_->prepare_history_usage(id);
    while (storage_->next_history_page(read)) {
        co_await file_work([&] {
            read.load_page();
        });
        co_await ready(call);
    }
    *call.response.mutable_history_usage() = read.take();
}

PolledTask<> ResearchHost::resources_operation(Call& call)
{
    const auto& request = call.request;
    auto& response = call.response;
    // A catalogue kind names the module that owns those resources.
    const auto& kind = request.resources().kind();
    if (kind == "strategy_package" || kind == "strategy_delivery")
        co_return co_await strategy_operation(call);
    if (kind == "logic_definition" || kind == "logic_case")
        co_return co_await logic_operation(call);
    if (kind == "research_variable")
        co_return co_await variable_operation(call);
    const auto node = configuration_.node_id;
    co_await ready(call);
    call.input_in_time();
    if (request.resources().kind() == "research_program") {
        const auto limit = protocol::catalogue_limit(request.resources());
        resource::proto::Catalogue catalogue;
        auto& owner = *catalogue.mutable_owner();
        owner.set_node_id(node);
        owner.set_service_id(configuration_.service);
        owner.set_domain("research");
        catalogue.set_kind("research_program");
        co_await file_work([&] {
            programs::Files programs(configuration_.directory / "programs", owner);
            const auto page = programs.catalogue(request.resources().after(), limit);
            for (const auto& entry : page) {
                auto* row = catalogue.add_items();
                auto* object = row->mutable_object();
                *object->mutable_owner() = owner;
                object->set_kind("research_program");
                object->set_id(entry.digest);
                object->set_version(entry.digest);
                row->set_state(resource::proto::CatalogueEntry::SEALED);
                row->set_logical_bytes(entry.bytes);
                row->set_relative_location(
                    (std::filesystem::path("programs") / entry.digest).generic_string());
            }
            if (page.size() == limit && !programs.catalogue(page.back().digest, 1).empty())
                catalogue.set_next_after(page.back().digest);
        });
        *response.mutable_resources() = std::move(catalogue);
    } else
        *response.mutable_resources() =
            research::task_resources(*storage_, node, request.resources());
}

PolledTask<> ResearchHost::submit_operation(Call& call)
{
    using Definition = research::proto::ResearchSubmit::DefinitionCase;
    switch (call.request.submit().definition_case()) {
        case Definition::kBacktest:
            co_return co_await submit_backtest(call);
        case Definition::kBacktestReuse:
            co_return co_await reuse_backtest(call);
        case Definition::kFactorRequest:
            co_return co_await submit_factor(call);
        case Definition::kStatistics:
            co_return co_await submit_statistics(call);
        case Definition::kEvaluation:
            co_return co_await submit_evaluation(call);
        case Definition::kSample:
            co_return co_await submit_sample(call);
        case Definition::kLogicTrial:
            co_return co_await submit_logic_trial(call);
        case Definition::kAccountReview:
            co_return co_await submit_account_review(call);
        case Definition::DEFINITION_NOT_SET:
            break;
    }
    throw std::invalid_argument("missing task input");
}

PolledTask<> ResearchHost::handle(Call& call, std::string frame)
{
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
        health->set_version(ASTERION_PRODUCT_VERSION);
        health->set_recovery_required(degraded_);
        health->set_initialized(initialized_);
        health->set_uptime_ms(
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started_)
                    .count()));
        co_return;
    }
    if (request.has_quiesce()) {
        quiesce(call);
        co_return;
    }
    if (!initialized_)
        throw Error(ErrorCode::unavailable, "research service is initializing");
    if (call.lane == Lane::health)
        throw std::invalid_argument("health channel only accepts heartbeat");
    call.admitted();
    call.task_id = named_task(request);
    co_await ready(call, call.deadline);
    if (request.has_submit())
        check_submission_shape(request.submit());
    if (request.has_retry())
        check_retry(storage_->describe(request.retry().id()));
    call.admitted();
    if (changes_state(request))
        refuse_during_upgrade(quiescing_);
    std::optional<VerificationSlots::Lease> preparation;
    if (prepares_input(request))
        preparation.emplace(input_slots_.acquire(false, request.correlation_id()));
    using Operation = research::proto::ResearchRequest::OperationCase;
    switch (request.operation_case()) {
        case Operation::kVariable:
            co_await variable_operation(call);
            break;
        case Operation::kLogic:
            co_await logic_operation(call);
            break;
        case Operation::kStrategy:
            co_await strategy_operation(call);
            break;
        case Operation::kReleaseDecision:
            release_decision(call);
            break;
        case Operation::kResources:
            co_await resources_operation(call);
            break;
        case Operation::kRetirement:
            co_await retirement(call);
            break;
        case Operation::kAbandonPreparation:
            co_await abandon_preparation(call);
            break;
        case Operation::kRecoverSubmission:
            co_await recover_submission(call);
            break;
        case Operation::kHistoryUsage:
            co_await history_usage(call);
            break;
        case Operation::kSubmit:
            co_await submit_operation(call);
            break;
        case Operation::kMetadataGet:
            *response.mutable_task() = storage_->describe(request.metadata_get().id());
            break;
        case Operation::kMetadataEdit:
            *response.mutable_task() =
                (co_await commit(storage_->edit_metadata(request.metadata_edit()))).task();
            break;
        case Operation::kGet: {
            auto slot = input_slots_.acquire(false, request.get().id());
            auto read = storage_->prepare_input(request.get().id());
            co_await file_work([&] {
                read.load();
            });
            co_await ready(call);
            call.input_in_time();
            *response.mutable_task() = storage_->confirm_input(std::move(read));
            break;
        }
        case Operation::kList:
            *response.mutable_tasks() =
                storage_->list(request.list().limit(), request.list().before_sequence());
            break;
        case Operation::kCancel:
            *response.mutable_task() =
                (co_await commit(storage_->cancel(request.cancel().id()))).task();
            break;
        case Operation::kRetry:
            *response.mutable_task() =
                (co_await commit(storage_->retry(request.retry().id()))).task();
            break;
        case Operation::kResult: {
            const auto& id = request.result().id();
            auto slot = verification_slots_.acquire(false, id);
            auto read = storage_->prepare_result(id);
            co_await file_work([&] {
                read.verify();
            });
            co_await ready(call);
            call.verified_in_time();
            response = storage_->confirm_result(std::move(read));
            response.set_version(1);
            response.set_service_id(configuration_.service);
            response.set_correlation_id(request.correlation_id());
            break;
        }
        case Operation::kHeartbeat:
        case Operation::kQuiesce:
        case Operation::OPERATION_NOT_SET:
            throw std::invalid_argument("missing task operation");
    }
    if (response.has_task() && !request.has_get())
        response.mutable_task()->clear_definition();
}

PolledTask<wire::TaskResponse>
ResearchHost::respond(std::string frame, Lane lane, Clock::time_point deadline)
{
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
    } catch (const std::exception& error) {
        // Persistence failures mark the service degraded where they happen; any
        // other failure belongs to this request and must not stop the service.
        response.mutable_error()->set_code(std::string(error_name(ErrorCode::internal_error)));
        response.mutable_error()->set_message(error.what());
    }
    std::string task;
    std::optional<std::uint32_t> attempt;
    if (response.has_task()) {
        task = response.task().id();
        attempt = response.task().attempt();

    } else if (response.has_result_task()) {
        task = response.result_task().id();
        attempt = response.result_task().attempt();
    } else if (request.has_submit())
        task = request.submit().id();
    else if (request.has_retry())
        task = request.retry().id();
    else
        task = named_task(request);
    protocol::log_rpc_result(
        "research-service",
        request,
        response,
        request.has_list() || request.has_heartbeat(),
        {{"service_id", configuration_.service}, {"task_id", task}},
        attempt);
    co_return std::move(response);
}

PolledTask<std::string> ResearchHost::serve(std::string frame, Lane lane)
{
    auto response = co_await respond(std::move(frame), lane, Clock::now() + 10s);
    if (!response.has_result_task()
        && (!response.has_task()
            || response.task().definition_case() == wire::Task::DEFINITION_NOT_SET))
        co_return response.SerializeAsString();
    std::string reply;
    co_await file_work([&] {
        reply = response.SerializeAsString();
    });
    co_return reply;
}

service::RpcHost::Reply ResearchHost::accept(std::string frame, Lane lane)
{
    auto operation = std::make_shared<PolledTask<std::string>>(serve(std::move(frame), lane));
    return [operation]() -> std::optional<service::RpcHost::Message> {
        if (!operation->poll())
            return {};
        return operation->take();
    };
}

bool ResearchHost::advance(service::RpcHost::Stage stage)
{
    using Stage = service::RpcHost::Stage;
    if (opening_.valid() && opening_.wait_for(0s) == std::future_status::ready) {
        opening_.get();
        initialized_ = true;
    }
    if (stage != Stage::running)
        quiescing_ = true;
    data_.poll();
    if (initialized_) {
        collect_metadata();
        collect_writes();
        // Metadata uses the same SQLite connection. Calculations keep running
        // on their pool, but state-owner database access waits for this job.
        if (metadata_work_)
            return false;
        if (!scheduling_ && !degraded_ && !quiescing_ && stage == Stage::running
            && Clock::now() >= next_schedule_)
            scheduling_.emplace(schedule_tasks());
        if (scheduling_ && scheduling_->poll()) {
            try {
                scheduling_->take();
            } catch (const Error& error) {
                if (error.code() != ErrorCode::unavailable
                    && error.code() != ErrorCode::resource_exhausted)
                    throw;
            }
            scheduling_.reset();
            next_schedule_ = Clock::now() + 1s;
        }
        for (auto it = executions_.begin(); it != executions_.end();) {
            if (!it->second.poll()) {
                ++it;
                continue;
            }
            try {
                it->second.take();
            } catch (const std::exception& error) {
                degraded_ = true;
                log::error("task {} failed: {}", it->first, error.what());
            }
            it = executions_.erase(it);
        }
    }
    // Accepted work retains its coroutine frame through all file and RPC waits,
    // even after a client disconnect. Never destroy locals still used by a pool.
    return stage == Stage::stopping_resources && !opening_.valid() && writing_.empty()
        && !scheduling_ && executions_.empty();
}

} // namespace asterion::tasks
