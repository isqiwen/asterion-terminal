#include "research_host_internal.hpp"

namespace asterion::tasks {

PolledTask<research::proto::EventSetReviewInput> ResearchHost::event_set_evidence(
    const Call& call,
    const research::proto::EventSetReviewRequest& selected,
    const resource::proto::Owner& owner,
    const std::string& artifact)
{
    protocol::validate_event_set_review_request(selected);
    const auto remaining = [&] {
        call.input_in_time();
        return std::chrono::ceil<std::chrono::milliseconds>(call.input_deadline - Clock::now());
    };
    const auto route = co_await data_.resolve_notice(
        selected.source().owner(),
        std::min(remaining(), 5000ms),
        call.request.correlation_id());
    intelligence::proto::IntelligenceRequest query;
    query.set_version(1);
    query.set_service_id(route.owner.service_id());
    query.set_correlation_id(next_correlation_id());
    *query.mutable_event_set()->mutable_publication() = selected.source();
    const auto raw = co_await data_.read(route, query.SerializeAsString(), remaining());
    research::proto::EventSetReviewInput result;
    co_await file_work([&] {
        intelligence::proto::IntelligenceResponse reply;
        if (!reply.ParseFromString(*raw))
            throw Error(ErrorCode::unavailable, "invalid fixed event set response");
        protocol::validate_message(reply);
        if (reply.version() != query.version() || reply.service_id() != query.service_id()
            || reply.correlation_id() != query.correlation_id())
            throw Error(ErrorCode::conflict, "fixed event set response identity mismatch");
        if (reply.has_error())
            throw_remote_error(reply.error().code(), reply.error().message());
        if (!reply.has_set_publication() || !reply.set_publication().has_published()
            || reply.set_publication().published().reference().SerializeAsString()
                != selected.source().SerializeAsString())
            throw Error(ErrorCode::conflict, "fixed event set differs from selected identity");
        protocol::validate_notice_set_record(reply.set_publication());
        *result.mutable_owner() = owner;
        *result.mutable_request() = selected;
        *result.mutable_publication() = reply.set_publication();
        result.set_program_artifact(artifact);
    });
    data::proto::DataOwner selected_data;
    const auto& data_owner = result.publication().published().storage().content().owner();
    selected_data.set_node_id(data_owner.node_id());
    selected_data.set_service_id(data_owner.service_id());
    const auto data_route = co_await data_.resolve(
        selected_data,
        std::min(remaining(), 5000ms),
        call.request.correlation_id());
    data::proto::DataRequest read;
    *read.mutable_intelligence_set_read() = result.publication().intent().reference();
    const auto saved =
        co_await data_call(data_route, std::move(read), remaining(), call.request.correlation_id());
    *result.mutable_content() = saved.intelligence_set_content();
    for (const auto& member : result.content().payload().members()) {
        data::proto::DataRequest member_read;
        *member_read.mutable_intelligence_read() = member.publication().publication().preparation();
        const auto member_saved = co_await data_call(
            data_route,
            std::move(member_read),
            remaining(),
            call.request.correlation_id());
        research::proto::NoticeReviewRequest request;
        *request.mutable_source() = member.publication().reference();
        request.set_as_of_ms(result.content().payload().request().query().as_of_ms());
        request.set_use_scope(selected.use_scope());
        co_await file_work([&] {
            *result.add_members() = protocol::make_notice_review_input(
                owner,
                request,
                member.publication(),
                member_saved.intelligence_content(),
                artifact);
        });
        if (member.event().has_latest_change()) {
            data::proto::DataRequest change_read;
            *change_read.mutable_intelligence_change_read() =
                member.event().latest_change().intent().reference();
            const auto change_saved = co_await data_call(
                data_route,
                std::move(change_read),
                remaining(),
                call.request.correlation_id());
            *result.add_changes() = change_saved.intelligence_change_content();
        }
    }
    co_await file_work([&] {
        protocol::validate_event_set_review_input(result);
    });
    call.input_in_time();
    co_return result;
}

PolledTask<research::proto::NoticeReviewInput> ResearchHost::notice_evidence(
    const Call& call,
    const research::proto::NoticeReviewRequest& selected,
    const resource::proto::Owner& owner,
    const std::string& artifact)
{
    protocol::validate_notice_review_request(selected);
    const auto route =
        co_await data_.resolve_notice(selected.source().owner(), 5s, call.request.correlation_id());
    intelligence::proto::IntelligenceRequest query;
    query.set_version(1);
    query.set_service_id(route.owner.service_id());
    query.set_correlation_id(next_correlation_id());
    *query.mutable_publication() = selected.source();
    const auto raw = co_await data_.read(route, query.SerializeAsString(), 10s);
    intelligence::proto::NoticePublished publication;
    co_await file_work([&] {
        intelligence::proto::IntelligenceResponse reply;
        if (!reply.ParseFromString(*raw))
            throw Error(ErrorCode::unavailable, "invalid fixed notice publication response");
        protocol::validate_message(reply);
        if (reply.version() != query.version() || reply.service_id() != query.service_id()
            || reply.correlation_id() != query.correlation_id())
            throw Error(ErrorCode::conflict, "fixed notice publication response identity mismatch");
        if (reply.has_error())
            throw_remote_error(reply.error().code(), reply.error().message());
        if (!reply.has_publication()
            || reply.publication().reference().SerializeAsString()
                != selected.source().SerializeAsString())
            throw Error(
                ErrorCode::conflict,
                "fixed notice publication differs from selected identity");
        protocol::validate_notice_published(reply.publication());
        publication = reply.publication();
    });
    call.input_in_time();
    data::proto::DataOwner selected_data;
    const auto& content_owner = publication.publication().storage().content().owner();
    selected_data.set_node_id(content_owner.node_id());
    selected_data.set_service_id(content_owner.service_id());
    const auto data_route =
        co_await data_.resolve(selected_data, 5s, call.request.correlation_id());
    data::proto::DataRequest read;
    *read.mutable_intelligence_read() = publication.publication().preparation();
    const auto saved =
        co_await data_call(data_route, std::move(read), 10s, call.request.correlation_id());
    research::proto::NoticeReviewInput result;
    co_await file_work([&] {
        result = protocol::make_notice_review_input(
            owner,
            selected,
            publication,
            saved.intelligence_content(),
            artifact);
    });
    call.input_in_time();
    co_return result;
}

PolledTask<intelligence::proto::NoticeConsumerRecord> ResearchHost::notice_custody(
    const Call& call,
    const intelligence::proto::NoticeConsumerBinding& binding,
    intelligence::proto::NoticeConsumerRecord::Phase phase)
{
    protocol::validate_notice_consumer_binding(binding);
    const auto route =
        co_await data_.resolve_notice(binding.source().owner(), 5s, call.request.correlation_id());
    intelligence::proto::IntelligenceRequest request;
    request.set_version(1);
    request.set_service_id(route.owner.service_id());
    request.set_correlation_id(next_correlation_id());
    auto& command = *request.mutable_consumer();
    if (phase == intelligence::proto::NoticeConsumerRecord::PREPARED)
        *command.mutable_prepare() = binding;
    else if (phase == intelligence::proto::NoticeConsumerRecord::CONFIRMED)
        *command.mutable_confirm() = binding;
    else if (phase == intelligence::proto::NoticeConsumerRecord::RELEASED)
        *command.mutable_release() = binding.consumer();
    else
        *command.mutable_get() = binding.consumer();
    const auto raw = co_await data_.read(route, request.SerializeAsString(), 8s);
    intelligence::proto::NoticeConsumerRecord record;
    co_await file_work([&] {
        intelligence::proto::IntelligenceResponse response;
        if (!response.ParseFromString(*raw))
            throw Error(ErrorCode::unavailable, "invalid notice custody response");
        protocol::validate_message(response);
        if (response.version() != request.version() || response.service_id() != request.service_id()
            || response.correlation_id() != request.correlation_id())
            throw Error(ErrorCode::conflict, "notice custody response identity differs");
        if (response.has_error())
            throw_remote_error(response.error().code(), response.error().message());
        if (!response.has_consumer())
            throw Error(ErrorCode::unavailable, "notice custody acknowledgement missing");
        record = response.consumer();
        protocol::validate_notice_consumer_record(record);
        if (record.binding().SerializeAsString() != binding.SerializeAsString()
            || (phase == intelligence::proto::NoticeConsumerRecord::PREPARED
                && record.phase() != intelligence::proto::NoticeConsumerRecord::PREPARED
                && record.phase() != intelligence::proto::NoticeConsumerRecord::CONFIRMED)
            || (phase != intelligence::proto::NoticeConsumerRecord::UNSPECIFIED_PHASE
                && phase != intelligence::proto::NoticeConsumerRecord::PREPARED
                && record.phase() != phase))
            throw Error(ErrorCode::conflict, "notice custody fixed relation or phase differs");
    });
    co_return record;
}

PolledTask<> ResearchHost::notice_source_changes(Call& call)
{
    const auto id = call.request.notice_source_changes().id();
    validate_id(id);
    if (storage_->describe(id).kind() != wire::NOTICE_REVIEW)
        throw Error(ErrorCode::conflict, "task does not consume a fixed notice");
    auto read = storage_->prepare_input(id);
    co_await file_work([&] {
        read.load_for_claim();
    });
    const auto binding = notice_consumer_binding(read.execution(), id);
    const auto task = storage_->confirm_input(std::move(read));
    const auto& input = task.notice_review();
    const auto route = co_await data_.resolve_notice(
        input.request().source().owner(),
        5s,
        call.request.correlation_id());
    intelligence::proto::IntelligenceRequest query;
    query.set_version(1);
    query.set_service_id(route.owner.service_id());
    query.set_correlation_id(next_correlation_id());
    query.mutable_get()->set_id(input.publication().publication().candidate().id());
    const auto decode = [](const std::string& bytes, const auto& request) {
        intelligence::proto::IntelligenceResponse reply;
        if (!reply.ParseFromString(bytes))
            throw Error(ErrorCode::unavailable, "invalid notice source observation response");
        protocol::validate_message(reply);
        if (reply.version() != request.version() || reply.service_id() != request.service_id()
            || reply.correlation_id() != request.correlation_id())
            throw Error(ErrorCode::conflict, "notice source observation response identity differs");
        if (reply.has_error())
            throw_remote_error(reply.error().code(), reply.error().message());
        return reply;
    };
    research::proto::NoticeSourceObservation observation;
    *observation.mutable_consumer() = binding.consumer();
    const auto source_bytes = co_await data_.read(route, query.SerializeAsString(), 8s);
    co_await file_work([&] {
        const auto reply = decode(*source_bytes, query);
        if (!reply.has_record())
            throw Error(ErrorCode::conflict, "notice source statement is missing");
        protocol::validate_notice_record(reply.record());
        if (!reply.record().has_event()
            || reply.record().published().SerializeAsString()
                != input.publication().SerializeAsString())
            throw Error(
                ErrorCode::conflict,
                "notice source statement differs from fixed research input");
        *observation.mutable_source() = reply.record();
    });
    call.input_in_time();
    query.clear_operation();
    query.set_correlation_id(next_correlation_id());
    *query.mutable_event_view()->mutable_event() = observation.source().event().reference();
    const auto event_bytes = co_await data_.read(route, query.SerializeAsString(), 8s);
    co_await file_work([&] {
        const auto reply = decode(*event_bytes, query);
        if (!reply.has_event_view())
            throw Error(ErrorCode::conflict, "notice source event view is missing");
        *observation.mutable_current() = reply.event_view();
        observation.set_observed_at_ms(SystemClock{}.utc_now() / 1000000);
        protocol::validate_notice_source_observation(observation);
    });
    call.input_in_time();
    *call.response.mutable_notice_source_observation() = std::move(observation);
}

PolledTask<> ResearchHost::reconcile_notice_custody(Call& call)
{
    const auto id = call.request.reconcile_notice_custody().id();
    validate_id(id);
    wire::TaskExecution execution;
    auto phase = intelligence::proto::NoticeConsumerRecord::PREPARED;
    std::optional<wire::Task> existing = storage_->find_task(id);
    const bool admitted = existing.has_value();
    if (existing) {
        if (existing->kind() != wire::NOTICE_REVIEW && existing->kind() != wire::EVENT_SET_REVIEW)
            throw Error(ErrorCode::conflict, "task does not consume a fixed notice");
        auto read = storage_->prepare_input(id);
        co_await file_work([&] {
            read.load_for_claim();
        });
        execution = read.execution();
        phase = intelligence::proto::NoticeConsumerRecord::CONFIRMED;
    } else {
        if (storage_->abandoned_preparation(id)) {
            execution = storage_->abandoned_preparation_record(id).execution();
            phase = intelligence::proto::NoticeConsumerRecord::RELEASED;
        } else {
            wire::PreparedTaskSubmission prepared;
            co_await file_work([&] {
                prepared = storage_->prepared_submission_record(id);
            });
            execution = prepared.execution();
        }
    }
    const auto binding = notice_consumer_binding(execution, id);
    if (binding.consumer().owner().service_id() != configuration_.service)
        throw Error(ErrorCode::conflict, "notice custody belongs to another Research owner");
    call.input_in_time();
    auto receipt = co_await notice_custody(
        call,
        binding,
        admitted ? intelligence::proto::NoticeConsumerRecord::PREPARED : phase);
    co_await file_work([&] {
        if (std::filesystem::is_directory(configuration_.directory / id))
            save_notice_custody(configuration_.directory / id, receipt);
    });
    if (admitted) {
        receipt = co_await notice_custody(call, binding, phase);
        co_await file_work([&] {
            save_notice_custody(configuration_.directory / id, receipt);
        });
    }
    *call.response.mutable_notice_custody() = receipt;
}

PolledTask<> ResearchHost::submit_notice_review(Call& call)
{
    const auto& submission = call.request.submit();
    const auto& selected = submission.notice_review();
    protocol::validate_notice_review_request(selected);
    validate_id(submission.id());
    if (submission.has_data_owner() || submission.has_derivation())
        throw std::invalid_argument(
            "notice review does not accept Data selection or experiment derivation");
    if (const auto found_task = storage_->find_task(submission.id())) {
        const auto& existing = *found_task;
        if (existing.kind() != wire::NOTICE_REVIEW || existing.has_retirement())
            throw Error(ErrorCode::conflict, "notice review identity is already used or retired");
        auto read = storage_->prepare_input(submission.id());
        co_await file_work([&] {
            read.load_for_claim();
        });
        co_await ready(call);
        if (!read.execution().has_notice_review()
            || read.execution().notice_review().request().SerializeAsString()
                != selected.SerializeAsString())
            throw Error(
                ErrorCode::conflict,
                "notice review identity names different fixed evidence");
        *call.response.mutable_task() = storage_->describe(submission.id());
        co_return; // Replaying accepted identity needs neither source nor current program.
    }
    const auto observed = co_await program(call);
    auto input = co_await notice_evidence(call, selected, observed.owner, observed.artifact);
    *call.response.mutable_task() = (co_await submit(call, [&] {
        return task_submission(*storage_, submission.id(), std::move(input));
    })).task();
}

PolledTask<> ResearchHost::submit_event_set_review(Call& call)
{
    const auto& submission = call.request.submit();
    const auto& selected = submission.event_set_review();
    protocol::validate_event_set_review_request(selected);
    validate_id(submission.id());
    if (submission.has_data_owner() || submission.has_derivation())
        throw std::invalid_argument(
            "event set review does not accept Data selection or experiment derivation");
    if (const auto found_task = storage_->find_task(submission.id())) {
        const auto& existing = *found_task;
        if (existing.kind() != wire::EVENT_SET_REVIEW || existing.has_retirement())
            throw Error(
                ErrorCode::conflict,
                "event set review identity is already used or retired");
        auto read = storage_->prepare_input(submission.id());
        co_await file_work([&] {
            read.load_for_claim();
        });
        co_await ready(call);
        if (!read.execution().has_event_set_review()
            || read.execution().event_set_review().request().SerializeAsString()
                != selected.SerializeAsString())
            throw Error(
                ErrorCode::conflict,
                "event set review identity names different fixed evidence");
        *call.response.mutable_task() = storage_->describe(submission.id());
        co_return; // Replaying accepted identity needs neither source nor current program.
    }
    const auto observed = co_await program(call);
    auto input = co_await event_set_evidence(call, selected, observed.owner, observed.artifact);
    *call.response.mutable_task() = (co_await submit(call, [&] {
        return task_submission(*storage_, submission.id(), std::move(input));
    })).task();
}

} // namespace asterion::tasks
