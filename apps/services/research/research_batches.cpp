#include "research_host_internal.hpp"

namespace asterion::tasks {

PolledTask<wire::ResearchBatchReply> ResearchHost::batch_metadata(
    const Call& call,
    const resource::proto::Owner& owner,
    bool writer,
    std::function<void(research::Batches&, wire::ResearchBatchReply&)> operation)
{
    co_await wait_for_store({}, call.deadline);
    auto work = std::make_shared<MetadataWork>();
    metadata_work_ = work;
    try {
        work->result = journal_.submit(
            [this, owner, writer, operation = std::move(operation), work](std::stop_token) {
                research::Batches batches(
                    *storage_,
                    owner,
                    writer ? sqlite::Database::Access::writer
                           : sqlite::Database::Access::read_only);
                operation(batches, work->batch);
            });
    } catch (...) {
        metadata_work_.reset();
        throw;
    }
    co_await PollUntil{[&] {
        return work->done;
    }};
    if (work->error)
        std::rethrow_exception(work->error);
    co_return std::move(work->batch);
}

PolledTask<research::proto::BatchAssessmentReply> ResearchHost::assessment_metadata(
    const Call& call,
    const resource::proto::Owner& owner,
    bool writer,
    std::function<void(research::Batches&, research::proto::BatchAssessmentReply&)> operation)
{
    co_await wait_for_store({}, call.deadline);
    auto work = std::make_shared<MetadataWork>();
    metadata_work_ = work;
    try {
        work->result = journal_.submit(
            [this, owner, writer, operation = std::move(operation), work](std::stop_token) {
                research::Batches batches(
                    *storage_,
                    owner,
                    writer ? sqlite::Database::Access::writer
                           : sqlite::Database::Access::read_only);
                operation(batches, work->assessment);
            });
    } catch (...) {
        metadata_work_.reset();
        throw;
    }
    co_await PollUntil{[&] {
        return work->done;
    }};
    if (work->error)
        std::rethrow_exception(work->error);
    co_return std::move(work->assessment);
}

PolledTask<research::proto::BatchAssessmentTask> ResearchHost::assessment_task(
    Call& call,
    const wire::Task& observed,
    const std::string& input_digest)
{
    call.input_in_time();
    if (observed.state() != wire::SUCCEEDED)
        co_return research::capture_batch_assessment_task(observed, input_digest);
    auto slot = verification_slots_.acquire(false, observed.id());
    co_await wait_for_store({}, call.deadline);
    auto read = storage_->prepare_result(observed.id());
    if (read.input_digest() != input_digest)
        throw Error(
            ErrorCode::conflict,
            "batch assessment observation changed; read a new preview");
    co_await file_work([&] {
        read.verify();
    });
    co_await ready(call);
    call.input_in_time();
    const auto verified = storage_->confirm_result(std::move(read));
    research::proto::BatchAssessmentTask result;
    co_await file_work([&] {
        result = research::capture_batch_assessment_task(observed, input_digest, &verified);
    });
    co_return result;
}

PolledTask<research::proto::BatchAssessmentSnapshot>
ResearchHost::assessment_snapshot(Call& call, const resource::proto::Object& object)
{
    auto captured =
        co_await batch_metadata(call, object.owner(), false, [object](auto& batches, auto& reply) {
            *reply.mutable_state() = batches.state(object.id());
            if (protocol::backtest_batch_object(reply.state().definition()).SerializeAsString()
                != object.SerializeAsString())
                throw Error(ErrorCode::conflict, "backtest batch identity mismatch");
        });
    co_await ready(call);
    batch_attach_tasks(captured);
    co_await file_work([&] {
        (void)programs::Files(configuration_.directory / "programs", object.owner())
            .verify(captured.state().definition().program_artifact());
    });
    co_await ready(call);
    const auto original = storage_->describe(captured.state().definition().request().source().id());
    research::proto::BatchAssessmentSnapshot result;
    *result.mutable_batch() = captured.state();
    *result.mutable_source() = co_await assessment_task(
        call,
        original,
        captured.state().definition().source_input_sha256());
    for (const auto& member : captured.members()) {
        const auto receipt = std::ranges::find(
            captured.state().members(),
            member.id(),
            &research::proto::BacktestBatchMemberReceipt::task_id);
        if (receipt == captured.state().members().end())
            throw Error(ErrorCode::conflict, "batch assessment member differs from fixed manifest");
        *result.add_members() = co_await assessment_task(call, member, receipt->input_sha256());
    }
    (void)co_await assessment_metadata(
        call,
        object.owner(),
        false,
        [&result](auto& batches, auto&) {
            batches.verify_assessment_snapshot(result);
        });
    co_return result;
}

PolledTask<> ResearchHost::assessment_operation(Call& call)
{
    const auto request = call.request.assessment();
    protocol::validate_message(request);
    resource::proto::Owner owner;
    const auto node = configuration_.node_id;
    owner.set_node_id(node);
    owner.set_service_id(configuration_.service);
    owner.set_domain("research");
    if (request.has_preview() || request.has_mutation()) {
        const auto& object =
            request.has_preview() ? request.preview() : request.mutation().save().batch();
        validate_id(object.id());
        if (object.owner().SerializeAsString() != owner.SerializeAsString()
            || object.kind() != "backtest_batch")
            throw Error(ErrorCode::conflict, "backtest batch identity mismatch");
        if (request.has_mutation()) {
            validate_id(request.mutation().id());
            protocol::validate_batch_assessment_declaration(request.mutation().save());
            auto replay = co_await assessment_metadata(
                call,
                owner,
                false,
                [request](auto& batches, auto& reply) {
                    if (const auto previous = batches.assessment_receipt(request.mutation()))
                        *reply.mutable_assessment() = *previous;
                });
            if (replay.has_assessment()) {
                *call.response.mutable_assessment() = std::move(replay);
                co_return;
            }
            refuse_during_upgrade(quiescing_);
        }
        auto snapshot = co_await assessment_snapshot(call, object);
        if (request.has_preview()) {
            auto& reply = *call.response.mutable_assessment();
            reply.set_preview_digest(protocol::batch_assessment_snapshot_digest(snapshot));
            *reply.mutable_preview() = std::move(snapshot);
            co_return;
        }
        const auto programme = co_await program(call);
        co_await ready(call);
        call.input_in_time();
        refuse_during_upgrade(quiescing_);
        *call.response.mutable_assessment() = co_await assessment_metadata(
            call,
            owner,
            true,
            [request, snapshot = std::move(snapshot), programme](auto& batches, auto& reply) {
                *reply.mutable_assessment() =
                    batches.save_assessment(request.mutation(), snapshot, programme.artifact);
            });
        co_return;
    }
    if (request.has_read()) {
        *call.response.mutable_assessment() =
            co_await assessment_metadata(call, owner, false, [request](auto& batches, auto& reply) {
                *reply.mutable_assessment() = batches.assessment(request.read());
            });
        co_return;
    }
    if (request.has_list()) {
        const auto& page = request.list();
        *call.response.mutable_assessment() =
            co_await assessment_metadata(call, owner, false, [page](auto& batches, auto& reply) {
                const auto values =
                    batches.assessments(page.batch_id(), page.after(), page.limit());
                for (const auto& value : values)
                    *reply.add_assessments() = value;
                if (values.size() == page.limit()
                    && !batches.assessments(page.batch_id(), values.back().declaration().id(), 1)
                        .empty())
                    reply.set_next_after(values.back().declaration().id());
            });
        co_return;
    }
    throw std::invalid_argument("batch assessment operation required");
}

PolledTask<> ResearchHost::ensure_batches(Call& call, const resource::proto::Owner& owner)
{
    if (batches_recovering_)
        co_await PollUntil{[&] {
            return !batches_recovering_;
        }};
    if (batches_recovered_)
        co_return;
    batches_recovering_ = true;
    try {
        auto active = co_await batch_metadata(call, owner, false, [](auto& batches, auto& reply) {
            for (const auto& value : batches.states("", 1, true))
                *reply.add_states() = value;
        });
        if (!active.states().empty())
            (void)co_await batch_metadata(call, owner, true, [](auto& batches, auto& reply) {
                for (const auto& value : batches.interrupt_running())
                    *reply.add_states() = value;
            });
        batches_recovered_ = true;
        batches_recovering_ = false;
    } catch (...) {
        batches_recovering_ = false;
        throw;
    }
}

void ResearchHost::batch_attach_tasks(wire::ResearchBatchReply& reply)
{
    if (!reply.has_state())
        return;
    research::Batches batches(*storage_, reply.state().definition().owner());
    *reply.mutable_state() = batches.state(reply.state().definition().request().id());
    reply.clear_members();
    for (const auto& task : batches.member_tasks(reply.state()))
        *reply.add_members() = task;
}

bool ResearchHost::batch_admission_open(const Call& call, const std::string& id)
{
    if (!call.request.has_submit() || !call.request.submit().derivation().has_batch())
        return true;
    const auto& object = call.request.submit().derivation().batch();
    research::Batches batches(*storage_, object.owner());
    const auto value = batches.state(object.id());
    const auto started = batch_started_.find(object.id());
    if (value.phase() != research::proto::BATCH_RUNNING || started == batch_started_.end()
        || Clock::now() >= started->second.deadline)
        return false;
    const auto member = std::ranges::find(
        value.members(),
        id,
        &research::proto::BacktestBatchMemberReceipt::task_id);
    if (member == value.members().end())
        throw Error(ErrorCode::conflict, "task is not a member of the fixed batch");
    batches.verify_admission(object, id, member->input_sha256());
    return true;
}

PolledTask<> ResearchHost::batch_operation(Call& call)
{
    resource::proto::Owner owner;
    if (batch_owner_)
        owner = *batch_owner_;
    else {
        const auto node = configuration_.node_id;
        owner.set_domain("research");
        owner.set_node_id(node);
        owner.set_service_id(configuration_.service);
    }
    if (call.request.has_resources()) {
        co_await wait_for_store({}, call.deadline);
        *call.response.mutable_resources() =
            research::Batches(*storage_, owner).resources(call.request.resources());
        co_return; // A catalogue read must not initialize metadata or recover running batches.
    }
    co_await ensure_batches(call, owner);
    batch_owner_ = owner;
    const auto request = call.request.batch();
    protocol::validate_message(request);
    if (request.has_mutation()) {
        protocol::validate_backtest_batch_mutation(request.mutation());
        if (!request.mutation().has_stop())
            refuse_during_upgrade(quiescing_);
        auto replay =
            co_await batch_metadata(call, owner, false, [request](auto& batches, auto& reply) {
                if (const auto previous = batches.receipt(request.mutation()))
                    *reply.mutable_state() = batches.state(previous->definition().request().id());
            });
        if (replay.has_state()) {
            co_await ready(call);
            batch_attach_tasks(replay);
            *call.response.mutable_batch() = std::move(replay);
            co_return;
        }
    }
    std::optional<research::proto::BacktestBatchDefinition> captured;
    std::optional<research::proto::BacktestBatchDefinition> starting;
    if (request.has_mutation() && request.mutation().has_save()) {
        const auto& definition = request.mutation().save();
        auto slot = verification_slots_.acquire(false, definition.source().id());
        co_await wait_for_store({}, call.deadline);
        auto read = storage_->prepare_result(definition.source().id());
        const auto input_digest = read.input_digest();
        co_await file_work([&] {
            read.verify();
        });
        co_await ready(call);
        auto verified = storage_->confirm_result(std::move(read));
        const auto programme = co_await program(call);
        co_await file_work([&] {
            const auto risk = risk_providers::Module::selected();
            captured = research::capture_backtest_batch(
                owner,
                definition,
                verified,
                input_digest,
                programme.artifact,
                risk.artifact(),
                SystemClock{}.utc_now() / 1000000);
            (void)programs::Files(configuration_.directory / "programs", owner)
                .verify(captured->program_artifact());
            (void)risk_providers::Module::pinned(
                configuration_.directory / definition.source().id(),
                captured->risk_artifact());
        });
    }
    if (request.has_mutation() && request.mutation().has_start()) {
        auto original =
            co_await batch_metadata(call, owner, false, [request](auto& batches, auto& reply) {
                *reply.mutable_state() = batches.state(request.mutation().start().id());
            });
        const auto programme = co_await program(call);
        const auto fixed = original.state().definition();
        starting = fixed;
        co_await file_work([&] {
            if (programme.artifact != fixed.program_artifact()
                || risk_providers::Module::selected().artifact() != fixed.risk_artifact())
                throw Error(
                    ErrorCode::conflict,
                    "batch requires its original execution and risk implementations");
        });
    }
    call.input_in_time();
    const auto started = Clock::now();
    bool inserted = false;
    if (starting)
        inserted =
            batch_started_
                .try_emplace(
                    starting->request().id(),
                    BatchTiming{
                        started,
                        started + std::chrono::seconds(starting->request().time_budget_seconds())
                    })
                .second;
    wire::ResearchBatchReply reply;
    try {
        reply = co_await batch_metadata(
            call,
            owner,
            request.has_mutation(),
            [request, captured](auto& batches, auto& reply) {
                if (request.has_mutation()) {
                    const auto accepted =
                        batches.apply(request.mutation(), captured ? &*captured : nullptr);
                    *reply.mutable_state() = batches.state(accepted.definition().request().id());
                } else if (request.has_read())
                    *reply.mutable_state() = batches.state(request.read().id());
                else if (request.has_list()) {
                    const auto& page = request.list();
                    const auto values = batches.states(page.after(), page.limit());
                    for (const auto& value : values)
                        *reply.add_states() = value;
                    if (values.size() == page.limit()
                        && !batches.states(values.back().definition().request().id(), 1).empty())
                        reply.set_next_after(values.back().definition().request().id());
                } else
                    throw std::invalid_argument("research batch operation required");
            });
    } catch (...) {
        if (inserted) {
            try {
                const auto current =
                    research::Batches(*storage_, owner).state(starting->request().id());
                if (current.phase() != research::proto::BATCH_RUNNING
                    && current.phase() != research::proto::BATCH_STOPPING)
                    batch_started_.erase(starting->request().id());
            } catch (...) {
                batch_started_.erase(starting->request().id());
            }
        }
        throw;
    }
    if (request.has_mutation())
        next_schedule_ = Clock::now();
    co_await ready(call);
    batch_attach_tasks(reply);
    *call.response.mutable_batch() = std::move(reply);
}

PolledTask<> ResearchHost::verify_prepared_batch(const Call& call, Store::Submission& input)
{
    if (!call.request.has_submit() || !call.request.submit().derivation().has_batch())
        co_return;
    co_await ready(call);
    const auto id = call.request.submit().id();
    const auto object = call.request.submit().derivation().batch();
    const auto execution = input.execution();
    const auto value = research::Batches(*storage_, object.owner()).state(object.id());
    const auto member = std::ranges::find(
        value.members(),
        id,
        &research::proto::BacktestBatchMemberReceipt::task_id);
    if (member == value.members().end() || member->input_sha256() != execution.input_sha256())
        throw Error(
            ErrorCode::conflict,
            "prepared batch member input differs from its fixed intent");
    if (batch_admission_open(call, id))
        co_return;
    if (storage_->find_task(id))
        co_return;
    Call control;
    control.deadline = control.input_deadline = control.verification_deadline =
        Clock::time_point::max();
    if (value.phase() == research::proto::BATCH_RUNNING) {
        research::proto::BacktestBatchMutation stop;
        stop.set_id(next_correlation_id());
        *stop.mutable_stop()->mutable_batch() = object;
        stop.mutable_stop()->set_reason(
            "Batch wall-clock budget exhausted before member registration");
        (void)co_await batch_metadata(
            control,
            object.owner(),
            true,
            [stop](auto& batches, auto& reply) {
                *reply.mutable_state() = batches.apply(stop);
            });
    }
    std::optional<wire::PreparedTaskSubmission> prepared;
    co_await file_work([&] {
        prepared = storage_->prepared_submission_record(id);
    });
    co_await ready(control);
    resource::proto::Object consumer;
    *consumer.mutable_owner() = object.owner();
    consumer.set_kind("backtest");
    consumer.set_id(id);
    consumer.set_version(execution.input_sha256());
    storage_->release_prepared_reservation(input);
    try {
        auto abandoned = storage_->abandon_preparation(*prepared, consumer);
        (void)co_await commit(std::move(abandoned));
    } catch (...) {
        storage_->persistence_failed();
        degraded_ = true;
        throw;
    }
    throw Error(
        ErrorCode::conflict,
        "batch stopped before task registration; complete preparation was durably abandoned");
}

PolledTask<> ResearchHost::schedule_batches()
{
    if (batch_started_.empty() || !batch_owner_)
        co_return;
    const auto owner = *batch_owner_;
    Call control;
    control.deadline = control.input_deadline = control.verification_deadline =
        Clock::time_point::max();
    control.request.set_version(1);
    control.request.set_service_id(configuration_.service);
    control.request.set_correlation_id(next_correlation_id());
    const bool beginning = batch_cursor_.empty();
    auto page = co_await batch_metadata(
        control,
        owner,
        false,
        [after = batch_cursor_](auto& batches, auto& reply) {
            for (const auto& state : batches.states(after, 200, true))
                *reply.add_states() = state;
        });
    if (page.states().empty()) {
        batch_cursor_.clear();
        if (beginning)
            batch_started_.clear();
        co_return;
    }
    const auto terminal = [](wire::TaskState state) {
        return state == wire::SUCCEEDED || state == wire::FAILED || state == wire::CANCELLED
            || state == wire::INTERRUPTED;
    };
    for (auto value : page.states()) {
        const auto id = value.definition().request().id();
        batch_cursor_ = id;
        const auto timing = batch_started_.find(id);
        std::optional<std::uint64_t> elapsed;
        if (timing != batch_started_.end())
            elapsed =
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    Clock::now() - timing->second.started)
                        .count());
        if (value.phase() == research::proto::BATCH_RUNNING
            && (timing == batch_started_.end() || Clock::now() >= timing->second.deadline)) {
            research::proto::BacktestBatchMutation stop;
            stop.set_id(next_correlation_id());
            *stop.mutable_stop()->mutable_batch() =
                protocol::backtest_batch_object(value.definition());
            stop.mutable_stop()->set_reason(
                timing == batch_started_.end() ? "Batch scheduling ownership unavailable"
                                               : "Batch wall-clock budget exhausted");
            auto stopped =
                co_await batch_metadata(control, owner, true, [stop](auto& batches, auto& reply) {
                    *reply.mutable_state() = batches.apply(stop);
                });
            value = stopped.state();
        }
        bool busy = false, unknown = false;
        const auto members = value.members();
        for (const auto& receipt : members) {
            co_await ready(control);
            std::optional<wire::Task> task = storage_->find_task(receipt.task_id());
            if (task
                && task->metadata().derivation().batch().SerializeAsString()
                    != protocol::backtest_batch_object(value.definition()).SerializeAsString()) {
                if (receipt.admission() == research::proto::BATCH_MEMBER_ADMITTED)
                    throw Error(
                        ErrorCode::conflict,
                        "confirmed batch member identity belongs to another task");
                if (receipt.admission() != research::proto::BATCH_MEMBER_REJECTED) {
                    protocol::proto::Error error;
                    error.set_code("conflict");
                    error.set_message(
                        "Planned member task identity is already owned by an unrelated task");
                    auto refused = co_await batch_metadata(
                        control,
                        owner,
                        true,
                        [id, target = receipt.task_id(), error](auto& batches, auto& reply) {
                            auto current = batches.reject_member(id, target, error);
                            if (current.phase() == research::proto::BATCH_RUNNING) {
                                research::proto::BacktestBatchMutation stop;
                                stop.set_id(next_correlation_id());
                                *stop.mutable_stop()->mutable_batch() =
                                    protocol::backtest_batch_object(current.definition());
                                stop.mutable_stop()->set_reason(
                                    "Planned member identity is occupied; unrelated tasks are preserved");
                                current = batches.apply(stop);
                            }
                            *reply.mutable_state() = current;
                        });
                    value = refused.state();
                }
                continue;
            }
            if (task) {
                if (receipt.admission() == research::proto::BATCH_MEMBER_PENDING) {
                    auto confirmed = co_await batch_metadata(
                        control,
                        owner,
                        true,
                        [id, target = receipt.task_id()](auto& batches, auto& reply) {
                            *reply.mutable_state() = batches.confirm_member(id, target);
                        });
                    value = confirmed.state();
                } else if (receipt.admission() != research::proto::BATCH_MEMBER_ADMITTED) {
                    throw Error(
                        ErrorCode::conflict,
                        "unexpected task occupies an unadmitted batch member identity");
                }
                if (task->metadata().derivation().batch().SerializeAsString()
                    != protocol::backtest_batch_object(value.definition()).SerializeAsString())
                    throw Error(ErrorCode::conflict, "actual task belongs to another batch intent");
                busy = busy || !terminal(task->state());
                if (value.phase() == research::proto::BATCH_STOPPING && !terminal(task->state())) {
                    co_await ready(control);
                    (void)co_await commit(storage_->cancel(receipt.task_id()));
                }
            } else if (receipt.admission() == research::proto::BATCH_MEMBER_ADMITTED) {
                throw Error(ErrorCode::conflict, "confirmed batch member task is missing");
            } else if (receipt.admission() == research::proto::BATCH_MEMBER_PENDING) {
                const auto abandoned = storage_->abandoned_preparation(receipt.task_id());
                if (abandoned && abandoned->version() == receipt.input_sha256()) {
                    protocol::proto::Error error;
                    error.set_code("conflict");
                    error.set_message(
                        "Member preparation was durably abandoned before task registration");
                    auto rejected = co_await batch_metadata(
                        control,
                        owner,
                        true,
                        [id, target = receipt.task_id(), error](auto& batches, auto& reply) {
                            *reply.mutable_state() = batches.reject_member(id, target, error);
                        });
                    value = rejected.state();
                } else
                    unknown = true;
            }
        }
        if (unknown && value.phase() == research::proto::BATCH_RUNNING) {
            research::proto::BacktestBatchMutation stop;
            stop.set_id(next_correlation_id());
            *stop.mutable_stop()->mutable_batch() =
                protocol::backtest_batch_object(value.definition());
            stop.mutable_stop()->set_reason(
                "Member admission is uncertain; verify the original identity before a new batch");
            auto stopped =
                co_await batch_metadata(control, owner, true, [stop](auto& batches, auto& reply) {
                    *reply.mutable_state() = batches.apply(stop);
                });
            value = stopped.state();
        }
        auto finished = co_await batch_metadata(
            control,
            owner,
            true,
            [id, elapsed](auto& batches, auto& reply) {
                *reply.mutable_state() = batches.finish(id, elapsed);
            });
        value = finished.state();
        if (value.phase() != research::proto::BATCH_RUNNING
            && value.phase() != research::proto::BATCH_STOPPING) {
            batch_started_.erase(id);
            continue;
        }
        if (value.phase() != research::proto::BATCH_RUNNING || busy || unknown)
            continue;
        const auto next = std::ranges::find(
            value.members(),
            research::proto::BATCH_MEMBER_WAITING,
            &research::proto::BacktestBatchMemberReceipt::admission);
        if (next == value.members().end())
            continue;
        const auto target = next->task_id();
        Call child;
        child.deadline = Clock::time_point::max();
        child.verification_deadline = Clock::now() + protocol::task_verification_timeout;
        child.input_deadline =
            std::min(Clock::now() + protocol::task_input_timeout, batch_started_.at(id).deadline);
        child.request.set_version(1);
        child.request.set_service_id(configuration_.service);
        child.request.set_correlation_id(next_correlation_id());
        auto& submission = *child.request.mutable_submit();
        submission.set_id(target);
        *submission.mutable_data_owner() = value.definition().data_owner();
        *submission.mutable_backtest_reuse() =
            protocol::backtest_batch_member_reuse(value.definition(), target);
        *submission.mutable_derivation()->mutable_batch() =
            protocol::backtest_batch_object(value.definition());
        std::exception_ptr failure;
        try {
            co_await reuse_backtest(child);
        } catch (...) {
            failure = std::current_exception();
        }
        if (degraded_)
            co_return;
        co_await ready(control);
        bool admitted = false;
        if (const auto found_task = storage_->find_task(target)) {
            const auto& actual = *found_task;
            admitted = actual.metadata().derivation().batch().SerializeAsString()
                == protocol::backtest_batch_object(value.definition()).SerializeAsString();
        }
        if (admitted) {
            (void)co_await batch_metadata(
                control,
                owner,
                true,
                [id, target](auto& batches, auto& reply) {
                    *reply.mutable_state() = batches.confirm_member(id, target);
                });
        } else if (failure) {
            protocol::proto::Error error;
            try {
                std::rethrow_exception(failure);
            } catch (const std::exception& e) {
                error.set_code(std::string(error_name(classify(e))));
                error.set_message(std::string(e.what()).substr(0, 2048));
            }
            if (error.message().empty())
                error.set_message("Batch member admission failed");
            (void)co_await batch_metadata(
                control,
                owner,
                true,
                [id, target, error, exhausted = Clock::now() >= batch_started_.at(id).deadline](
                    auto& batches,
                    auto& reply) {
                    auto current = batches.reject_member(id, target, error);
                    if (current.phase() == research::proto::BATCH_RUNNING) {
                        research::proto::BacktestBatchMutation stop;
                        stop.set_id(next_correlation_id());
                        *stop.mutable_stop()->mutable_batch() =
                            protocol::backtest_batch_object(current.definition());
                        stop.mutable_stop()->set_reason(
                            exhausted
                                ? "Batch wall-clock budget exhausted"
                                : "Member admission failed; remaining scope requires a new batch");
                        current = batches.apply(stop);
                    }
                    *reply.mutable_state() = current;
                });
        } else {
            degraded_ = true;
        }
        // Admit at most one member per turn, then let the existing task pool run.
        co_return;
    }
}

} // namespace asterion::tasks
