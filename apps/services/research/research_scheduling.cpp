#include "research_host_internal.hpp"

namespace asterion::tasks {

PolledTask<> ResearchHost::schedule_tasks()
{
    co_await schedule_batches();
    if (degraded_ || quiescing_)
        co_return;
    if (executions_.size() >= configuration_.task_concurrency)
        co_return;
    const auto pending = storage_->active_tasks();
    if (std::ranges::none_of(pending, [](const auto& task) {
            return task.state() == wire::QUEUED;
        }))
        co_return;
    if (quiescing_)
        co_return;
    for (const auto& task : storage_->active_tasks()) {
        if (executions_.size() >= configuration_.task_concurrency)
            break;
        if (task.state() == wire::QUEUED && !executions_.contains(task.id()))
            executions_.emplace(task.id(), run_task(task.id()));
    }
}

PolledTask<> ResearchHost::run_task(std::string id)
{
    Call call;
    call.task_id = id;
    call.deadline = Clock::time_point::max();
    call.input_deadline = Clock::now() + protocol::task_input_timeout;
    call.request.set_correlation_id(next_correlation_id());
    std::exception_ptr failure;
    std::string token;
    unsigned attempt_number = 0;
    try {
        co_await wait_for_store(id);
        const auto before = storage_->describe(id);
        if (before.state() != wire::QUEUED || quiescing_)
            co_return;
        attempt_number = before.attempt() + 1;
        co_await claim(call);
        auto attempt = std::move(call.attempt);
        token = attempt.token();
        log_process_event(
            "research-service",
            LogLevel::info,
            "task.started",
            {{"task_id", id}, {"attempt", attempt_number}});
        std::stop_source cancel;
        std::atomic<unsigned> completed{0};
        wire::TaskFinish result;
        auto execution = computation_.submit(
            [&,
                attempt = std::move(attempt),
                loaded_program = loaded_program_ ? loaded_program_->artifact : std::string{}](
                std::stop_token pool_stop) mutable {
                std::stop_callback stop(pool_stop, [&] {
                    cancel.request_stop();
                });
                result = execute(
                    std::move(attempt),
                    std::filesystem::absolute(configuration_.directory),
                    cancel.get_token(),
                    [&](auto done, auto total) {
                        if (done > total || done > std::numeric_limits<unsigned>::max()
                            || done < completed.load())
                            throw std::invalid_argument("invalid task progress");
                        completed = static_cast<unsigned>(done);
                    },
                    loaded_program);
            });
        // The execution frame and its inputs outlive the pool callback even when
        // persistence fails or the service is stopping.
        std::exception_ptr control_failure;
        while (execution.wait_for(0ms) != std::future_status::ready) {
            const auto next = Clock::now() + 100ms;
            co_await PollUntil{[&] {
                return execution.wait_for(0ms) == std::future_status::ready || Clock::now() >= next;
            }};
            if (service::stop_requested() || degraded_ || control_failure)
                cancel.request_stop();
            if (control_failure || degraded_)
                continue;
            try {
                co_await wait_for_store(id);
                const auto state = storage_->describe(id);
                if (state.state() == wire::CANCEL_REQUESTED)
                    cancel.request_stop();
                if (state.completed() != completed.load())
                    co_await commit(storage_->progress(id, token, completed.load()));
            } catch (...) {
                control_failure = std::current_exception();
                cancel.request_stop();
            }
        }
        execution.get();
        if (control_failure)
            std::rethrow_exception(control_failure);
        co_await wait_for_store(id);
        call.verification_deadline = Clock::now() + protocol::task_verification_timeout;
        co_await finish(call, result);
        log_process_event(
            "research-service",
            LogLevel::info,
            "task.finished",
            {{"task_id", id},
                {"attempt", attempt_number},
                {"state", wire::TaskState_Name(call.response.task().state())}});
    } catch (...) {
        failure = std::current_exception();
    }
    if (failure) {
        std::string message, code;
        try {
            std::rethrow_exception(failure);
        } catch (const std::exception& error) {
            message = error.what();
            code = std::string(error_name(classify(error)));
        }
        co_await wait_for_store(id);
        if (token.empty())
            token = call.attempt.token();
        const auto state = storage_->describe(id);
        if (state.state() == wire::QUEUED && token.empty() && code == "unavailable")
            co_return; // This task waits for its own Data; unrelated owners remain runnable.
        if (state.state() == wire::CANCEL_REQUESTED && !token.empty())
            co_await commit(storage_->acknowledge_cancel(id, token));
        else if (!token.empty() && state.state() == wire::RUNNING)
            co_await commit(
                service::stop_requested()
                    ? storage_->interrupt(id, token, "service stopped during task execution")
                    : storage_->fail(id, token, message, code));
        else if (state.state() == wire::QUEUED && !quiescing_)
            co_await commit(storage_->fail_start(id, attempt_number, message, code));
    }
}

// Data owns historical resolution; Task owns this exact calculation definition
// and its copied immutable input after acceptance.

PolledTask<> ResearchHost::claim(Call& call)
{
    const auto& id = call.task_id;
    if ((storage_->describe(id).kind() != wire::BACKTEST
            && storage_->describe(id).kind() != wire::FACTOR
            && storage_->describe(id).kind() != wire::ACCOUNT_REVIEW
            && storage_->describe(id).kind() != wire::NOTICE_REVIEW
            && storage_->describe(id).kind() != wire::EVENT_SET_REVIEW
            && storage_->describe(id).kind() != wire::STATISTICS
            && storage_->describe(id).kind() != wire::PERFORMANCE_EVALUATION
            && storage_->describe(id).kind() != wire::SAMPLE
            && storage_->describe(id).kind() != wire::LOGIC_TRIAL
            && storage_->describe(id).kind() != wire::MODEL_TRAINING
            && storage_->describe(id).kind() != wire::MODEL_TIME_VALIDATION))
        throw Error(ErrorCode::conflict, "acquisition belongs to the data service");
    // Load before committing an attempt. Slow file parsing must not consume an
    // execution slot while blocking unrelated controls.
    co_await PollUntil{[&] {
        call.input_in_time();
        return input_slots_.completion_available();
    }};
    auto slot = input_slots_.acquire(true, id);
    co_await ready(call);
    auto read = storage_->prepare_input(id);
    co_await file_work([&] {
        read.load_for_claim();
    });
    co_await ready(call);
    call.input_in_time();
    refuse_during_upgrade(quiescing_);
    if (read.execution().has_logic_trial()) {
        co_await wait_for_store({}, call.deadline);
        const auto& fixed = read.execution().logic_trial();
        research::Logic(*storage_, fixed.owner()).verify_input(fixed, false);
    }
    if (read.execution().has_strategy_use()) {
        co_await wait_for_store({}, call.deadline);
        research::Strategies(*storage_, read.execution().strategy_use().package().owner())
            .verify_use(read.execution().strategy_use(), id, false);
    }
    if (read.execution().has_research_plan())
        co_await verify_plan(call, read.execution().research_plan());
    if (read.execution().has_variable_definition()) {
        const auto object = protocol::variable_object(read.execution().variable_definition());
        (void)co_await resolve_variable(call, object);
    }
    if (read.execution().has_event_set_review()) {
        const auto observed = co_await program(call);
        const auto& fixed = read.execution().event_set_review();
        if (fixed.owner().SerializeAsString() != observed.owner.SerializeAsString()
            || fixed.program_artifact() != observed.artifact)
            throw Error(
                ErrorCode::conflict,
                "event set review requires its fixed execution program and owner");
        co_await file_work([&] {
            (void)programs::Files(configuration_.directory / "programs", fixed.owner())
                .verify(fixed.program_artifact());
        });
        co_await ready(call);
        call.input_in_time();
        refuse_during_upgrade(quiescing_);
        const auto token = (co_await commit(storage_->claim(read))).token();
        call.attempt.set_token(token);
        call.attempt = storage_->confirm_attempt(std::move(read));
        call.attempt.set_token(token);
        co_return;
    }
    if (read.execution().has_notice_review()) {
        const auto observed = co_await program(call);
        const auto& fixed = read.execution().notice_review();
        if (fixed.owner().SerializeAsString() != observed.owner.SerializeAsString()
            || fixed.program_artifact() != observed.artifact)
            throw Error(
                ErrorCode::conflict,
                "notice review requires its fixed execution program and owner");
        co_await file_work([&] {
            (void)programs::Files(configuration_.directory / "programs", fixed.owner())
                .verify(fixed.program_artifact());
        });
        co_await ready(call);
        call.input_in_time();
        refuse_during_upgrade(quiescing_);
        const auto token = (co_await commit(storage_->claim(read))).token();
        call.attempt.set_token(token);
        call.attempt = storage_->confirm_attempt(std::move(read));
        call.attempt.set_token(token);
        co_return;
    }
    if (read.execution().has_account_review()) {
        const auto observed = co_await program(call);
        const auto& fixed = read.execution().account_review();
        if (fixed.owner().SerializeAsString() != observed.owner.SerializeAsString()
            || fixed.program_artifact() != observed.artifact)
            throw Error(
                ErrorCode::conflict,
                "account review requires its fixed execution program and owner");
        co_await file_work([&] {
            (void)programs::Files(configuration_.directory / "programs", fixed.owner())
                .verify(fixed.program_artifact());
        });
        co_await ready(call);
        call.input_in_time();
        refuse_during_upgrade(quiescing_);
        const auto token = (co_await commit(storage_->claim(read))).token();
        call.attempt.set_token(token);
        call.attempt = storage_->confirm_attempt(std::move(read));
        call.attempt.set_token(token);
        co_return;
    }
    const auto kind = storage_->describe(id).kind();
    if (kind == wire::BACKTEST || kind == wire::FACTOR || kind == wire::STATISTICS
        || kind == wire::SAMPLE || kind == wire::LOGIC_TRIAL || kind == wire::MODEL_TRAINING
        || kind == wire::MODEL_TIME_VALIDATION || kind == wire::PERFORMANCE_EVALUATION) {
        const auto programme = co_await program(call);
        if (read.execution().program_artifact().empty()
            || read.execution().program_artifact() != programme.artifact)
            throw Error(ErrorCode::conflict, "calculation requires its fixed execution program");
        co_await file_work([&] {
            (void)programs::Files(configuration_.directory / "programs", programme.owner)
                .verify(read.execution().program_artifact());
        });
        co_await ready(call);
        call.input_in_time();
        refuse_during_upgrade(quiescing_);
    }
    if (kind == wire::STATISTICS || kind == wire::SAMPLE || kind == wire::LOGIC_TRIAL
        || kind == wire::MODEL_TRAINING || kind == wire::MODEL_TIME_VALIDATION
        || kind == wire::PERFORMANCE_EVALUATION) {
        const auto token = (co_await commit(storage_->claim(read))).token();
        call.attempt = storage_->confirm_attempt(std::move(read));
        call.attempt.set_token(token);
        co_return; // Source is owned Research evidence; Data availability is irrelevant.
    }
    const auto route = co_await data_.resolve(
        read.execution().data_owner(),
        std::chrono::ceil<std::chrono::milliseconds>(call.input_deadline - Clock::now()),
        call.request.correlation_id());
    data::proto::DataRequest heartbeat;
    heartbeat.mutable_heartbeat();
    const auto observed =
        co_await data_call(route, std::move(heartbeat), 1s, call.request.correlation_id());
    if (!observed.health().initialized() || observed.health().recovery_required())
        throw Error(ErrorCode::unavailable, "task data owner is not ready");
    if (read.execution().has_data_owner()) {
        const auto retention =
            research_retention(read.execution(), route, configuration_.service, id);
        co_await file_work([&] {
            verify_research_retention(configuration_.directory / id, retention);
        });
        data::proto::DataRequest confirm;
        *confirm.mutable_retention()->mutable_confirm() = retention;
        const auto retained =
            co_await data_call(route, std::move(confirm), 5s, call.request.correlation_id());
        if (retained.retention().phase() != data::proto::DataRetentionRecord::CONFIRMED
            || retained.retention().retention().SerializeAsString()
                != retention.SerializeAsString())
            throw std::invalid_argument("task input retention was not confirmed");
    }
    co_await ready(call);
    call.input_in_time();
    refuse_during_upgrade(quiescing_);
    auto* attempt = &call.attempt;
    const auto token = (co_await commit(storage_->claim(read))).token();
    // The claim is already durable even if fixed-input confirmation refuses it.
    // Retain its token so run_task can fail that running attempt exactly once.
    attempt->set_token(token);
    *attempt = storage_->confirm_attempt(std::move(read));
    attempt->set_token(token);
    attempt->set_data_endpoint(route.endpoint);
    attempt->set_data_instance(route.owner.service_id());
}

PolledTask<> ResearchHost::finish(Call& call, const wire::TaskFinish& p)
{
    co_await PollUntil{[&] {
        call.verified_in_time();
        return verification_slots_.completion_available();
    }};
    auto slot = verification_slots_.acquire(true, p.id());
    co_await ready(call);
    auto completion = storage_->prepare_finish(p);
    co_await file_work([&] {
        completion.prepare_payload();
    });
    co_await ready(call);
    call.verified_in_time();
    co_await ready(call);
    *call.response.mutable_task() =
        (co_await commit(storage_->finish(std::move(completion)))).task();
}

} // namespace asterion::tasks
