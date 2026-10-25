#include "research_host_internal.hpp"

namespace asterion::tasks {

PolledTask<> ResearchHost::project_operation(Call& call)
{
    const auto node = configuration_.node_id;
    co_await wait_for_store({}, call.deadline);
    call.input_in_time();
    const auto request = call.request.project();
    const auto catalogue = call.request.resources();
    if (request.has_mutation())
        refuse_during_upgrade(quiescing_);
    resource::proto::Owner owner;
    owner.set_node_id(node);
    owner.set_service_id(configuration_.service);
    owner.set_domain("research");
    if (request.has_mutation() && request.mutation().has_conclude()) {
        const auto& result = request.mutation().conclude().result();
        validate_id(result.id());
        if (result.owner().SerializeAsString() != owner.SerializeAsString()
            || (result.kind() != "factor_result" && result.kind() != "sample_result"
                && result.kind() != "statistics_result" && result.kind() != "backtest_result"
                && result.kind() != "performance_evaluation_result"))
            throw Error(ErrorCode::conflict, "research conclusion result mismatch");
        auto slot = verification_slots_.acquire(false, result.id());
        auto read = storage_->prepare_result(result.id());
        co_await file_work([&] {
            read.verify();
        });
        co_await ready(call);
        call.input_in_time();
        const auto verified = storage_->confirm_result(std::move(read));
        const bool kind_matches = (verified.has_factor() && result.kind() == "factor_result")
            || (verified.has_sample() && result.kind() == "sample_result")
            || (verified.has_statistics() && result.kind() == "statistics_result")
            || (verified.has_backtest() && result.kind() == "backtest_result")
            || (verified.has_evaluation() && result.kind() == "performance_evaluation_result");
        if (!kind_matches || verified.result_task().result_digest() != result.version())
            throw Error(ErrorCode::conflict, "research conclusion result mismatch");
        // Rejoin the same journal after file verification. Formal evidence is
        // read-only; only the human interpretation is appended below.
        co_await wait_for_store({}, call.deadline);
    }
    auto work = std::make_shared<MetadataWork>();
    metadata_work_ = work;
    try {
        work->result = journal_.submit([this, request, catalogue, owner, work](std::stop_token) {
            research::Projects projects(
                *storage_,
                owner,
                request.has_mutation() ? sqlite::Database::Access::writer
                                       : sqlite::Database::Access::read_only);
            if (!catalogue.kind().empty()) {
                work->catalogue = projects.resources(catalogue);
                return;
            }
            research::proto::ProjectReply reply;
            if (request.has_mutation()) {
                work->reply = projects.apply(request.mutation());
                return;
            }
            if (request.has_draft()) {
                const auto found = projects.draft(request.draft().id());
                if (!found)
                    throw Error(ErrorCode::not_found, "unknown research project");
                *reply.mutable_draft() = *found;
            } else if (request.has_plan())
                *reply.mutable_plan() = projects.plan(request.plan().id());
            else if (request.has_conclusion())
                *reply.mutable_conclusion() = projects.conclusion(request.conclusion().id());
            else if (request.has_conclusions()) {
                const auto& query = request.conclusions();
                const auto rows =
                    projects.conclusions(query.project_id(), query.after(), query.limit());
                for (const auto& row : rows)
                    *reply.add_conclusions() = row;
                if (rows.size() == query.limit()
                    && !projects.conclusions(query.project_id(), rows.back().declaration().id(), 1)
                        .empty())
                    reply.set_next_after(rows.back().declaration().id());
            } else if (request.has_drafts()) {
                const auto& query = request.drafts();
                if (!query.project_id().empty())
                    throw std::invalid_argument("invalid research project list query");
                const auto rows = projects.drafts(query.after(), query.limit());
                for (const auto& row : rows)
                    *reply.add_drafts() = row;
                if (rows.size() == query.limit() && !projects.drafts(rows.back().id(), 1).empty())
                    reply.set_next_after(rows.back().id());
            } else if (request.has_plans()) {
                const auto& query = request.plans();
                const auto rows = projects.plans(query.project_id(), query.after(), query.limit());
                for (const auto& row : rows)
                    *reply.add_plans() = row;
                if (rows.size() == query.limit()
                    && !projects.plans(query.project_id(), rows.back().id(), 1).empty())
                    reply.set_next_after(rows.back().id());
            } else
                throw std::invalid_argument("invalid research project request");
            work->reply = std::move(reply);
        });
    } catch (...) {
        metadata_work_.reset();
        throw;
    }
    // Accepted work owns its request and connection gate through disconnects.
    co_await PollUntil{[&] {
        return work->done;
    }};
    if (work->error)
        std::rethrow_exception(work->error);
    if (call.request.has_resources())
        *call.response.mutable_resources() = std::move(work->catalogue);
    else
        *call.response.mutable_project() = std::move(work->reply);
}

PolledTask<research::proto::ModelReply> ResearchHost::model_metadata(
    const Call& call,
    const resource::proto::Owner& owner,
    bool writer,
    std::function<void(research::Models&, research::proto::ModelReply&)> operation)
{
    co_await wait_for_store({}, call.deadline);
    auto work = std::make_shared<MetadataWork>();
    metadata_work_ = work;
    try {
        work->result = journal_.submit(
            [this, owner, writer, operation = std::move(operation), work](std::stop_token) {
                research::Models models(
                    *storage_,
                    owner,
                    writer ? sqlite::Database::Access::writer
                           : sqlite::Database::Access::read_only);
                operation(models, work->model);
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
    co_return std::move(work->model);
}

PolledTask<> ResearchHost::model_operation(Call& call)
{
    const auto node = configuration_.node_id;
    resource::proto::Owner owner;
    owner.set_node_id(node);
    owner.set_service_id(configuration_.service);
    owner.set_domain("research");
    if (call.request.has_resources()) {
        const auto query = call.request.resources();
        co_await wait_for_store({}, call.deadline);
        call.input_in_time();
        auto work = std::make_shared<MetadataWork>();
        metadata_work_ = work;
        try {
            work->result = journal_.submit([this, owner, query, work](std::stop_token) {
                work->catalogue = research::Models(*storage_, owner).resources(query);
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
        *call.response.mutable_resources() = std::move(work->catalogue);
        co_return;
    }
    const auto request = call.request.model();
    if (request.has_preview()) {
        const auto& query = request.preview();
        protocol::validate_strategy_object(query.mapping(), "model_mapping");
        if (query.mapping().owner().SerializeAsString() != owner.SerializeAsString()
            || query.limit() < 1 || query.limit() > 200)
            throw std::invalid_argument("diagnostic mapping owner or page is invalid");
        const auto value =
            co_await model_metadata(call, owner, false, [query](auto& models, auto& reply) {
                *reply.mutable_mapping() = models.mapping(query.mapping().id());
            });
        const auto mapping = value.mapping();
        if (protocol::model_mapping_object(mapping).SerializeAsString()
            != query.mapping().SerializeAsString())
            throw Error(ErrorCode::conflict, "diagnostic mapping fixed identity changed");
        auto slot = verification_slots_.acquire(false, mapping.specification().model().id());
        auto read = storage_->prepare_result(mapping.specification().model().id());
        co_await file_work([&] {
            read.verify();
        });
        co_await ready(call);
        call.input_in_time();
        const auto original = storage_->confirm_result(std::move(read));
        research::proto::ModelMappingProjection projection;
        co_await file_work([&] {
            projection =
                research::project_model_mapping(mapping, original, query.begin(), query.limit());
        });
        co_await ready(call);
        call.input_in_time();
        *call.response.mutable_model()->mutable_projection() = std::move(projection);
        co_return;
    }
    if (request.has_mutation()) {
        refuse_during_upgrade(quiescing_);
        protocol::validate_model_mutation(request.mutation());
        const auto old =
            co_await model_metadata(call, owner, false, [request](auto& models, auto& reply) {
                if (const auto receipt = models.receipt(request.mutation()))
                    reply = *receipt;
            });
        if (old.has_draft() || old.has_plan() || old.has_mapping() || old.has_time_plan()) {
            *call.response.mutable_model() = old;
            co_return;
        }
        std::optional<research::proto::ModelPlan> captured;
        std::optional<research::proto::ModelMapping> captured_mapping;
        std::optional<research::proto::ModelTimePlan> captured_time;
        if (request.mutation().has_fix()) {
            const auto draft =
                co_await model_metadata(call, owner, false, [request](auto& models, auto& reply) {
                    const auto value = models.draft(request.mutation().fix().draft_id());
                    if (!value)
                        throw Error(ErrorCode::not_found, "model draft does not exist");
                    *reply.mutable_draft() = *value;
                });
            if (draft.draft().digest() != request.mutation().fix().expected_digest())
                throw Error(ErrorCode::conflict, "model draft changed before fixing");
            const auto programme = co_await program(call);
            const auto source_id = draft.draft().specification().source().id();
            auto slot = verification_slots_.acquire(false, source_id);
            auto read = storage_->prepare_result(source_id);
            const auto input_digest = read.input_digest();
            const auto input_bytes = read.input_bytes();
            co_await file_work([&] {
                read.verify();
            });
            co_await ready(call);
            call.input_in_time();
            const auto source = storage_->confirm_result(std::move(read));
            captured = research::capture_model_plan(
                owner,
                draft.draft(),
                request.mutation().fix().plan_id(),
                source,
                input_digest,
                input_bytes,
                programme.artifact,
                SystemClock{}.utc_now() / 1000000);
        }
        if (request.mutation().has_time_validation()) {
            const auto saved =
                co_await model_metadata(call, owner, false, [request](auto& models, auto& reply) {
                    *reply.mutable_plan() = models.plan(
                        request.mutation().time_validation().specification().recipe().id());
                });
            const auto programme = co_await program(call);
            if (saved.plan().program_artifact() != programme.artifact)
                throw Error(
                    ErrorCode::conflict,
                    "model time plan requires its fixed current training program");
            const auto source_id = saved.plan().source().result().id();
            auto slot = verification_slots_.acquire(false, source_id);
            auto read = storage_->prepare_result(source_id);
            co_await file_work([&] {
                read.verify();
            });
            co_await ready(call);
            call.input_in_time();
            auto verified =
                std::make_shared<wire::TaskResponse>(storage_->confirm_result(std::move(read)));
            const auto value = co_await model_metadata(
                call,
                owner,
                false,
                [request, verified](auto& models, auto& reply) {
                    *reply.mutable_time_plan() =
                        models.capture_time(request.mutation().time_validation(), *verified);
                });
            captured_time = value.time_plan();
        }
        if (request.mutation().has_mapping()) {
            const auto source_id = request.mutation().mapping().specification().model().id();
            auto slot = verification_slots_.acquire(false, source_id);
            auto read = storage_->prepare_result(source_id);
            co_await file_work([&] {
                read.verify();
            });
            co_await ready(call);
            call.input_in_time();
            auto verified =
                std::make_shared<wire::TaskResponse>(storage_->confirm_result(std::move(read)));
            const auto value = co_await model_metadata(
                call,
                owner,
                false,
                [request, verified](auto& models, auto& reply) {
                    *reply.mutable_mapping() =
                        models.capture_mapping(request.mutation().mapping(), *verified);
                });
            captured_mapping = value.mapping();
        }
        call.input_in_time();
        refuse_during_upgrade(quiescing_);
        *call.response.mutable_model() = co_await model_metadata(
            call,
            owner,
            true,
            [request, captured, captured_mapping, captured_time](auto& models, auto& reply) {
                reply = models.apply(
                    request.mutation(),
                    captured ? &*captured : nullptr,
                    captured_mapping ? &*captured_mapping : nullptr,
                    captured_time ? &*captured_time : nullptr);
            });
        co_return;
    }
    *call.response.mutable_model() =
        co_await model_metadata(call, owner, false, [request](auto& models, auto& reply) {
            if (request.has_read()) {
                if (request.read().kind() == research::proto::ModelRead::DRAFT) {
                    const auto value = models.draft(request.read().id());
                    if (!value)
                        throw Error(ErrorCode::not_found, "model draft does not exist");
                    *reply.mutable_draft() = *value;
                } else if (request.read().kind() == research::proto::ModelRead::PLAN)
                    *reply.mutable_plan() = models.plan(request.read().id());
                else if (request.read().kind() == research::proto::ModelRead::MAPPING)
                    *reply.mutable_mapping() = models.mapping(request.read().id());
                else if (request.read().kind() == research::proto::ModelRead::TIME_PLAN)
                    *reply.mutable_time_plan() = models.time_plan(request.read().id());
                else
                    throw std::invalid_argument("invalid model read kind");
            } else if (request.has_list()) {
                const auto& page = request.list();
                if (page.kind() == research::proto::ModelRead::DRAFT) {
                    const auto rows = models.drafts(page.after(), page.limit());
                    for (const auto& row : rows)
                        *reply.add_drafts() = row;
                    if (rows.size() == page.limit() && !models.drafts(rows.back().id(), 1).empty())
                        reply.set_next_after(rows.back().id());
                } else if (page.kind() == research::proto::ModelRead::PLAN) {
                    const auto rows = models.plans(page.after(), page.limit());
                    for (const auto& row : rows)
                        *reply.add_plans() = row;
                    if (rows.size() == page.limit() && !models.plans(rows.back().id(), 1).empty())
                        reply.set_next_after(rows.back().id());
                } else if (page.kind() == research::proto::ModelRead::MAPPING) {
                    const auto rows = models.mappings(page.after(), page.limit());
                    for (const auto& row : rows)
                        *reply.add_mappings() = row;
                    if (rows.size() == page.limit()
                        && !models.mappings(rows.back().id(), 1).empty())
                        reply.set_next_after(rows.back().id());
                } else if (page.kind() == research::proto::ModelRead::TIME_PLAN) {
                    const auto rows = models.time_plans(page.after(), page.limit());
                    for (const auto& row : rows)
                        *reply.add_time_plans() = row;
                    if (rows.size() == page.limit()
                        && !models.time_plans(rows.back().id(), 1).empty())
                        reply.set_next_after(rows.back().id());
                } else
                    throw std::invalid_argument("invalid model list kind");
            } else
                throw std::invalid_argument("invalid model operation");
        });
}

PolledTask<> ResearchHost::submit_model(Call& call)
{
    const auto& submission = call.request.submit();
    const bool time = submission.has_model_time();
    const auto selected =
        time ? submission.model_time().plan() : submission.model_training().plan();
    protocol::validate_strategy_object(selected, time ? "model_time_plan" : "model_plan");
    validate_id(submission.id());
    if (submission.has_data_owner() || submission.has_derivation() || submission.has_research_plan()
        || submission.has_strategy_use())
        throw std::invalid_argument("model training accepts only its fixed owned plan");
    if (const auto found_task = storage_->find_task(submission.id())) {
        const auto& existing = *found_task;
        if (existing.kind() != (time ? wire::MODEL_TIME_VALIDATION : wire::MODEL_TRAINING)
            || existing.has_retirement()
            || (time ? existing.model_time_plan() : existing.model_plan()).SerializeAsString()
                != selected.SerializeAsString())
            throw Error(ErrorCode::conflict, "model task identity names different fixed evidence");
        *call.response.mutable_task() = existing;
        co_return;
    }
    const auto programme = co_await program(call);
    if (selected.owner().SerializeAsString() != programme.owner.SerializeAsString())
        throw Error(ErrorCode::conflict, "model plan must belong to this Research service");
    auto fixed = co_await model_metadata(
        call,
        programme.owner,
        false,
        [selected, programme, time](auto& models, auto& reply) {
            if (time)
                *reply.mutable_time_plan() = models.time_admission(selected, programme.artifact);
            else
                *reply.mutable_plan() = models.admission(selected, programme.artifact);
        });
    research::proto::ModelTrainingInput input;
    research::proto::ModelTimeInput time_input;
    if (time)
        *time_input.mutable_plan() = fixed.time_plan();
    else
        *input.mutable_plan() = fixed.plan();
    *call.response.mutable_task() = (co_await submit(call, [&] {
        return time ? task_submission(*storage_, submission.id(), std::move(time_input))
                    : task_submission(*storage_, submission.id(), std::move(input));
    })).task();
}

PolledTask<> ResearchHost::variable_operation(Call& call)
{
    const auto node = configuration_.node_id;
    co_await wait_for_store({}, call.deadline);
    call.input_in_time();
    const auto request = call.request.variable();
    const auto catalogue = call.request.resources();
    if (request.has_mutation())
        refuse_during_upgrade(quiescing_);
    resource::proto::Owner owner;
    owner.set_node_id(node);
    owner.set_service_id(configuration_.service);
    owner.set_domain("research");
    auto work = std::make_shared<MetadataWork>();
    metadata_work_ = work;
    try {
        work->result = journal_.submit([this, request, catalogue, owner, work](std::stop_token) {
            if (request.has_preview()) {
                *work->variable.mutable_preview() = research::Variables::preview(request.preview());
                return;
            }
            research::Variables variables(
                *storage_,
                owner,
                request.has_mutation() ? sqlite::Database::Access::writer
                                       : sqlite::Database::Access::read_only);
            if (!catalogue.kind().empty()) {
                work->catalogue = variables.resources(catalogue);
                return;
            }
            if (request.has_mutation())
                work->variable = variables.apply(request.mutation());
            else if (request.has_get())
                *work->variable.mutable_definition() = variables.definition(request.get().id());
            else if (request.has_list()) {
                const auto& page = request.list();
                const auto rows = variables.list(page.after(), page.limit());
                for (const auto& row : rows)
                    *work->variable.add_definitions() = row;
                if (rows.size() == page.limit() && !variables.list(rows.back().id(), 1).empty())
                    work->variable.set_next_after(rows.back().id());
            } else
                throw std::invalid_argument("invalid research variable request");
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
    if (call.request.has_resources())
        *call.response.mutable_resources() = std::move(work->catalogue);
    else
        *call.response.mutable_variable() = std::move(work->variable);
}

PolledTask<> ResearchHost::logic_operation(Call& call)
{
    const auto node = configuration_.node_id;
    co_await wait_for_store({}, call.deadline);
    call.input_in_time();
    const auto request = call.request.logic();
    const auto catalogue = call.request.resources();
    if (request.has_mutation())
        refuse_during_upgrade(quiescing_);
    resource::proto::Owner owner;
    owner.set_node_id(node);
    owner.set_service_id(configuration_.service);
    owner.set_domain("research");
    auto work = std::make_shared<MetadataWork>();
    metadata_work_ = work;
    try {
        work->result = journal_.submit([this, request, catalogue, owner, work](std::stop_token) {
            research::Logic logic(
                *storage_,
                owner,
                request.has_mutation() ? sqlite::Database::Access::writer
                                       : sqlite::Database::Access::read_only);
            if (!catalogue.kind().empty()) {
                work->catalogue = logic.resources(catalogue);
                return;
            }
            if (request.has_mutation())
                work->logic = logic.apply(request.mutation());
            else if (request.has_get())
                *work->logic.mutable_definition() = logic.definition(request.get().id());
            else if (request.has_example())
                *work->logic.mutable_example() = logic.example(request.example().id());
            else if (request.has_list()) {
                const auto& page = request.list();
                const auto rows = logic.definitions(page.after(), page.limit());
                for (const auto& row : rows)
                    *work->logic.add_definitions() = row;
                if (rows.size() == page.limit() && !logic.definitions(rows.back().id(), 1).empty())
                    work->logic.set_next_after(rows.back().id());
            } else if (request.has_examples()) {
                const auto& page = request.examples();
                const auto rows = logic.examples(page.definition_id(), page.after(), page.limit());
                for (const auto& row : rows)
                    *work->logic.add_examples() = row;
                if (rows.size() == page.limit()
                    && !logic.examples(page.definition_id(), rows.back().id(), 1).empty())
                    work->logic.set_next_after(rows.back().id());
            } else
                throw std::invalid_argument("invalid research logic request");
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
    if (call.request.has_resources())
        *call.response.mutable_resources() = std::move(work->catalogue);
    else
        *call.response.mutable_logic() = std::move(work->logic);
}

PolledTask<> ResearchHost::strategy_operation(Call& call)
{
    const auto node = configuration_.node_id;
    co_await wait_for_store({}, call.deadline);
    call.input_in_time();
    const auto request = call.request.strategy();
    const auto catalogue = call.request.resources();
    if (request.has_mutation())
        refuse_during_upgrade(quiescing_);
    resource::proto::Owner owner;
    owner.set_node_id(node);
    owner.set_service_id(configuration_.service);
    owner.set_domain("research");
    // Replay the durable receipt before touching original files or loading modules.
    if (request.has_mutation()
        && (request.mutation().has_freeze() || request.mutation().has_logic())) {
        auto replay = std::make_shared<MetadataWork>();
        metadata_work_ = replay;
        try {
            replay->result = journal_.submit([this, request, owner, replay](std::stop_token) {
                research::Strategies strategies(*storage_, owner);
                if (const auto receipt = strategies.receipt(request.mutation())) {
                    replay->strategy = *receipt;
                    replay->replay = true;
                }
            });
        } catch (...) {
            metadata_work_.reset();
            throw;
        }
        co_await PollUntil{[&] {
            return replay->done;
        }};
        if (replay->error)
            std::rethrow_exception(replay->error);
        if (replay->replay) {
            *call.response.mutable_strategy() = std::move(replay->strategy);
            co_return;
        }
    }
    std::optional<research::proto::StrategyPackage> captured;
    if (request.has_mutation()
        && (request.mutation().has_freeze() || request.mutation().has_logic())) {
        const auto& source = request.mutation().has_logic() ? request.mutation().logic().source()
                                                            : request.mutation().freeze().source();
        validate_id(source.id());
        co_await wait_for_store({}, call.deadline);
        auto slot = verification_slots_.acquire(false, source.id());
        auto read = storage_->prepare_result(source.id());
        const auto input_digest = read.input_digest();
        co_await file_work([&] {
            read.verify();
        });
        co_await ready(call);
        call.input_in_time();
        auto verified = storage_->confirm_result(std::move(read));
        co_await file_work([&] {
            if (request.mutation().has_logic()) {
                const auto module = risk_providers::Module::selected();
                captured = research::capture_logic_strategy_package(
                    owner,
                    request.mutation().logic(),
                    verified,
                    input_digest,
                    module.artifact());
                programs::Files programs(configuration_.directory / "programs", owner);
                (void)programs.preserve(module.path(), module.artifact());
                (void)programs.verify(captured->definition().program_artifact());
                return;
            }
            captured = research::capture_strategy_package(
                owner,
                request.mutation().freeze(),
                verified,
                input_digest);
            programs::Files programs(configuration_.directory / "programs", owner);
            programs.verify(captured->definition().program_artifact());
            risk_providers::Module::pinned(
                configuration_.directory / source.id(),
                captured->definition().risk_artifact());
        });
    }
    co_await wait_for_store({}, call.deadline);
    call.input_in_time();
    if (request.has_mutation())
        refuse_during_upgrade(quiescing_);
    auto work = std::make_shared<MetadataWork>();
    metadata_work_ = work;
    try {
        work->result =
            journal_.submit([this, request, catalogue, owner, captured, work](std::stop_token) {
                research::Strategies strategies(
                    *storage_,
                    owner,
                    request.has_mutation() ? sqlite::Database::Access::writer
                                           : sqlite::Database::Access::read_only);
                if (!catalogue.kind().empty()) {
                    work->catalogue = strategies.resources(catalogue);
                } else if (request.has_mutation()) {
                    work->strategy =
                        strategies.apply(request.mutation(), captured ? &*captured : nullptr);
                } else if (request.has_get()) {
                    *work->strategy.mutable_package() = strategies.package(request.get().id());
                } else if (request.has_delivery()) {
                    work->strategy = strategies.delivery_status(request.delivery().id());
                } else if (request.has_list()) {
                    const auto& page = request.list();
                    if (!page.package_id().empty())
                        throw std::invalid_argument("invalid research strategy list query");
                    const auto rows = strategies.packages(page.after(), page.limit());
                    for (const auto& row : rows)
                        *work->strategy.add_packages() = row;
                    if (rows.size() == page.limit()
                        && !strategies.packages(rows.back().id(), 1).empty())
                        work->strategy.set_next_after(rows.back().id());
                } else if (request.has_deliveries()) {
                    const auto& page = request.deliveries();
                    const auto rows =
                        strategies.deliveries(page.package_id(), page.after(), page.limit());
                    for (const auto& row : rows)
                        *work->strategy.add_deliveries() = row;
                    if (rows.size() == page.limit()
                        && !strategies.deliveries(page.package_id(), rows.back().id(), 1).empty())
                        work->strategy.set_next_after(rows.back().id());
                } else {
                    throw std::invalid_argument("invalid research strategy request");
                }
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
    if (call.request.has_resources())
        *call.response.mutable_resources() = std::move(work->catalogue);
    else
        *call.response.mutable_strategy() = std::move(work->strategy);
}

PolledTask<> ResearchHost::strategy_holders(Call& call, wire::TaskRetirement& preview)
{
    if ((preview.task().kind() != wire::BACKTEST && preview.task().kind() != wire::LOGIC_TRIAL
            && preview.task().kind() != wire::FACTOR
            && preview.task().kind() != wire::MODEL_TRAINING
            && preview.task().kind() != wire::MODEL_TIME_VALIDATION)
        || preview.state() == wire::TaskRetirement::RETIRED)
        co_return;
    co_await wait_for_store({}, call.deadline);
    const auto owner = preview.object().owner();
    const auto id = preview.task().id();
    auto work = std::make_shared<MetadataWork>();
    metadata_work_ = work;
    try {
        work->result = journal_.submit([this, owner, id, work](std::stop_token) {
            research::Strategies strategies(*storage_, owner);
            work->consumers = strategies.source_consumers(id);
            research::Models models(*storage_, owner);
            for (const auto& consumer : models.source_consumers(id))
                work->consumers.push_back(consumer);
            research::Batches batches(*storage_, owner);
            for (const auto& batch : batches.source_consumers(id))
                work->consumers.push_back("batch:" + batch);
            for (const auto& assessment : batches.assessment_consumers(id))
                work->consumers.push_back("batch-assessment:" + assessment);
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
    for (const auto& consumer : work->consumers)
        preview.add_consumers(consumer);
    if (!work->consumers.empty())
        preview.set_state(wire::TaskRetirement::BLOCKED);
}

} // namespace asterion::tasks
