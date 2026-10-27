#include "research_host_internal.hpp"

namespace asterion::tasks {

PolledTask<> ResearchHost::recover_submission(Call& call)
{
    const auto id = call.request.recover_submission().id();
    validate_id(id);
    if (auto task = storage_->find_task(id)) {
        *call.response.mutable_task() = std::move(*task);
        co_return; // A durable accepted task needs no source connection to replay this request.
    }
    // Only an absent task may consume its owned preparation record.
    if (storage_->abandoned_preparation(id))
        throw Error(ErrorCode::conflict, "preparation was durably abandoned");
    wire::PreparedTaskSubmission prepared;
    co_await file_work([&] {
        prepared = storage_->prepared_submission_record(id);
    });
    call.input_in_time();
    if (prepared.execution().has_strategy_use()) {
        co_await wait_for_store({}, call.deadline);
        research::Strategies(*storage_, prepared.execution().strategy_use().package().owner())
            .verify_use(prepared.execution().strategy_use(), id, true);
    }
    if (prepared.execution().has_variable_definition()) {
        const auto object = protocol::variable_object(prepared.execution().variable_definition());
        (void)co_await resolve_variable(call, object);
    }
    if (prepared.execution().has_account_review()) {
        const auto& fixed = prepared.execution().account_review();
        protocol::validate_account_review_execution(fixed);
        const auto node = configuration_.node_id;
        if (fixed.owner().node_id() != node || fixed.owner().service_id() != configuration_.service)
            throw Error(ErrorCode::conflict, "account review preparation belongs to another owner");
        bool missing = false;
        co_await file_work([&] {
            (void)programs::Files(configuration_.directory / "programs", fixed.owner())
                .find(fixed.program_artifact());
            missing = !std::filesystem::exists(configuration_.directory / id / "input.pb");
        });
        std::optional<research::proto::AccountReviewInput> recovered;
        if (missing)
            recovered = co_await account_evidence(
                call,
                fixed.request(),
                fixed.owner(),
                fixed.program_artifact());
        std::optional<Store::Submission> input;
        co_await file_work([&] {
            if (recovered) {
                const auto bytes = recovered->SerializeAsString();
                if (sha256_bytes(bytes) != prepared.record().input_digest()
                    || bytes.size() != prepared.record().input_bytes())
                    throw Error(
                        ErrorCode::conflict,
                        "recovered account evidence differs from preparation");
                (void)payload::publish(
                    configuration_.directory / id / "input.pb",
                    *recovered,
                    payload::max_input_bytes);
            }
            input.emplace(storage_->resume_submission(id));
        });
        *call.response.mutable_task() = (co_await submit(
                                             call,
                                             [&] {
                                                 return std::move(*input);
                                             },
                                             true))
                                            .task();
        co_return;
    }
    if (prepared.execution().has_logic_trial()) {
        const auto& fixed = prepared.execution().logic_trial();
        protocol::validate_logic_trial_input(fixed);
        const auto node = configuration_.node_id;
        if (fixed.owner().node_id() != node || fixed.owner().service_id() != configuration_.service)
            throw Error(ErrorCode::conflict, "invalid experiment derivation");
        std::optional<Store::Submission> input;
        co_await file_work([&] {
            (void)programs::Files(configuration_.directory / "programs", fixed.owner())
                .find(prepared.execution().program_artifact());
            const auto path = configuration_.directory / id / "input.pb";
            if (!std::filesystem::exists(path)) {
                const auto restored = payload::publish(path, fixed, payload::max_input_bytes);
                if (restored.digest != prepared.record().input_digest()
                    || restored.size != prepared.record().input_bytes())
                    throw std::invalid_argument("reconstructed input differs from preparation");
            }
            input.emplace(storage_->resume_submission(id));
        });
        *call.response.mutable_task() = (co_await submit(
                                             call,
                                             [&] {
                                                 return std::move(*input);
                                             },
                                             true))
                                            .task();
        co_return;
    }
    if (prepared.execution().has_evaluation()) {
        const auto& fixed = prepared.execution().evaluation();
        protocol::validate_evaluation_input(fixed);
        const auto& owner = fixed.request().source().owner();
        const auto node = configuration_.node_id;
        if (owner.node_id() != node || owner.service_id() != configuration_.service)
            throw Error(ErrorCode::conflict, "invalid experiment derivation");
        std::optional<Store::Submission> input;
        co_await file_work([&] {
            (void)programs::Files(configuration_.directory / "programs", owner)
                .find(prepared.execution().program_artifact());
            const auto path = configuration_.directory / id / "input.pb";
            if (!std::filesystem::exists(path)) {
                const auto restored = payload::publish(path, fixed, payload::max_input_bytes);
                if (restored.digest != prepared.record().input_digest()
                    || restored.size != prepared.record().input_bytes())
                    throw std::invalid_argument("reconstructed input differs from preparation");
            }
            input.emplace(storage_->resume_submission(id));
        });
        *call.response.mutable_task() = (co_await submit(
                                             call,
                                             [&] {
                                                 return std::move(*input);
                                             },
                                             true))
                                            .task();
        co_return;
    }
    if (prepared.execution().has_statistics()) {
        const auto& fixed = prepared.execution().statistics();
        protocol::validate_statistics_input(fixed);
        const auto node = configuration_.node_id;
        if (fixed.owner().node_id() != node || fixed.owner().service_id() != configuration_.service)
            throw Error(ErrorCode::conflict, "invalid experiment derivation");
        std::optional<Store::Submission> input;
        co_await file_work([&] {
            (void)programs::Files(configuration_.directory / "programs", fixed.owner())
                .find(prepared.execution().program_artifact());
            const auto path = configuration_.directory / id / "input.pb";
            if (!std::filesystem::exists(path)) {
                const auto restored = payload::publish(path, fixed, payload::max_input_bytes);
                if (restored.digest != prepared.record().input_digest()
                    || restored.size != prepared.record().input_bytes())
                    throw std::invalid_argument("reconstructed input differs from preparation");
            }
            input.emplace(storage_->resume_submission(id));
        });
        *call.response.mutable_task() = (co_await submit(
                                             call,
                                             [&] {
                                                 return std::move(*input);
                                             },
                                             true))
                                            .task();
        co_return;
    }
    if (!prepared.execution().has_data_owner())
        throw Error(ErrorCode::conflict, "research recovery requires a fixed Data owner");
    call.data_route = co_await data_.resolve(
        prepared.execution().data_owner(),
        std::chrono::ceil<std::chrono::milliseconds>(call.input_deadline - Clock::now()),
        call.request.correlation_id());
    const auto retention =
        research_retention(prepared.execution(), *call.data_route, configuration_.service, id);
    co_await file_work([&] {
        save_research_retention(configuration_.directory / id, retention);
    });
    data::proto::DataRequest keep;
    *keep.mutable_retention()->mutable_prepare() = retention;
    const auto kept =
        co_await data_call(*call.data_route, std::move(keep), 5s, call.request.correlation_id());
    if (kept.retention().retention().SerializeAsString() != retention.SerializeAsString())
        throw std::invalid_argument("recovery input retention identity mismatch");
    std::optional<Store::Submission> input;
    const auto deadline = call.input_deadline;
    auto reconstruction = files_.submit([&, deadline](std::stop_token stop) {
        const auto path = configuration_.directory / id / "input.pb";
        if (!std::filesystem::exists(path)) {
            wire::TaskAttempt attempt;
            *attempt.mutable_task() = prepared.record().task();
            *attempt.mutable_execution() = prepared.execution();
            attempt.set_data_endpoint(call.data_route->endpoint);
            attempt.set_data_instance(call.data_route->owner.service_id());
            resolve_task_input(attempt, stop, deadline);
            const auto& task = attempt.task();
            const google::protobuf::Message& definition = task.has_input()
                ? static_cast<const google::protobuf::Message&>(task.input())
                : task.has_sample() ? static_cast<const google::protobuf::Message&>(task.sample())
                                    : static_cast<const google::protobuf::Message&>(task.factor());
            const auto restored = payload::publish(path, definition, payload::max_input_bytes);
            if (restored.digest != prepared.record().input_digest()
                || restored.size != prepared.record().input_bytes())
                throw std::invalid_argument("reconstructed input differs from preparation");
        }
        input.emplace(storage_->resume_submission(id));
    });
    co_await PollFuture{std::move(reconstruction)};
    co_await ready(call);
    call.input_in_time();
    *call.response.mutable_task() = (co_await submit(
                                         call,
                                         [&] {
                                             return std::move(*input);
                                         },
                                         true))
                                        .task();
}

void ResearchHost::release_decision(Call& call)
{
    const auto& requested = call.request.release_decision();
    validate_id(requested.id());
    if (requested.owner().domain() != "research"
        || requested.owner().service_id() != configuration_.service)
        throw Error(ErrorCode::conflict, "release decision belongs to another owner");
    auto* reply = call.response.mutable_release_decision();
    *reply->mutable_object() = requested;
    if (const auto found_task = storage_->find_task(requested.id())) {
        const auto& task = *found_task;
        reply->set_state(
            task.has_retirement()
                    && task.retirement().SerializeAsString() == requested.SerializeAsString()
                ? resource::proto::ReleaseDecision::RELEASED
                : resource::proto::ReleaseDecision::RETAINED);
        return;
    }
    const auto decided = storage_->abandoned_preparation(requested.id());
    reply->set_state(
        decided && decided->SerializeAsString() == requested.SerializeAsString()
            ? resource::proto::ReleaseDecision::RELEASED
            : resource::proto::ReleaseDecision::UNKNOWN);
}

PolledTask<> ResearchHost::abandon_preparation(Call& call)
{
    const auto id = call.request.abandon_preparation().id();
    validate_id(id);
    if (storage_->find_task(id))
        throw Error(ErrorCode::conflict, "admitted task cannot be abandoned as a preparation");
    auto released = storage_->abandoned_preparation(id);
    if (!released) {
        wire::PreparedTaskSubmission prepared;
        co_await file_work([&] {
            prepared = storage_->prepared_submission_record(id);
        });
        call.input_in_time();
        const auto node = configuration_.node_id;
        if (prepared.execution().has_account_review()) {
            const auto& fixed = prepared.execution().account_review();
            protocol::validate_account_review_execution(fixed);
            if (fixed.owner().node_id() != node
                || fixed.owner().service_id() != configuration_.service)
                throw Error(
                    ErrorCode::conflict,
                    "account review preparation belongs to another owner");
            resource::proto::Object object;
            *object.mutable_owner() = fixed.owner();
            object.set_kind("account_review");
            object.set_id(id);
            object.set_version(prepared.record().input_digest());
            co_await ready(call);
            call.input_in_time();
            refuse_during_upgrade(quiescing_);
            (void)co_await commit(storage_->abandon_preparation(prepared, object));
            auto* reply = call.response.mutable_release_decision();
            *reply->mutable_object() = object;
            reply->set_state(resource::proto::ReleaseDecision::RELEASED);
            co_return;
        }
        if (prepared.execution().has_logic_trial()) {
            const auto& fixed = prepared.execution().logic_trial();
            protocol::validate_logic_trial_input(fixed);
            if (fixed.owner().node_id() != node
                || fixed.owner().service_id() != configuration_.service)
                throw Error(ErrorCode::conflict, "invalid experiment derivation");
            resource::proto::Object object;
            *object.mutable_owner() = fixed.owner();
            object.set_kind("logic_trial");
            object.set_id(id);
            object.set_version(prepared.record().input_digest());
            co_await ready(call);
            call.input_in_time();
            refuse_during_upgrade(quiescing_);
            (void)co_await commit(storage_->abandon_preparation(prepared, object));
            auto* reply = call.response.mutable_release_decision();
            *reply->mutable_object() = object;
            reply->set_state(resource::proto::ReleaseDecision::RELEASED);
            co_return;
        }
        if (prepared.execution().has_evaluation()) {
            const auto& fixed = prepared.execution().evaluation();
            protocol::validate_evaluation_input(fixed);
            const auto& owner = fixed.request().source().owner();
            if (owner.node_id() != node || owner.service_id() != configuration_.service)
                throw Error(ErrorCode::conflict, "invalid experiment derivation");
            resource::proto::Object object;
            *object.mutable_owner() = owner;
            object.set_kind("performance_evaluation");
            object.set_id(id);
            object.set_version(prepared.record().input_digest());
            co_await ready(call);
            call.input_in_time();
            refuse_during_upgrade(quiescing_);
            (void)co_await commit(storage_->abandon_preparation(prepared, object));
            auto* reply = call.response.mutable_release_decision();
            *reply->mutable_object() = object;
            reply->set_state(resource::proto::ReleaseDecision::RELEASED);
            co_return;
        }
        if (prepared.execution().has_statistics()) {
            const auto& fixed = prepared.execution().statistics();
            protocol::validate_statistics_input(fixed);
            if (fixed.owner().node_id() != node
                || fixed.owner().service_id() != configuration_.service)
                throw Error(ErrorCode::conflict, "invalid experiment derivation");
            resource::proto::Object object;
            *object.mutable_owner() = fixed.owner();
            object.set_kind("statistics");
            object.set_id(id);
            object.set_version(prepared.record().input_digest());
            co_await ready(call);
            call.input_in_time();
            refuse_during_upgrade(quiescing_);
            (void)co_await commit(storage_->abandon_preparation(prepared, object));
            auto* reply = call.response.mutable_release_decision();
            *reply->mutable_object() = object;
            reply->set_state(resource::proto::ReleaseDecision::RELEASED);
            co_return;
        }
        const ResearchDataRoute route{prepared.execution().data_owner(), {}, node};
        const auto retention =
            research_retention(prepared.execution(), route, configuration_.service, id);
        co_await file_work([&] {
            save_research_retention(configuration_.directory / id, retention);
        });
        co_await ready(call);
        call.input_in_time();
        refuse_during_upgrade(quiescing_);
        (void)co_await commit(storage_->abandon_preparation(prepared, retention.consumer()));
        released = retention.consumer();
    }
    auto* reply = call.response.mutable_release_decision();
    *reply->mutable_object() = *released;
    reply->set_state(resource::proto::ReleaseDecision::RELEASED);
}

PolledTask<> ResearchHost::retirement(Call& call)
{
    const auto& request = call.request.retirement();
    validate_id(request.id());
    const auto node = configuration_.node_id;
    co_await ready(call);
    call.input_in_time();
    auto preview = storage_->retirement(request.id(), node);
    co_await strategy_holders(call, preview);
    if (storage_->describe(request.id()).kind() == wire::LOGIC_TRIAL) {
        wire::PreparedTaskSubmission prepared;
        co_await file_work([&] {
            prepared = storage_->prepared_submission_record(request.id());
        });
        const auto& fixed = prepared.execution().logic_trial();
        protocol::validate_logic_trial_input(fixed);
        if (fixed.owner().node_id() != node || fixed.owner().service_id() != configuration_.service
            || preview.object().owner().SerializeAsString() != fixed.owner().SerializeAsString()
            || preview.object().version() != prepared.record().input_digest()
            || preview.object().kind() != "logic_trial")
            throw Error(ErrorCode::conflict, "invalid logic trial source identity");
        if (request.execute() && preview.state() == wire::TaskRetirement::AVAILABLE) {
            co_await ready(call);
            call.input_in_time();
            refuse_during_upgrade(quiescing_);
            preview = storage_->retirement(request.id(), node);
            co_await strategy_holders(call, preview);
            if (preview.state() == wire::TaskRetirement::AVAILABLE) {
                (void)co_await commit(storage_->retire(preview.object()));
                preview = storage_->retirement(request.id(), node);
            }
        }
        // The same Research journal owns both the derivation and its release.
        // Logical retirement preserves all formal input/result/program files.
        preview.set_source_release_confirmed(preview.state() == wire::TaskRetirement::RETIRED);
        *call.response.mutable_retirement() = std::move(preview);
        co_return;
    }
    if (storage_->describe(request.id()).kind() == wire::PERFORMANCE_EVALUATION) {
        wire::PreparedTaskSubmission prepared;
        co_await file_work([&] {
            prepared = storage_->prepared_submission_record(request.id());
        });
        const auto& fixed = prepared.execution().evaluation();
        protocol::validate_evaluation_input(fixed);
        const auto& owner = fixed.request().source().owner();
        if (owner.node_id() != node || owner.service_id() != configuration_.service
            || preview.object().owner().SerializeAsString() != owner.SerializeAsString()
            || preview.object().version() != prepared.record().input_digest()
            || preview.object().kind() != "performance_evaluation")
            throw Error(ErrorCode::conflict, "evaluation retirement identity changed");
        if (request.execute() && preview.state() == wire::TaskRetirement::AVAILABLE) {
            co_await ready(call);
            call.input_in_time();
            refuse_during_upgrade(quiescing_);
            (void)co_await commit(storage_->retire(preview.object()));
            preview = storage_->retirement(request.id(), node);
        }
        preview.set_source_release_confirmed(preview.state() == wire::TaskRetirement::RETIRED);
        *call.response.mutable_retirement() = std::move(preview);
        co_return;
    }
    if (storage_->describe(request.id()).kind() == wire::STATISTICS) {
        wire::PreparedTaskSubmission prepared;
        co_await file_work([&] {
            prepared = storage_->prepared_submission_record(request.id());
        });
        const auto& fixed = prepared.execution().statistics();
        protocol::validate_statistics_input(fixed);
        if (fixed.owner().node_id() != node || fixed.owner().service_id() != configuration_.service
            || preview.object().owner().SerializeAsString() != fixed.owner().SerializeAsString()
            || preview.object().version() != prepared.record().input_digest()
            || preview.object().kind() != "statistics")
            throw Error(ErrorCode::conflict, "invalid statistics source identity");
        if (request.execute() && preview.state() == wire::TaskRetirement::AVAILABLE) {
            co_await ready(call);
            call.input_in_time();
            refuse_during_upgrade(quiescing_);
            (void)co_await commit(storage_->retire(preview.object()));
            preview = storage_->retirement(request.id(), node);
        }
        // The same Research journal owns both the derivation and its release.
        // Logical retirement preserves all formal input/result/program files.
        preview.set_source_release_confirmed(preview.state() == wire::TaskRetirement::RETIRED);
        *call.response.mutable_retirement() = std::move(preview);
        co_return;
    }
    if (storage_->describe(request.id()).kind() == wire::ACCOUNT_REVIEW) {
        wire::PreparedTaskSubmission prepared;
        co_await file_work([&] {
            prepared = storage_->prepared_submission_record(request.id());
        });
        const auto& fixed = prepared.execution().account_review();
        protocol::validate_account_review_execution(fixed);
        if (fixed.owner().node_id() != node || fixed.owner().service_id() != configuration_.service
            || preview.object().owner().SerializeAsString() != fixed.owner().SerializeAsString()
            || preview.object().version() != prepared.record().input_digest()
            || preview.object().kind() != "account_review")
            throw Error(ErrorCode::conflict, "account review retirement identity changed");
        if (request.execute() && preview.state() == wire::TaskRetirement::AVAILABLE) {
            co_await ready(call);
            call.input_in_time();
            refuse_during_upgrade(quiescing_);
            (void)co_await commit(storage_->retire(preview.object()));
            preview = storage_->retirement(request.id(), node);
        }
        if (preview.state() == wire::TaskRetirement::RETIRED) {
            preview.set_source_release_confirmed(false);
            preview.mutable_release_error()->set_code("unavailable");
            preview.mutable_release_error()->set_message(
                "Trading facts remain retained; source release is not supported");
        }
        *call.response.mutable_retirement() = std::move(preview);
        co_return;
    }
    if (request.execute() && preview.state() == wire::TaskRetirement::AVAILABLE) {
        wire::PreparedTaskSubmission prepared;
        co_await file_work([&] {
            prepared = storage_->prepared_submission_record(request.id());
            const ResearchDataRoute route{prepared.execution().data_owner(), {}, node};
            const auto retained = research_retention(
                prepared.execution(),
                route,
                configuration_.service,
                request.id());
            if (retained.consumer().SerializeAsString() != preview.object().SerializeAsString())
                throw Error(ErrorCode::conflict, "research retirement evidence changed");
            verify_research_retention(configuration_.directory / request.id(), retained);
        });
        co_await ready(call);
        call.input_in_time();
        refuse_during_upgrade(quiescing_);
        // File verification yielded: a candidate may have been frozen since
        // the preview. Rejoin its journal and inspect the current authority.
        preview = storage_->retirement(request.id(), node);
        co_await strategy_holders(call, preview);
        if (preview.state() == wire::TaskRetirement::AVAILABLE) {
            (void)co_await commit(storage_->retire(preview.object()));
            preview = storage_->retirement(request.id(), node);
        }
    }
    if (preview.state() == wire::TaskRetirement::RETIRED) {
        wire::PreparedTaskSubmission prepared;
        co_await file_work([&] {
            prepared = storage_->prepared_submission_record(request.id());
        });
        const ResearchDataRoute original{prepared.execution().data_owner(), {}, node};
        const auto retained = research_retention(
            prepared.execution(),
            original,
            configuration_.service,
            request.id());
        if (retained.consumer().SerializeAsString() != preview.object().SerializeAsString())
            throw Error(ErrorCode::conflict, "research retirement evidence changed");
        try {
            const auto route =
                co_await data_.resolve(retained.source(), 3s, call.request.correlation_id());
            data::proto::DataRequest query;
            if (request.execute())
                *query.mutable_retention()->mutable_release() = preview.object();
            else
                *query.mutable_retention()->mutable_get() = preview.object();
            const auto result =
                co_await data_call(route, std::move(query), 5s, call.request.correlation_id());
            if (result.retention().retention().SerializeAsString() != retained.SerializeAsString())
                throw Error(ErrorCode::conflict, "Data retirement release identity mismatch");
            preview.set_source_release_confirmed(
                result.retention().phase() == data::proto::DataRetentionRecord::RELEASED);
        } catch (const Error& error) {
            preview.mutable_release_error()->set_code(std::string(error_name(error.code())));
            preview.mutable_release_error()->set_message(error.what());
        }
    }
    *call.response.mutable_retirement() = std::move(preview);
}

} // namespace asterion::tasks
