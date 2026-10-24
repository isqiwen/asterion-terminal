#include "backtest/model_strategy.hpp"
#include <asterion/research.pb.h>
#include "research_host.hpp"
#include "project_store.hpp"
#include "variable_store.hpp"
#include "logic_store.hpp"
#include "strategy_store.hpp"
#include "batch_store.hpp"
#include "model_store.hpp"
#include "research_models.hpp"
#include <asterion/protocol/research_model.hpp>
#include "research_batch.hpp"
#include <asterion/protocol/research_batch.hpp>
#include <asterion/protocol/research_assessment.hpp>
#include <asterion/protocol/research_strategy.hpp>
#include <asterion/protocol/research_variable.hpp>
#include "research_execution.hpp"
#include "research_statistics.hpp"
#include "research_evaluation.hpp"
#include <asterion/protocol/research_statistics.hpp>
#include <asterion/protocol/research_evaluation.hpp>
#include <asterion/protocol/research_sample.hpp>
#include <asterion/protocol/research_logic.hpp>
#include "task_payload.hpp"
#include "program_files.hpp"
#include <asterion/protocol/account_review.hpp>
#include <asterion/protocol/notice_review.hpp>
#include <asterion/protocol/intelligence.hpp>
#include <asterion/protocol/trading_facts.hpp>
#include <asterion/protocol/resource.hpp>
#include <unistd.h>
#include <limits>
#include "risk_module.hpp"
#include <asterion/kernel/logger.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/owner.hpp>
#include <asterion/kernel/trace.hpp>
#include <asterion/protocol/data.hpp>
#include <asterion/protocol/data_client.hpp>
#include <asterion/protocol/factor.hpp>
#include <asterion/protocol/task_execution.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <set>
#include <asterion/protocol/rpc_diagnostics.hpp>
#include <algorithm>
#include <stdexcept>

namespace asterion::tasks {

using namespace std::chrono_literals;
namespace wire = task::proto;

namespace {

// The existing task an operation names. Submissions and retries are admitted
// by the store itself and wait for no earlier change of their own.
std::string named_task(const research::proto::ResearchRequest& request)
{
    if (request.has_recover_submission())
        return request.recover_submission().id();
    if (request.has_reconcile_notice_custody())
        return request.reconcile_notice_custody().id();
    if (request.has_notice_source_changes())
        return request.notice_source_changes().id();
    if (request.has_abandon_preparation())
        return request.abandon_preparation().id();
    if (request.has_release_decision())
        return request.release_decision().id();
    if (request.has_retirement())
        return request.retirement().id();
    if (request.has_metadata_get())
        return request.metadata_get().id();
    if (request.has_metadata_edit())
        return request.metadata_edit().id();
    if (request.has_get())
        return request.get().id();
    if (request.has_cancel())
        return request.cancel().id();
    if (request.has_result())
        return request.result().id();
    return {};
}

// New work is not admitted once an upgrade has asked the service to quiesce.
void require_fixed_owner(
    const data::proto::DataOwner& selected,
    const data::proto::DataOwner& fixed)
{
    validate_id(fixed.node_id());
    validate_id(fixed.service_id());
    if ((!selected.node_id().empty() && selected.node_id() != fixed.node_id())
        || (!selected.service_id().empty() && selected.service_id() != fixed.service_id()))
        throw Error(ErrorCode::conflict, "fixed experiment cannot change its data owner");
}

void refuse_during_upgrade(bool quiescing)
{
    if (quiescing)
        throw Error(ErrorCode::unavailable, "research service is preparing for upgrade");
}

} // namespace

struct ResearchHost::Call
{
    research::proto::ResearchRequest request;
    wire::TaskResponse response;
    wire::TaskAttempt attempt;
    Lane lane = Lane::client;
    Clock::time_point deadline, verification_deadline, input_deadline;
    std::string task_id;
    mutable std::optional<ResearchDataRoute> data_route;

    void admitted() const
    {
        if ((lane != Lane::health && service::stop_requested()) || Clock::now() >= deadline)
            throw Error(ErrorCode::unavailable, "task request admission timed out");
    }

    void input_in_time() const
    {
        if (service::stop_requested() || Clock::now() >= input_deadline)
            throw Error(ErrorCode::unavailable, "task input preparation timed out");
    }

    void verified_in_time() const
    {
        if (service::stop_requested() || Clock::now() >= verification_deadline)
            throw Error(ErrorCode::unavailable, "task result verification timed out");
    }
};

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

PolledTask<> ResearchHost::project_operation(Call& call)
{
    const auto node = co_await data_.node_id(
        std::chrono::ceil<std::chrono::milliseconds>(call.input_deadline - Clock::now()),
        call.request.correlation_id());
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
    const auto node = co_await data_.node_id(
        std::chrono::ceil<std::chrono::milliseconds>(call.input_deadline - Clock::now()),
        call.request.correlation_id());
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
                        throw std::out_of_range("model draft does not exist");
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
                        throw std::out_of_range("model draft does not exist");
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
    try {
        const auto existing = storage_->describe(submission.id());
        if (existing.kind() != (time ? wire::MODEL_TIME_VALIDATION : wire::MODEL_TRAINING)
            || existing.has_retirement()
            || (time ? existing.model_time_plan() : existing.model_plan()).SerializeAsString()
                != selected.SerializeAsString())
            throw Error(ErrorCode::conflict, "model task identity names different fixed evidence");
        *call.response.mutable_task() = existing;
        co_return;
    } catch (const std::out_of_range&) {
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
        return time ? storage_->submission(submission.id(), std::move(time_input))
                    : storage_->submission(submission.id(), std::move(input));
    })).task();
}

PolledTask<> ResearchHost::variable_operation(Call& call)
{
    const auto node = co_await data_.node_id(
        std::chrono::ceil<std::chrono::milliseconds>(call.input_deadline - Clock::now()),
        call.request.correlation_id());
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
    const auto node = co_await data_.node_id(
        std::chrono::ceil<std::chrono::milliseconds>(call.input_deadline - Clock::now()),
        call.request.correlation_id());
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
    const auto node = co_await data_.node_id(
        std::chrono::ceil<std::chrono::milliseconds>(call.input_deadline - Clock::now()),
        call.request.correlation_id());
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
        const auto node = co_await data_.node_id(
            std::chrono::ceil<std::chrono::milliseconds>(call.input_deadline - Clock::now()),
            call.request.correlation_id());
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

PolledTask<> ResearchHost::strategy_operation(Call& call)
{
    const auto node = co_await data_.node_id(
        std::chrono::ceil<std::chrono::milliseconds>(call.input_deadline - Clock::now()),
        call.request.correlation_id());
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
    try {
        (void)storage_->describe(id);
        co_return;
    } catch (const std::out_of_range&) {
    }
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

template <class F>
PolledTask<Store::Change> ResearchHost::submit(const Call& call, F make_input, bool prepared_files)
{
    const auto id = call.request.has_submit() ? call.request.submit().id()
                                              : call.request.recover_submission().id();
    std::optional<Store::Submission> input;
    co_await file_work([&] {
        input.emplace(make_input());
    });
    co_await ready(call);
    call.input_in_time();
    refuse_during_upgrade(quiescing_);
    if (!batch_admission_open(call, id))
        throw Error(
            ErrorCode::conflict,
            "batch stopped or exhausted its budget before member admission");
    storage_->admit_submission(*input);
    try {
        data::proto::DataRetention retention;
        co_await file_work([&] {
            if (!prepared_files)
                input->prepare_files();
            if (input->execution().has_account_review() || input->execution().has_notice_review()
                || input->execution().has_event_set_review() || input->execution().has_statistics()
                || input->execution().has_evaluation() || input->execution().has_logic_trial()
                || input->execution().has_model_training() || input->execution().has_model_time())
                return; // Owned fixed evidence; source domains retain their formal publications.
            if (!call.data_route)
                throw std::logic_error("research submission has no resolved data owner");
            retention = research_retention(
                input->execution(),
                *call.data_route,
                configuration_.service,
                id);
            save_research_retention(configuration_.directory / id, retention);
        });
        if (call.request.has_submit() && call.request.submit().derivation().has_batch())
            co_await verify_prepared_batch(call, *input);
        if (input->execution().has_notice_review() || input->execution().has_event_set_review()) {
            const auto binding = notice_consumer_binding(input->execution(), id);
            const auto prepared = co_await notice_custody(
                call,
                binding,
                intelligence::proto::NoticeConsumerRecord::PREPARED);
            co_await file_work([&] {
                save_notice_custody(configuration_.directory / id, prepared);
            });
            co_await ready(call);
            auto accepted = co_await commit(storage_->register_submission(*input));
            try {
                const auto confirmed = co_await notice_custody(
                    call,
                    binding,
                    intelligence::proto::NoticeConsumerRecord::CONFIRMED);
                co_await file_work([&] {
                    save_notice_custody(configuration_.directory / id, confirmed);
                });
            } catch (const std::exception& error) {
                // Durable source PREPARED still holds. Task admission has already committed.
                log_process_event(
                    "research",
                    LogLevel::warning,
                    "notice.custody.confirmation_pending",
                    {{"task_id", id}, {"code", std::string(error_name(classify(error)))}});
            }
            co_return std::move(accepted);
        }
        if (input->execution().has_account_review() || input->execution().has_statistics()
            || input->execution().has_evaluation() || input->execution().has_logic_trial()
            || input->execution().has_model_training() || input->execution().has_model_time()) {
            co_await ready(call);
            if (input->execution().has_model_training()) {
                co_await wait_for_store({}, call.deadline);
                const auto execution = input->execution();
                const auto& fixed = execution.model_training().plan();
                (void)research::Models(*storage_, fixed.owner())
                    .admission(protocol::model_plan_object(fixed), execution.program_artifact());
            }
            if (input->execution().has_model_time()) {
                co_await wait_for_store({}, call.deadline);
                const auto execution = input->execution();
                const auto& fixed = execution.model_time().plan();
                (void)research::Models(*storage_, fixed.owner())
                    .time_admission(
                        protocol::model_time_plan_object(fixed),
                        execution.program_artifact());
            }
            if (input->execution().has_logic_trial()) {
                co_await wait_for_store({}, call.deadline);
                const auto fixed = input->execution().logic_trial();
                research::Logic(*storage_, fixed.owner()).verify_input(fixed, true);
            }
            co_return co_await commit(storage_->register_submission(*input));
        }
        data::proto::DataRequest retain;
        *retain.mutable_retention()->mutable_prepare() = retention;
        const auto retained = co_await data_call(
            *call.data_route,
            std::move(retain),
            5s,
            call.request.correlation_id());
        if (retained.retention().retention().SerializeAsString() != retention.SerializeAsString()
            || (retained.retention().phase() != data::proto::DataRetentionRecord::PREPARED
                && retained.retention().phase() != data::proto::DataRetentionRecord::CONFIRMED))
            throw std::invalid_argument("data did not confirm retention of the fixed inputs");
        // Admission has reserved the task's capacity and order. Complete
        // that accepted submission even if its client deadline has passed.
        co_await ready(call);
        if (input->execution().has_strategy_use()) {
            co_await wait_for_store({}, call.deadline);
            research::Strategies(*storage_, input->execution().strategy_use().package().owner())
                .verify_use(input->execution().strategy_use(), id, true);
        }
        if (input->execution().has_model_replay()) {
            co_await wait_for_store({}, call.deadline);
            const auto definition = input->execution().model_replay();
            research::Models models(*storage_, definition.mapping().owner());
            if (models.mapping(definition.mapping().id()).SerializeAsString()
                != definition.mapping().SerializeAsString())
                throw Error(ErrorCode::conflict, "model replay mapping changed before admission");
            (void)models.time_admission(
                protocol::model_time_plan_object(definition.training().plan()),
                input->execution().program_artifact());
        }
        while (call.request.has_submit() && call.request.submit().derivation().has_batch()) {
            bool existing = false;
            try {
                (void)storage_->describe(id);
                existing = true;
            } catch (const std::out_of_range&) {
            }
            if (existing || batch_admission_open(call, id))
                break;
            co_await verify_prepared_batch(call, *input);
        }
        co_return co_await commit(storage_->register_submission(*input));
    } catch (...) {
        if (storage_->abandon_submission(*input))
            degraded_ = true;
        throw;
    }
}

PolledTask<> ResearchHost::recover_submission(Call& call)
{
    const auto id = call.request.recover_submission().id();
    validate_id(id);
    try {
        *call.response.mutable_task() = storage_->describe(id);
        co_return; // A durable accepted task needs no source connection to replay this request.
    } catch (const std::out_of_range&) {
        // Only an absent task may consume its owned preparation record.
    }
    if (storage_->abandoned_preparation(id))
        throw Error(ErrorCode::conflict, "preparation was durably abandoned");
    wire::PreparedTaskSubmission prepared;
    co_await file_work([&] {
        prepared = storage_->prepared_submission_record(id);
    });
    call.input_in_time();
    if (prepared.record().task().metadata().derivation().has_batch())
        throw Error(
            ErrorCode::conflict,
            "unadmitted batch preparation requires explicit abandonment and a new batch");
    if (prepared.execution().has_strategy_use()) {
        co_await wait_for_store({}, call.deadline);
        research::Strategies(*storage_, prepared.execution().strategy_use().package().owner())
            .verify_use(prepared.execution().strategy_use(), id, true);
    }
    if (prepared.record().task().has_research_plan())
        co_await verify_plan(call, prepared.record().task().research_plan());
    if (prepared.execution().has_variable_definition()) {
        const auto object = protocol::variable_object(prepared.execution().variable_definition());
        (void)co_await resolve_variable(call, object);
    }
    if (prepared.execution().has_event_set_review()) {
        const auto& fixed = prepared.execution().event_set_review();
        protocol::validate_event_set_review_execution(fixed);
        const auto node = co_await data_.node_id(5s, call.request.correlation_id());
        if (fixed.owner().node_id() != node || fixed.owner().service_id() != configuration_.service)
            throw Error(
                ErrorCode::conflict,
                "event set review preparation belongs to another owner");
        bool missing = false;
        co_await file_work([&] {
            (void)programs::Files(configuration_.directory / "programs", fixed.owner())
                .verify(fixed.program_artifact());
            missing = !std::filesystem::exists(configuration_.directory / id / "input.pb");
        });
        std::optional<research::proto::EventSetReviewInput> recovered;
        if (missing)
            recovered = co_await event_set_evidence(
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
                        "recovered event set evidence differs from preparation");
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
    if (prepared.execution().has_notice_review()) {
        const auto& fixed = prepared.execution().notice_review();
        protocol::validate_notice_review_execution(fixed);
        const auto node = co_await data_.node_id(5s, call.request.correlation_id());
        if (fixed.owner().node_id() != node || fixed.owner().service_id() != configuration_.service)
            throw Error(ErrorCode::conflict, "notice review preparation belongs to another owner");
        bool missing = false;
        co_await file_work([&] {
            (void)programs::Files(configuration_.directory / "programs", fixed.owner())
                .verify(fixed.program_artifact());
            missing = !std::filesystem::exists(configuration_.directory / id / "input.pb");
        });
        std::optional<research::proto::NoticeReviewInput> recovered;
        if (missing)
            recovered = co_await notice_evidence(
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
    if (prepared.execution().has_account_review()) {
        const auto& fixed = prepared.execution().account_review();
        protocol::validate_account_review_execution(fixed);
        const auto node = co_await data_.node_id(5s, call.request.correlation_id());
        if (fixed.owner().node_id() != node || fixed.owner().service_id() != configuration_.service)
            throw Error(ErrorCode::conflict, "account review preparation belongs to another owner");
        bool missing = false;
        co_await file_work([&] {
            (void)programs::Files(configuration_.directory / "programs", fixed.owner())
                .verify(fixed.program_artifact());
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
        const auto node = co_await data_.node_id(5s, call.request.correlation_id());
        if (fixed.owner().node_id() != node || fixed.owner().service_id() != configuration_.service)
            throw Error(ErrorCode::conflict, "invalid experiment derivation");
        std::optional<Store::Submission> input;
        co_await file_work([&] {
            (void)programs::Files(configuration_.directory / "programs", fixed.owner())
                .verify(prepared.execution().program_artifact());
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
    if (prepared.execution().has_model_training()) {
        const auto& fixed = prepared.execution().model_training();
        protocol::validate_model_training_input(fixed);
        const auto node = co_await data_.node_id(5s, call.request.correlation_id());
        if (fixed.plan().owner().node_id() != node
            || fixed.plan().owner().service_id() != configuration_.service)
            throw Error(
                ErrorCode::conflict,
                "model preparation belongs to another Research service");
        co_await wait_for_store({}, call.deadline);
        (void)research::Models(*storage_, fixed.plan().owner())
            .admission(
                protocol::model_plan_object(fixed.plan()),
                prepared.execution().program_artifact());
        std::optional<Store::Submission> input;
        co_await file_work([&] {
            (void)programs::Files(configuration_.directory / "programs", fixed.plan().owner())
                .verify(prepared.execution().program_artifact());
            const auto path = configuration_.directory / id / "input.pb";
            if (!std::filesystem::exists(path)) {
                const auto restored = payload::publish(path, fixed, payload::max_input_bytes);
                if (restored.digest != prepared.record().input_digest()
                    || restored.size != prepared.record().input_bytes())
                    throw std::invalid_argument(
                        "reconstructed model descriptor differs from preparation");
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
    if (prepared.execution().has_model_time()) {
        const auto& fixed = prepared.execution().model_time();
        protocol::validate_model_time_input(fixed);
        const auto node = co_await data_.node_id(5s, call.request.correlation_id());
        if (fixed.plan().owner().node_id() != node
            || fixed.plan().owner().service_id() != configuration_.service)
            throw Error(
                ErrorCode::conflict,
                "model preparation belongs to another Research service");
        co_await wait_for_store({}, call.deadline);
        (void)research::Models(*storage_, fixed.plan().owner())
            .time_admission(
                protocol::model_time_plan_object(fixed.plan()),
                prepared.execution().program_artifact());
        std::optional<Store::Submission> input;
        co_await file_work([&] {
            (void)programs::Files(configuration_.directory / "programs", fixed.plan().owner())
                .verify(prepared.execution().program_artifact());
            const auto path = configuration_.directory / id / "input.pb";
            if (!std::filesystem::exists(path)) {
                const auto restored = payload::publish(path, fixed, payload::max_input_bytes);
                if (restored.digest != prepared.record().input_digest()
                    || restored.size != prepared.record().input_bytes())
                    throw std::invalid_argument(
                        "reconstructed model descriptor differs from preparation");
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
        const auto node = co_await data_.node_id(5s, call.request.correlation_id());
        if (owner.node_id() != node || owner.service_id() != configuration_.service)
            throw Error(ErrorCode::conflict, "invalid experiment derivation");
        std::optional<Store::Submission> input;
        co_await file_work([&] {
            (void)programs::Files(configuration_.directory / "programs", owner)
                .verify(prepared.execution().program_artifact());
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
        const auto node = co_await data_.node_id(5s, call.request.correlation_id());
        if (fixed.owner().node_id() != node || fixed.owner().service_id() != configuration_.service)
            throw Error(ErrorCode::conflict, "invalid experiment derivation");
        std::optional<Store::Submission> input;
        co_await file_work([&] {
            (void)programs::Files(configuration_.directory / "programs", fixed.owner())
                .verify(prepared.execution().program_artifact());
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
    try {
        const auto task = storage_->describe(requested.id());
        reply->set_state(
            task.kind() != wire::NOTICE_REVIEW && task.kind() != wire::EVENT_SET_REVIEW
                    && task.has_retirement()
                    && task.retirement().SerializeAsString() == requested.SerializeAsString()
                ? resource::proto::ReleaseDecision::RELEASED
                : resource::proto::ReleaseDecision::RETAINED);
        return;
    } catch (const std::out_of_range&) {
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
    try {
        (void)storage_->describe(id);
        throw Error(ErrorCode::conflict, "admitted task cannot be abandoned as a preparation");
    } catch (const std::out_of_range&) {
    }
    auto released = storage_->abandoned_preparation(id);
    if (!released) {
        wire::PreparedTaskSubmission prepared;
        co_await file_work([&] {
            prepared = storage_->prepared_submission_record(id);
        });
        call.input_in_time();
        const auto node = co_await data_.node_id(
            std::chrono::ceil<std::chrono::milliseconds>(call.input_deadline - Clock::now()),
            call.request.correlation_id());
        if (prepared.execution().has_event_set_review()) {
            const auto& fixed = prepared.execution().event_set_review();
            protocol::validate_event_set_review_execution(fixed);
            if (fixed.owner().node_id() != node
                || fixed.owner().service_id() != configuration_.service)
                throw Error(
                    ErrorCode::conflict,
                    "event set review preparation belongs to another owner");
            resource::proto::Object object;
            *object.mutable_owner() = fixed.owner();
            object.set_kind("event_set_review");
            object.set_id(id);
            object.set_version(prepared.record().input_digest());
            co_await ready(call);
            call.input_in_time();
            refuse_during_upgrade(quiescing_);
            (void)co_await commit(storage_->abandon_preparation(prepared, object));
            try {
                const auto binding = notice_consumer_binding(prepared.execution(), id);
                const auto receipt = co_await notice_custody(
                    call,
                    binding,
                    intelligence::proto::NoticeConsumerRecord::RELEASED);
                co_await file_work([&] {
                    if (std::filesystem::is_directory(configuration_.directory / id))
                        save_notice_custody(configuration_.directory / id, receipt);
                });
            } catch (const std::exception& error) {
                log_process_event(
                    "research",
                    LogLevel::warning,
                    "notice.custody.release_pending",
                    {{"task_id", id}, {"code", std::string(error_name(classify(error)))}});
            }
            auto* reply = call.response.mutable_release_decision();
            *reply->mutable_object() = object;
            reply->set_state(resource::proto::ReleaseDecision::RELEASED);
            co_return;
        }
        if (prepared.execution().has_notice_review()) {
            const auto& fixed = prepared.execution().notice_review();
            protocol::validate_notice_review_execution(fixed);
            if (fixed.owner().node_id() != node
                || fixed.owner().service_id() != configuration_.service)
                throw Error(
                    ErrorCode::conflict,
                    "notice review preparation belongs to another owner");
            resource::proto::Object object;
            *object.mutable_owner() = fixed.owner();
            object.set_kind("notice_review");
            object.set_id(id);
            object.set_version(prepared.record().input_digest());
            co_await ready(call);
            call.input_in_time();
            refuse_during_upgrade(quiescing_);
            (void)co_await commit(storage_->abandon_preparation(prepared, object));
            try {
                const auto binding = notice_consumer_binding(prepared.execution(), id);
                const auto receipt = co_await notice_custody(
                    call,
                    binding,
                    intelligence::proto::NoticeConsumerRecord::RELEASED);
                co_await file_work([&] {
                    if (std::filesystem::is_directory(configuration_.directory / id))
                        save_notice_custody(configuration_.directory / id, receipt);
                });
            } catch (const std::exception& error) {
                log_process_event(
                    "research",
                    LogLevel::warning,
                    "notice.custody.release_pending",
                    {{"task_id", id}, {"code", std::string(error_name(classify(error)))}});
            }
            auto* reply = call.response.mutable_release_decision();
            *reply->mutable_object() = object;
            reply->set_state(resource::proto::ReleaseDecision::RELEASED);
            co_return;
        }
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
        if (prepared.execution().has_model_training()) {
            const auto& fixed = prepared.execution().model_training();
            protocol::validate_model_training_input(fixed);
            if (fixed.plan().owner().node_id() != node
                || fixed.plan().owner().service_id() != configuration_.service)
                throw Error(
                    ErrorCode::conflict,
                    "model preparation belongs to another Research service");
            resource::proto::Object object;
            *object.mutable_owner() = fixed.plan().owner();
            object.set_kind("model_training");
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
        if (prepared.execution().has_model_time()) {
            const auto& fixed = prepared.execution().model_time();
            protocol::validate_model_time_input(fixed);
            if (fixed.plan().owner().node_id() != node
                || fixed.plan().owner().service_id() != configuration_.service)
                throw Error(
                    ErrorCode::conflict,
                    "model preparation belongs to another Research service");
            resource::proto::Object object;
            *object.mutable_owner() = fixed.plan().owner();
            object.set_kind("model_time_validation");
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
    if (released->kind() == "notice_review" || released->kind() == "event_set_review") {
        const auto prepared = storage_->abandoned_preparation_record(id);
        try {
            const auto binding = notice_consumer_binding(prepared.execution(), id);
            const auto receipt = co_await notice_custody(
                call,
                binding,
                intelligence::proto::NoticeConsumerRecord::RELEASED);
            co_await file_work([&] {
                if (std::filesystem::is_directory(configuration_.directory / id))
                    save_notice_custody(configuration_.directory / id, receipt);
            });
        } catch (const std::exception& error) {
            log_process_event(
                "research",
                LogLevel::warning,
                "notice.custody.release_pending",
                {{"task_id", id}, {"code", std::string(error_name(classify(error)))}});
        }
    }
    auto* reply = call.response.mutable_release_decision();
    *reply->mutable_object() = *released;
    reply->set_state(resource::proto::ReleaseDecision::RELEASED);
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

PolledTask<> ResearchHost::retirement(Call& call)
{
    const auto& request = call.request.retirement();
    validate_id(request.id());
    const auto node = co_await data_.node_id(
        std::chrono::ceil<std::chrono::milliseconds>(call.input_deadline - Clock::now()),
        call.request.correlation_id());
    co_await ready(call);
    call.input_in_time();
    auto preview = storage_->retirement(request.id(), node);
    co_await strategy_holders(call, preview);
    if (storage_->describe(request.id()).kind() == wire::MODEL_TRAINING) {
        wire::PreparedTaskSubmission prepared;
        co_await file_work([&] {
            prepared = storage_->prepared_submission_record(request.id());
        });
        const auto& fixed = prepared.execution().model_training();
        protocol::validate_model_training_input(fixed);
        if (fixed.plan().owner().node_id() != node
            || fixed.plan().owner().service_id() != configuration_.service
            || preview.object().owner().SerializeAsString()
                != fixed.plan().owner().SerializeAsString()
            || preview.object().version() != prepared.record().input_digest()
            || preview.object().kind() != "model_training")
            throw Error(ErrorCode::conflict, "model retirement original identity changed");
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
        // Same-domain reference authority; logical retirement preserves all evidence.
        preview.set_source_release_confirmed(preview.state() == wire::TaskRetirement::RETIRED);
        *call.response.mutable_retirement() = std::move(preview);
        co_return;
    }
    if (storage_->describe(request.id()).kind() == wire::MODEL_TIME_VALIDATION) {
        wire::PreparedTaskSubmission prepared;
        co_await file_work([&] {
            prepared = storage_->prepared_submission_record(request.id());
        });
        const auto& fixed = prepared.execution().model_time();
        protocol::validate_model_time_input(fixed);
        if (fixed.plan().owner().node_id() != node
            || fixed.plan().owner().service_id() != configuration_.service
            || preview.object().owner().SerializeAsString()
                != fixed.plan().owner().SerializeAsString()
            || preview.object().version() != prepared.record().input_digest()
            || preview.object().kind() != "model_time_validation")
            throw Error(ErrorCode::conflict, "model retirement original identity changed");
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
        // Same-domain reference authority; logical retirement preserves all evidence.
        preview.set_source_release_confirmed(preview.state() == wire::TaskRetirement::RETIRED);
        *call.response.mutable_retirement() = std::move(preview);
        co_return;
    }
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
    if (storage_->describe(request.id()).kind() == wire::EVENT_SET_REVIEW) {
        wire::PreparedTaskSubmission prepared;
        co_await file_work([&] {
            prepared = storage_->prepared_submission_record(request.id());
        });
        const auto& fixed = prepared.execution().event_set_review();
        protocol::validate_event_set_review_execution(fixed);
        if (fixed.owner().node_id() != node || fixed.owner().service_id() != configuration_.service
            || preview.object().owner().SerializeAsString() != fixed.owner().SerializeAsString()
            || preview.object().version() != prepared.record().input_digest()
            || preview.object().kind() != "event_set_review")
            throw Error(ErrorCode::conflict, "event set review retirement identity changed");
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
                "Intelligence publications and Data content remain retained; source release is not supported");
        }
        *call.response.mutable_retirement() = std::move(preview);
        co_return;
    }
    if (storage_->describe(request.id()).kind() == wire::NOTICE_REVIEW) {
        wire::PreparedTaskSubmission prepared;
        co_await file_work([&] {
            prepared = storage_->prepared_submission_record(request.id());
        });
        const auto& fixed = prepared.execution().notice_review();
        protocol::validate_notice_review_execution(fixed);
        if (fixed.owner().node_id() != node || fixed.owner().service_id() != configuration_.service
            || preview.object().owner().SerializeAsString() != fixed.owner().SerializeAsString()
            || preview.object().version() != prepared.record().input_digest()
            || preview.object().kind() != "notice_review")
            throw Error(ErrorCode::conflict, "notice review retirement identity changed");
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
                "Intelligence publications and Data content remain retained; source release is not supported");
        }
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
            std::optional<wire::Task> task;
            try {
                task = storage_->describe(receipt.task_id());
            } catch (const std::out_of_range&) {
            }
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
        try {
            const auto actual = storage_->describe(target);
            admitted = actual.metadata().derivation().batch().SerializeAsString()
                == protocol::backtest_batch_object(value.definition()).SerializeAsString();
        } catch (const std::out_of_range&) {
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
PolledTask<> ResearchHost::submit_backtest(Call& call)
{
    const auto& p = call.request.submit();
    const auto& b = p.backtest();
    if (p.has_research_plan())
        co_await verify_plan(call, p.research_plan());
    std::optional<research::proto::StrategyPackage> candidate;
    auto use = p.strategy_use();
    if (p.has_strategy_use()) {
        const auto digest = sha256_bytes(b.SerializeAsString());
        if (!use.request_sha256().empty() && use.request_sha256() != digest)
            throw Error(ErrorCode::conflict, "strategy delivery request identity mismatch");
        use.set_request_sha256(digest);
        protocol::validate_strategy_use(use);
        const auto node = co_await data_.node_id(5s, call.request.correlation_id());
        if (use.package().owner().node_id() != node
            || use.package().owner().service_id() != configuration_.service)
            throw Error(ErrorCode::conflict, "strategy delivery belongs to another Research owner");
        co_await wait_for_store({}, call.deadline);
        research::Strategies strategies(*storage_, use.package().owner());
        candidate = strategies.package(use.package().id());
        if (!candidate->has_logic() && !candidate->definition().has_portfolio())
            throw std::invalid_argument(
                "reference delivery requires unchanged fixed backtest reuse");
        strategies.verify_use(use, p.id(), false);
        if (strategies.delivery_status(use.delivery().id()).target_accepted()) {
            const auto task = storage_->describe(p.id());
            if (task.has_retirement()
                || task.strategy_use().SerializeAsString() != use.SerializeAsString()
                || task.research_plan().SerializeAsString()
                    != p.research_plan().SerializeAsString())
                throw Error(
                    ErrorCode::conflict,
                    "strategy delivery target names different fixed conditions");
            auto read = storage_->prepare_input(p.id());
            co_await file_work([&] {
                read.load_for_claim();
            });
            co_await ready(call);
            call.input_in_time();
            const auto& owner = p.data_owner();
            const auto& fixed = read.execution().data_owner();
            if ((!owner.node_id().empty() && owner.node_id() != fixed.node_id())
                || (!owner.service_id().empty() && owner.service_id() != fixed.service_id()))
                throw Error(ErrorCode::conflict, "strategy delivery Data owner changed");
            *call.response.mutable_task() = storage_->describe(p.id());
            co_return;
        }
        strategies.verify_use(use, p.id(), true);
    }
    // A product of the daily table brings its own months.
    if (static_cast<std::size_t>(b.contracts_size()) > max_account_contracts
        || (b.contracts().empty() && std::ranges::none_of(b.series(), [](const auto& series) {
               return series.has_product();
           })))
        throw std::invalid_argument("backtest requires 1 to 256 contracts");
    // Months of a dominant series are resolved together by Data.
    std::vector<bool> in_series(static_cast<std::size_t>(b.contracts_size()));
    for (const auto& series : b.series()) {
        if (series.has_product()) {
            if (series.contracts_size())
                throw std::invalid_argument("a dominant series names its months or its product");
            continue;
        }
        if (series.contracts_size() < 2)
            throw std::invalid_argument("a dominant series requires at least two month contracts");
        for (const auto index : series.contracts()) {
            if (index >= in_series.size() || in_series[index])
                throw std::invalid_argument("invalid dominant series contracts");
            in_series[index] = true;
        }
    }

    // The frozen contracts: ordinary ones in request order, then the dominant
    // months of each series, each with what it trades at.
    struct Frozen
    {
        data::proto::BarDataset dataset;
        const protocol::proto::CostSchedule* costs;
        unsigned slippage;
    };

    std::vector<Frozen> datasets;
    std::size_t total_bars = 0;
    const auto freeze = [&](data::proto::BarDataset dataset,
                            const protocol::proto::CostSchedule& costs,
                            unsigned slippage) {
        total_bars += static_cast<std::size_t>(dataset.bars_size());
        if (total_bars > protocol::max_dataset_bars)
            throw std::invalid_argument("dataset exceeds 200000 bars; narrow the date range");
        datasets.push_back({std::move(dataset), &costs, slippage});
    };
    const auto keep = [&](std::size_t index, data::proto::BarDataset dataset) {
        const auto& contract = b.contracts(static_cast<int>(index));
        freeze(std::move(dataset), contract.cost_schedule(), contract.slippage_ticks());
    };
    for (std::size_t c = 0; c < in_series.size(); ++c) {
        if (in_series[c])
            continue;
        data::proto::DataRequest query;
        *query.mutable_bar_dataset() = b.contracts(static_cast<int>(c)).data();
        auto prepared = co_await data_input(call, std::move(query));
        keep(c, std::move(*prepared.mutable_bar_dataset()));
    }
    std::vector<data::proto::DominantSchedule> schedules;
    for (const auto& series : b.series()) {
        data::proto::DataRequest query;
        if (series.has_product())
            *query.mutable_dominant_series()->mutable_product() = series.product().data();
        for (const auto index : series.contracts())
            *query.mutable_dominant_series()->add_months() = b.contracts(index).data();
        auto prepared = co_await data_input(call, std::move(query));
        auto& resolved = *prepared.mutable_dominant_series();
        if (resolved.months_size() != resolved.datasets_size())
            throw std::invalid_argument("dominant series response has inconsistent months");
        for (auto& roll : *resolved.mutable_schedule()->mutable_rolls())
            roll.set_contract(roll.contract() + static_cast<unsigned>(datasets.size()));
        for (int m = 0; m < resolved.months_size(); ++m) {
            // A product's months are Data's to find; all trade at the product's costs.
            if (series.has_product()) {
                freeze(
                    std::move(*resolved.mutable_datasets(m)),
                    series.product().cost_schedule(),
                    series.product().slippage_ticks());
                continue;
            }
            if (resolved.months(m) >= static_cast<unsigned>(series.contracts_size()))
                throw std::invalid_argument("dominant series response has an unknown month");
            keep(series.contracts(resolved.months(m)), std::move(*resolved.mutable_datasets(m)));
        }
        schedules.push_back(std::move(*resolved.mutable_schedule()));
    }
    backtest::proto::BacktestInput input;
    *input.mutable_data_owner() = call.data_route->owner;
    auto* paper = input.mutable_paper();
    *paper->mutable_deposit() = b.deposit();
    *paper->mutable_risk() = b.risk();
    for (auto& frozen : datasets) {
        auto* contract = paper->add_contracts();
        *contract->mutable_dataset() = std::move(frozen.dataset);
        *contract->mutable_cost_schedule() = *frozen.costs;
        contract->set_slippage_ticks(frozen.slippage);
    }
    for (auto& schedule : schedules)
        *input.add_series() = std::move(schedule);
    *input.mutable_settlement_margins() = b.settlement_margins();
    protocol::set_backtest_strategies(input, b);
    if (!b.margin_snapshot().empty()) {
        if (paper->contracts_size() != 1 || !b.settlement_margins().empty())
            throw std::invalid_argument(
                "historical margins require one contract and no manual settlement events");
        data::proto::DataRequest query;
        auto* margins = query.mutable_historical_margins();
        margins->set_snapshot(b.margin_snapshot());
        margins->set_contract(
            protocol::history_identity(paper->contracts(0).dataset().contract()).key());
        for (const auto& day : paper->contracts(0).dataset().days())
            margins->add_trading_days(day.trading_day());
        const auto prepared = co_await data_input(call, std::move(query));
        if (prepared.historical_margins().snapshot() != b.margin_snapshot())
            throw std::invalid_argument("historical margin snapshot is unavailable");
        protocol::apply_historical_margins(input, prepared.historical_margins());
    }

    co_await ready(call);
    call.input_in_time();
    refuse_during_upgrade(quiescing_);
    const auto programme = co_await program(call);
    call.input_in_time();
    *call.response.mutable_task() = (co_await submit(call, [&] {
        input.set_dataset_revision(protocol::dataset_revision(input.paper()));
        auto submission = storage_->submission(p.id(), std::move(input));
        submission.set_program_artifact(programme.artifact);
        if (p.has_research_plan())
            submission.set_research_plan(p.research_plan());
        if (candidate)
            submission.set_strategy_use(use, candidate->definition());
        return submission;
    })).task();
}

PolledTask<> ResearchHost::reuse_backtest(Call& call)
{
    const auto& p = call.request.submit();
    const auto& reuse = p.backtest_reuse();
    if (reuse.has_slippage_ticks() && reuse.has_fee_multiplier())
        throw std::invalid_argument("fee and slippage scenarios require separate requests");
    std::optional<research::proto::BacktestBatchDefinition> batch;
    if (reuse.has_model_replay()
        && (reuse.choice() != backtest::proto::BacktestReuse::FIXED
            || (reuse.has_slippage_ticks() || reuse.has_fee_multiplier()) || p.has_strategy_use()
            || p.has_derivation()))
        throw std::invalid_argument(
            "model replay requires unchanged fixed data and an explicit standalone request");
    using Choice = backtest::proto::BacktestReuse;
    if (p.id() == reuse.source_id())
        throw std::invalid_argument("experiment reuse requires a new task id");
    if ((reuse.choice() != Choice::FIXED && reuse.choice() != Choice::NEW_SNAPSHOT)
        || (reuse.choice() == Choice::FIXED ? !reuse.snapshot().empty()
                                            : reuse.snapshot().size() != 64
                    || reuse.snapshot().find_first_not_of("0123456789abcdef") != std::string::npos))
        throw std::invalid_argument("explicit experiment data choice required");
    if ((reuse.has_slippage_ticks() || reuse.has_fee_multiplier())
        && reuse.choice() != Choice::FIXED)
        throw std::invalid_argument("cost sensitivity requires the original fixed data");
    std::optional<research::proto::StrategyPackage> candidate;
    if (p.has_strategy_use()) {
        if (reuse.choice() != Choice::FIXED
            || (reuse.has_slippage_ticks() || reuse.has_fee_multiplier()))
            throw std::invalid_argument("strategy delivery requires unchanged fixed evidence");
        const auto& use = p.strategy_use();
        protocol::validate_strategy_use(use);
        const auto node = co_await data_.node_id(5s, call.request.correlation_id());
        if (use.package().owner().node_id() != node
            || use.package().owner().service_id() != configuration_.service)
            throw Error(ErrorCode::conflict, "strategy delivery belongs to another Research owner");
        co_await wait_for_store({}, call.deadline);
        research::Strategies strategies(*storage_, use.package().owner());
        strategies.verify_use(use, p.id(), false);
        candidate = strategies.package(use.package().id());
        if (candidate->has_logic())
            throw std::invalid_argument(
                "logic delivery requires a new explicit backtest configuration");
        const auto draft = strategies.delivery(use.delivery().id());
        if (draft.reference_result().id() != reuse.source_id()
            || draft.reference_result().version() != reuse.source_result_digest()
            || draft.source_input_sha256() != reuse.source_input_sha256())
            throw Error(ErrorCode::conflict, "strategy delivery source evidence mismatch");
        if (strategies.delivery_status(draft.id()).target_accepted()) {
            *call.response.mutable_task() = storage_->describe(p.id());
            co_return; // Existing admission survives deprecation and a disconnected Data owner.
        }
        strategies.verify_use(use, p.id(), true);
        const auto programme = co_await program(call);
        if (programme.artifact != candidate->definition().program_artifact())
            throw Error(
                ErrorCode::conflict,
                "strategy delivery requires its fixed execution program");
    }
    if (p.derivation().has_batch()) {
        auto fields = p.derivation();
        fields.clear_batch();
        if (fields.ByteSizeLong() || p.has_strategy_use() || p.has_research_plan())
            throw std::invalid_argument("batch reuse accepts only its fixed member binding");
        const auto object = p.derivation().batch();
        co_await wait_for_store({}, call.deadline);
        research::Batches batches(*storage_, object.owner());
        const auto current = batches.state(object.id());
        batch = current.definition();
        if (protocol::backtest_batch_object(*batch).SerializeAsString()
                != object.SerializeAsString()
            || protocol::backtest_batch_member_reuse(*batch, p.id()).SerializeAsString()
                != reuse.SerializeAsString())
            throw Error(
                ErrorCode::conflict,
                "backtest request differs from its fixed batch member");
        require_fixed_owner(p.data_owner(), batch->data_owner());
        bool existing = false;
        try {
            existing = !storage_->describe(p.id()).has_retirement();
        } catch (const std::out_of_range&) {
        }
        if (existing) {
            (void)co_await batch_metadata(
                call,
                object.owner(),
                true,
                [object, id = p.id()](auto& batches, auto& reply) {
                    *reply.mutable_state() = batches.confirm_member(object.id(), id);
                });
            co_await ready(call);
            *call.response.mutable_task() = storage_->describe(p.id());
            co_return;
        }
    }
    co_await wait_for_store(reuse.source_id());
    auto source_slot = verification_slots_.acquire(false, reuse.source_id());
    auto read = storage_->prepare_result(reuse.source_id());
    co_await file_work([&] {
        read.verify();
    });
    co_await ready(call);
    call.input_in_time();
    auto verified = storage_->confirm_result(std::move(read));
    const auto& task = verified.result_task();
    const auto source_program = task.program_artifact();
    if (task.kind() != wire::BACKTEST || !task.has_input() || !verified.has_backtest())
        throw std::invalid_argument("backtest reuse requires a compatible backtest result");
    if (task.result_digest() != reuse.source_result_digest())
        throw std::invalid_argument("experiment evidence changed; reopen its result before reuse");
    if (reuse.choice() == Choice::FIXED) {
        const auto owner = task.input().data_owner();
        require_fixed_owner(p.data_owner(), owner);
        *call.request.mutable_submit()->mutable_data_owner() = owner;
    }
    backtest::proto::BacktestInput original, input;
    std::string original_bytes;
    co_await file_work([&] {
        if ((reuse.has_slippage_ticks() || reuse.has_fee_multiplier())
            && task.risk_artifact() != risk_providers::Module::selected().artifact())
            throw std::invalid_argument(
                "cost sensitivity requires the original risk implementation");
        if (candidate) {
            const auto definition = protocol::strategy_definition(
                task.input(),
                task.program_artifact(),
                task.risk_artifact());
            if (definition.SerializeAsString() != candidate->definition().SerializeAsString()
                || task.risk_artifact() != risk_providers::Module::selected().artifact())
                throw Error(
                    ErrorCode::conflict,
                    "strategy delivery definition or risk implementation changed");
        }
        original = std::move(*verified.mutable_result_task()->mutable_input());
        original_bytes = original.SerializeAsString();
        if (sha256_bytes(original_bytes) != reuse.source_input_sha256())
            throw std::invalid_argument(
                "experiment evidence changed; reopen its result before reuse");
        verified.Clear();
        if (reuse.choice() == Choice::FIXED)
            input = std::move(original);
        else {
            input = original;
            input.mutable_paper()->clear_contracts();
            input.clear_series();
        }
    });
    co_await ready(call);
    call.input_in_time();
    if (reuse.has_model_replay()) {
        co_await wait_for_store({}, call.deadline);
        bool accepted = false;
        try {
            (void)storage_->describe(p.id());
            accepted = true;
        } catch (const std::out_of_range&) {
        }
        if (accepted) {
            auto existing_read = storage_->prepare_input(p.id());
            co_await file_work([&] {
                existing_read.load();
            });
            co_await ready(call);
            const auto existing = storage_->confirm_input(std::move(existing_read));
            if (!existing.has_input() || !existing.input().has_model_replay()
                || existing.has_retirement())
                throw Error(ErrorCode::conflict, "task id already has another experiment");
            const auto& fixed = existing.input().model_replay();
            research::proto::ModelReplaySelection original_selection;
            *original_selection.mutable_mapping() = protocol::model_mapping_object(fixed.mapping());
            original_selection.set_fold_index(fixed.fold_index());
            *original_selection.mutable_quantity() = fixed.quantity();
            original_selection.set_sides(fixed.sides());
            original_selection.set_processing_delay_bars(fixed.processing_delay_bars());
            original_selection.set_time_budget_seconds(fixed.time_budget_seconds());
            original_selection.set_availability(fixed.availability());
            auto expected = input;
            expected.clear_strategies();
            *expected.mutable_model_replay() = fixed;
            const auto& derivation = existing.metadata().derivation();
            if (original_selection.SerializeAsString() != reuse.model_replay().SerializeAsString()
                || expected.SerializeAsString() != existing.input().SerializeAsString()
                || derivation.source_id() != reuse.source_id()
                || derivation.source_result_digest() != reuse.source_result_digest()
                || derivation.data_choice() != "fixed"
                || existing.research_plan().SerializeAsString()
                    != p.research_plan().SerializeAsString())
                throw Error(
                    ErrorCode::conflict,
                    "task id already has another model replay request");
            *call.response.mutable_task() = storage_->describe(p.id());
            co_return; // The exact accepted request needs neither Data nor a new execution.
        }
    }
    const auto selected = [&](const data::proto::BarDataset& dataset) {
        auto selection = protocol::bar_dataset_selection(dataset);
        if (reuse.choice() == Choice::NEW_SNAPSHOT)
            selection.mutable_daily()->set_snapshot(reuse.snapshot());
        return selection;
    };
    const auto keep = [&](const protocol::proto::PaperContract& source,
                          data::proto::BarDataset dataset) {
        if (dataset.contract().SerializeAsString()
            != source.dataset().contract().SerializeAsString())
            throw std::invalid_argument(
                "new snapshot changes contract specifications; configure a new backtest");
        auto* target = input.mutable_paper()->add_contracts();
        *target->mutable_cost_schedule() = source.cost_schedule();
        target->set_slippage_ticks(source.slippage_ticks());
        *target->mutable_dataset() = std::move(dataset);
    };
    if (reuse.choice() == Choice::FIXED) {
        for (int c = 0; c < input.paper().contracts_size(); ++c) {
            data::proto::DataRequest query;
            *query.mutable_bar_dataset() = selected(input.paper().contracts(c).dataset());
            auto reply = co_await data_input(call, std::move(query));
            *input.mutable_paper()->mutable_contracts(c)->mutable_dataset() =
                std::move(*reply.mutable_bar_dataset());
        }
    } else {
        // Preserve the original actual-month universe and each month's own costs.
        // A new snapshot can change the schedule, not invent costs for new months.
        input.mutable_paper()->clear_contracts();
        input.clear_series();
        std::vector<bool> in_series(original.paper().contracts_size());
        std::vector<std::vector<unsigned>> members;
        for (const auto& schedule : original.series()) {
            std::set<unsigned> indexes;
            for (const auto& roll : schedule.rolls())
                indexes.insert(roll.contract());
            members.emplace_back(indexes.begin(), indexes.end());
            for (const auto index : indexes)
                in_series[index] = true;
        }
        for (int c = 0; c < original.paper().contracts_size(); ++c) {
            if (in_series[c])
                continue;
            data::proto::DataRequest query;
            *query.mutable_bar_dataset() = selected(original.paper().contracts(c).dataset());
            auto reply = co_await data_input(call, std::move(query));
            keep(original.paper().contracts(c), std::move(*reply.mutable_bar_dataset()));
        }
        for (std::size_t group = 0; group < members.size(); ++group) {
            const auto& indexes = members[group];
            data::proto::DataRequest query;
            query.mutable_dominant_series()->set_first_day(
                original.series(static_cast<int>(group)).rolls(0).trading_day());
            for (const auto index : indexes)
                *query.mutable_dominant_series()->add_months() =
                    selected(original.paper().contracts(index).dataset());
            auto reply = co_await data_input(call, std::move(query));
            auto& resolved = *reply.mutable_dominant_series();
            if (resolved.months_size() != resolved.datasets_size())
                throw std::invalid_argument("dominant series response has inconsistent months");
            const auto offset = static_cast<unsigned>(input.paper().contracts_size());
            for (int m = 0; m < resolved.months_size(); ++m) {
                if (resolved.months(m) >= indexes.size())
                    throw std::invalid_argument("dominant series response has an unknown month");
                keep(
                    original.paper().contracts(indexes[resolved.months(m)]),
                    std::move(*resolved.mutable_datasets(m)));
            }
            for (auto& roll : *resolved.mutable_schedule()->mutable_rolls())
                roll.set_contract(roll.contract() + offset);
            *input.add_series() = std::move(*resolved.mutable_schedule());
        }
    }
    co_await ready(call);
    call.input_in_time();
    refuse_during_upgrade(quiescing_);
    const auto programme = co_await program(call);
    call.input_in_time();
    if ((reuse.has_slippage_ticks() || reuse.has_fee_multiplier())
        && programme.artifact != source_program)
        throw Error(
            ErrorCode::conflict,
            "cost sensitivity requires its original execution program");
    if (batch) {
        if (programme.artifact != batch->program_artifact())
            throw Error(ErrorCode::conflict, "batch requires its original execution program");
        std::string digest;
        co_await file_work([&] {
            input.set_dataset_revision(protocol::dataset_revision(input.paper()));
            if (input.SerializeAsString() != original_bytes)
                throw std::invalid_argument("batch fixed input differs from its original source");
            input = reuse.has_fee_multiplier()
                ? protocol::backtest_fee_variant(
                      input,
                      Decimal::from_raw(reuse.fee_multiplier().units()))
                : protocol::backtest_slippage_variant(input, reuse.slippage_ticks());
            digest = sha256_bytes(input.SerializeAsString());
        });
        co_await ready(call);
        call.input_in_time();
        const auto timer = batch_started_.find(batch->request().id());
        if (timer == batch_started_.end() || Clock::now() >= timer->second.deadline)
            throw Error(ErrorCode::conflict, "batch budget exhausted before member preparation");
        (void)co_await batch_metadata(
            call,
            batch->owner(),
            true,
            [id = p.id(), batch, digest](auto& batches, auto& reply) {
                *reply.mutable_state() = batches.begin_member(batch->request().id(), id, digest);
            });
    }
    std::optional<backtest::proto::BacktestInput> replay;
    if (reuse.has_model_replay()) {
        if (input.has_model_replay() || input.paper().contracts_size() != 1
            || !input.series().empty() || input.has_walk_forward() || !input.holdout_day().empty())
            throw std::invalid_argument(
                "model replay requires an ordinary single-contract reference account");
        const auto selection = reuse.model_replay();
        protocol::validate_strategy_object(selection.mapping(), "model_mapping");
        const auto node = co_await data_.node_id(5s, call.request.correlation_id());
        const auto owner = selection.mapping().owner();
        if (owner.node_id() != node || owner.service_id() != configuration_.service)
            throw Error(ErrorCode::conflict, "model replay belongs to another Research owner");
        co_await wait_for_store({}, call.deadline);
        research::Models models(*storage_, owner);
        const auto mapping = models.mapping(selection.mapping().id());
        if (protocol::model_mapping_object(mapping).SerializeAsString()
                != selection.mapping().SerializeAsString()
            || mapping.specification().model().kind() != "model_time_validation_result")
            throw Error(
                ErrorCode::conflict,
                "model replay requires its exact chronological mapping");
        // The existing reuse lease bounds these sequential source reads to one public operation.
        auto model_read = storage_->prepare_result(mapping.specification().model().id());
        co_await file_work([&] {
            model_read.verify();
        });
        co_await ready(call);
        auto original_model = storage_->confirm_result(std::move(model_read));
        const auto& original_task = original_model.result_task();
        if (!original_model.has_model_time()
            || original_task.result_digest() != mapping.specification().model().version())
            throw Error(ErrorCode::conflict, "model replay fixed result identity changed");
        const auto fixed = original_task.model_time();
        (void)models.time_admission(
            protocol::model_time_plan_object(fixed.plan()),
            programme.artifact);
        replay = input;
        replay->clear_strategies();
        auto& definition = *replay->mutable_model_replay();
        *definition.mutable_mapping() = mapping;
        *definition.mutable_training() = fixed;
        definition.set_model_attempt(original_task.attempt());
        definition.set_fold_index(selection.fold_index());
        *definition.mutable_quantity() = selection.quantity();
        definition.set_sides(selection.sides());
        definition.set_processing_delay_bars(selection.processing_delay_bars());
        definition.set_time_budget_seconds(selection.time_budget_seconds());
        definition.set_availability(selection.availability());
        co_await file_work([&] {
            (void)protocol::decode_backtest(*replay);
            (void)research::resolve_model_replay_data(
                *replay,
                configuration_.directory,
                programme.artifact);
        });
        co_await ready(call);
        call.input_in_time();
    }
    *call.response.mutable_task() = (co_await submit(call, [&] {
        if (reuse.choice() == Choice::NEW_SNAPSHOT)
            *input.mutable_data_owner() = call.data_route->owner;
        input.set_dataset_revision(protocol::dataset_revision(input.paper()));
        if (!batch && reuse.choice() == Choice::FIXED
            && input.SerializeAsString() != original_bytes)
            throw std::invalid_argument(
                "reused backtest input does not match original fixed evidence");
        if (!batch && (reuse.has_slippage_ticks() || reuse.has_fee_multiplier()))
            input = reuse.has_fee_multiplier()
                ? protocol::backtest_fee_variant(
                      input,
                      Decimal::from_raw(reuse.fee_multiplier().units()))
                : protocol::backtest_slippage_variant(input, reuse.slippage_ticks());
        if (replay)
            input = std::move(*replay);
        auto submission = storage_->submission(p.id(), std::move(input));
        submission.set_program_artifact(programme.artifact);
        if (candidate)
            submission.set_strategy_use(p.strategy_use(), candidate->definition());
        wire::TaskDerivation source;
        source.set_source_id(reuse.source_id());
        source.set_source_result_digest(reuse.source_result_digest());
        source.set_data_choice(reuse.choice() == Choice::FIXED ? "fixed" : "snapshot");
        source.set_snapshot(reuse.snapshot());
        if (batch)
            *source.mutable_batch() = protocol::backtest_batch_object(*batch);
        submission.set_derivation(std::move(source));
        return submission;
    })).task();
}

PolledTask<> ResearchHost::verify_plan(Call& call, const research::proto::ProjectBinding& binding)
{
    co_await wait_for_store({}, call.deadline);
    call.input_in_time();
    research::Projects(*storage_, binding.owner()).verify_binding(binding);
}

PolledTask<research::proto::VariableDefinition>
ResearchHost::resolve_variable(Call& call, resource::proto::Object object)
{
    protocol::validate_variable_reference(object);
    co_await wait_for_store({}, call.deadline);
    call.input_in_time();
    const auto definition = research::Variables(*storage_, object.owner()).definition(object.id());
    protocol::verify_variable_reference(object, definition);
    co_return definition;
}

PolledTask<> ResearchHost::submit_factor(Call& call)
{
    const auto& p = call.request.submit();
    const auto& f = p.factor_request();
    std::optional<research::proto::ProjectBinding> binding;
    if (p.has_research_plan())
        binding = p.research_plan();
    if (p.has_derivation()) {
        co_await wait_for_store(p.derivation().source_id());
        auto source_slot = verification_slots_.acquire(false, p.derivation().source_id());
        auto read = storage_->prepare_result(p.derivation().source_id());
        co_await file_work([&] {
            read.verify();
        });
        co_await ready(call);
        auto source = storage_->confirm_result(std::move(read));
        if (!binding && source.result_task().has_research_plan())
            binding = source.result_task().research_plan();
        co_await file_work([&] {
            if (!source.has_factor()
                || source.result_task().result_digest() != p.derivation().source_result_digest())
                throw std::invalid_argument("invalid experiment derivation");
            const auto parameters = protocol::task_execution(source.result_task(), "");
            const auto expected = protocol::reuse_daily_factor(
                parameters.factor(),
                source.result_task().factor().dataset_revision(),
                p.derivation().snapshot(),
                std::vector<unsigned>(f.lookbacks().begin(), f.lookbacks().end()));
            if (expected.SerializeAsString() != f.SerializeAsString())
                throw std::invalid_argument("invalid experiment derivation");
        });
        co_await ready(call);
        if (p.derivation().data_choice() == "fixed") {
            const auto owner = source.result_task().factor().data_owner();
            require_fixed_owner(p.data_owner(), owner);
            *call.request.mutable_submit()->mutable_data_owner() = owner;
        }
    }
    if (binding)
        co_await verify_plan(call, *binding);
    std::optional<research::proto::VariableDefinition> variable;
    if (f.has_variable_definition())
        variable = co_await resolve_variable(call, f.variable_definition());
    auto input = protocol::factor_input(f);
    if (variable) {
        if (f.lookbacks_size() != 1 || f.lookbacks(0) != variable->window())
            throw std::invalid_argument("factor window differs from its fixed variable definition");
        *input.mutable_variable_definition() = *variable;
    }
    for (const auto& source : f.series()) {
        auto prepared = co_await data_input(call, protocol::factor_series_query(source));
        for (auto& series : protocol::factor_series(source, std::move(prepared)))
            protocol::add_factor_series(input, std::move(series));
    }
    co_await ready(call);
    call.input_in_time();
    refuse_during_upgrade(quiescing_);
    *input.mutable_data_owner() = call.data_route->owner;
    const auto programme = co_await program(call);
    call.input_in_time();
    if (variable && variable->owner().SerializeAsString() != programme.owner.SerializeAsString())
        throw Error(ErrorCode::conflict, "research variable owner mismatch");
    if (binding && binding->owner().SerializeAsString() != programme.owner.SerializeAsString())
        throw Error(ErrorCode::conflict, "research project owner mismatch");
    *call.response.mutable_task() = (co_await submit(call, [&] {
        // Hashing a daily series is file-pool work, like the rest of the submission.
        input.set_dataset_revision(protocol::factor_revision(input.series()));
        if (!f.expected_dataset_revision().empty()
            && input.dataset_revision() != f.expected_dataset_revision())
            throw std::invalid_argument("reused factor data does not match the original revision");
        auto submission = storage_->submission(p.id(), std::move(input));
        submission.set_program_artifact(programme.artifact);
        if (binding)
            submission.set_research_plan(*binding);
        if (p.has_derivation())
            submission.set_derivation(p.derivation());
        return submission;
    })).task();
}

PolledTask<ResearchProgram> ResearchHost::program(const Call& call)
{
    const auto observed = co_await data_.program(
        configuration_.service,
        static_cast<std::uint64_t>(::getpid()),
        5s,
        call.request.correlation_id());
    if (loaded_program_
        && (loaded_program_->owner.SerializeAsString() != observed.owner.SerializeAsString()
            || loaded_program_->artifact != observed.artifact))
        throw Error(ErrorCode::conflict, "loaded research program identity changed");
    loaded_program_ = observed;
    if (!program_preservation_.valid()) {
        program_preservation_ =
            files_
                .submit([this, observed](std::stop_token stop) {
                    (void)programs::Files(configuration_.directory / "programs", observed.owner)
                        .preserve(current_executable(), observed.artifact, stop);
                })
                .share();
    }
    const auto preserved = program_preservation_;
    co_await PollUntil{[&] {
        return preserved.wait_for(0ms) == std::future_status::ready;
    }};
    preserved.get();
    co_return observed;
}

PolledTask<research::proto::AccountReviewInput> ResearchHost::account_evidence(
    const Call& call,
    const research::proto::AccountReviewRequest& selected,
    const resource::proto::Owner& owner,
    const std::string& artifact)
{
    protocol::validate_account_review_request(selected);
    const auto route =
        co_await data_.resolve_facts(selected.source().owner(), 5s, call.request.correlation_id());
    protocol::proto::Request query;
    query.set_version(1);
    query.set_session_id(route.owner.service_id());
    query.set_correlation_id(next_correlation_id());
    *query.mutable_facts()->mutable_read() = selected.source();
    const auto raw = co_await data_.read(route, query.SerializeAsString(), 30s);
    research::proto::AccountReviewInput result;
    co_await file_work([&] {
        protocol::proto::Response reply;
        if (!reply.ParseFromString(*raw))
            throw Error(ErrorCode::unavailable, "invalid trading facts response");
        protocol::validate_message(reply);
        if (reply.version() != query.version() || reply.session_id() != query.session_id()
            || reply.correlation_id() != query.correlation_id())
            throw Error(ErrorCode::conflict, "trading facts response identity mismatch");
        if (reply.has_error())
            throw_remote_error(reply.error().code(), reply.error().message());
        if (!reply.has_facts())
            throw Error(ErrorCode::unavailable, "fixed trading facts are unavailable");
        result = protocol::make_account_review_input(owner, selected, reply.facts(), artifact);
    });
    call.input_in_time();
    co_return result;
}

PolledTask<> ResearchHost::submit_logic_trial(Call& call)
{
    const auto& submission = call.request.submit();
    const auto& selected = submission.logic_trial().example();
    protocol::validate_logic_object(selected, "logic_case");
    validate_id(submission.id());
    if (submission.has_data_owner() || submission.has_derivation() || submission.has_research_plan()
        || submission.has_strategy_use())
        throw std::invalid_argument("logic trial does not accept Data or backtest bindings");
    std::optional<wire::Task> existing;
    try {
        existing = storage_->describe(submission.id());
    } catch (const std::out_of_range&) {
    }
    if (existing) {
        if (existing->kind() != wire::LOGIC_TRIAL || existing->has_retirement())
            throw Error(ErrorCode::conflict, "logic trial identity is already used or retired");
        auto read = storage_->prepare_input(submission.id());
        co_await file_work([&] {
            read.load_for_claim();
        });
        co_await ready(call);
        call.input_in_time();
        if (!read.execution().has_logic_trial()
            || protocol::logic_object(read.execution().logic_trial().example()).SerializeAsString()
                != selected.SerializeAsString())
            throw Error(ErrorCode::conflict, "logic trial identity names different fixed evidence");
        *call.response.mutable_task() = storage_->describe(submission.id());
        co_return;
    }
    const auto programme = co_await program(call);
    co_await wait_for_store({}, call.deadline);
    call.input_in_time();
    auto input = research::Logic(*storage_, programme.owner).input(selected, true);
    *call.response.mutable_task() = (co_await submit(call, [&] {
        auto accepted = storage_->submission(submission.id(), std::move(input));
        accepted.set_program_artifact(programme.artifact);
        return accepted;
    })).task();
}

PolledTask<> ResearchHost::submit_sample(Call& call)
{
    const auto& submission = call.request.submit();
    const auto& selected = submission.sample();
    protocol::validate_sample_request(selected);
    if (submission.has_research_plan())
        co_await verify_plan(call, submission.research_plan());
    validate_id(submission.id());
    if (submission.has_derivation())
        throw std::invalid_argument("sample does not accept client derivation");
    std::optional<wire::Task> existing;
    try {
        existing = storage_->describe(submission.id());
    } catch (const std::out_of_range&) {
    }
    if (existing) {
        if (existing->kind() != wire::SAMPLE || existing->has_retirement())
            throw Error(ErrorCode::conflict, "sample identity is already used or retired");
        auto read = storage_->prepare_input(submission.id());
        co_await file_work([&] {
            read.load_for_claim();
        });
        co_await ready(call);
        call.input_in_time();
        const auto& requested_owner = submission.data_owner();
        const auto& fixed_owner = read.execution().data_owner();
        if (!read.execution().has_sample()
            || read.execution().sample().request().SerializeAsString()
                != selected.SerializeAsString()
            || existing->research_plan().SerializeAsString()
                != submission.research_plan().SerializeAsString()
            || (!requested_owner.node_id().empty()
                && requested_owner.node_id() != fixed_owner.node_id())
            || (!requested_owner.service_id().empty()
                && requested_owner.service_id() != fixed_owner.service_id()))
            throw Error(ErrorCode::conflict, "sample identity names different fixed evidence");
        *call.response.mutable_task() = storage_->describe(submission.id());
        co_return; // Replay needs neither current Data nor a replacement executable.
    }
    data::proto::DataRequest request;
    *request.mutable_bar_dataset() = selected.selection();
    auto source = co_await data_input(call, std::move(request));
    const auto programme = co_await program(call);
    call.input_in_time();
    research::proto::SampleInput input;
    *input.mutable_owner() = programme.owner;
    *input.mutable_data_owner() = call.data_route->owner;
    *input.mutable_request() = selected;
    *input.mutable_dataset() = std::move(*source.mutable_bar_dataset());
    *call.response.mutable_task() = (co_await submit(call, [&] {
        auto accepted = storage_->submission(submission.id(), std::move(input));
        accepted.set_program_artifact(programme.artifact);
        if (submission.has_research_plan())
            accepted.set_research_plan(submission.research_plan());
        return accepted;
    })).task();
}

PolledTask<> ResearchHost::submit_evaluation(Call& call)
{
    const auto& submission = call.request.submit();
    const auto& selected = submission.evaluation();
    protocol::validate_evaluation_request(selected);
    validate_id(submission.id());
    if (submission.has_data_owner() || submission.has_derivation() || submission.has_research_plan()
        || submission.has_strategy_use())
        throw std::invalid_argument(
            "evaluation does not accept Data selection or execution bindings");
    try {
        const auto existing = storage_->describe(submission.id());
        if (existing.kind() != wire::PERFORMANCE_EVALUATION || existing.has_retirement())
            throw Error(ErrorCode::conflict, "evaluation identity is already used or retired");
        auto read = storage_->prepare_input(submission.id());
        co_await file_work([&] {
            read.load_for_claim();
        });
        co_await ready(call);
        if (!read.execution().has_evaluation()
            || read.execution().evaluation().request().SerializeAsString()
                != selected.SerializeAsString())
            throw Error(ErrorCode::conflict, "evaluation identity names different fixed evidence");
        *call.response.mutable_task() = storage_->describe(submission.id());
        co_return; // Existing receipt never recalculates or reselects a source.
    } catch (const std::out_of_range&) {
    }
    const auto observed = co_await program(call);
    auto slot = verification_slots_.acquire(false, selected.source().id());
    auto read = storage_->prepare_result(selected.source().id());
    const auto input_digest = read.input_digest();
    const auto input_bytes = read.input_bytes();
    co_await file_work([&] {
        read.verify();
    });
    co_await ready(call);
    call.input_in_time();
    const auto source = storage_->confirm_result(std::move(read));
    research::proto::EvaluationInput input;
    co_await file_work([&] {
        input =
            capture_evaluation_input(observed.owner, selected, source, input_digest, input_bytes);
    });
    *call.response.mutable_task() = (co_await submit(call, [&] {
        auto accepted = storage_->submission(submission.id(), std::move(input));
        accepted.set_program_artifact(observed.artifact);
        return accepted;
    })).task();
}

PolledTask<> ResearchHost::submit_statistics(Call& call)
{
    const auto& submission = call.request.submit();
    const auto& selected = submission.statistics();
    protocol::validate_statistics_request(selected);
    if (submission.has_research_plan())
        co_await verify_plan(call, submission.research_plan());
    validate_id(submission.id());
    if (submission.has_data_owner() || submission.has_derivation())
        throw std::invalid_argument("statistics do not accept Data selection or client derivation");
    try {
        const auto existing = storage_->describe(submission.id());
        if (existing.kind() != wire::STATISTICS || existing.has_retirement())
            throw Error(ErrorCode::conflict, "statistics identity is already used or retired");
        auto read = storage_->prepare_input(submission.id());
        co_await file_work([&] {
            read.load_for_claim();
        });
        co_await ready(call);
        if (existing.research_plan().SerializeAsString()
                != submission.research_plan().SerializeAsString()
            || !read.execution().has_statistics()
            || read.execution().statistics().request().SerializeAsString()
                != selected.SerializeAsString())
            throw Error(ErrorCode::conflict, "statistics identity names different fixed evidence");
        *call.response.mutable_task() = storage_->describe(submission.id());
        co_return;
    } catch (const std::out_of_range&) {
    }
    const auto observed = co_await program(call);
    auto slot = verification_slots_.acquire(false, selected.source().id());
    auto read = storage_->prepare_result(selected.source().id());
    const auto input_digest = read.input_digest();
    const auto input_bytes = read.input_bytes();
    co_await file_work([&] {
        read.verify();
    });
    co_await ready(call);
    call.input_in_time();
    const auto source = storage_->confirm_result(std::move(read));
    auto input =
        capture_statistics_input(observed.owner, selected, source, input_digest, input_bytes);
    *call.response.mutable_task() = (co_await submit(call, [&] {
        auto accepted = storage_->submission(submission.id(), std::move(input));
        accepted.set_program_artifact(observed.artifact);
        if (submission.has_research_plan())
            accepted.set_research_plan(submission.research_plan());
        return accepted;
    })).task();
}

PolledTask<> ResearchHost::submit_account_review(Call& call)
{
    const auto& submission = call.request.submit();
    const auto& selected = submission.account_review();
    protocol::validate_account_review_request(selected);
    validate_id(submission.id());
    if (submission.has_data_owner() || submission.has_derivation())
        throw std::invalid_argument(
            "account review does not accept Data selection or experiment derivation");
    try {
        const auto existing = storage_->describe(submission.id());
        if (existing.kind() != wire::ACCOUNT_REVIEW || existing.has_retirement())
            throw Error(ErrorCode::conflict, "account review identity is already used or retired");
        auto read = storage_->prepare_input(submission.id());
        co_await file_work([&] {
            read.load_for_claim();
        });
        co_await ready(call);
        if (!read.execution().has_account_review()
            || read.execution().account_review().request().SerializeAsString()
                != selected.SerializeAsString())
            throw Error(
                ErrorCode::conflict,
                "account review identity names different fixed evidence");
        *call.response.mutable_task() = storage_->describe(submission.id());
        co_return; // Replaying accepted identity needs neither source nor current program.
    } catch (const std::out_of_range&) {
    }
    const auto observed = co_await program(call);
    auto input = co_await account_evidence(call, selected, observed.owner, observed.artifact);
    *call.response.mutable_task() = (co_await submit(call, [&] {
        return storage_->submission(submission.id(), std::move(input));
    })).task();
}

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
    std::optional<wire::Task> existing;
    try {
        existing = storage_->describe(id);
    } catch (const std::out_of_range&) {
    }
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
    try {
        const auto existing = storage_->describe(submission.id());
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
    } catch (const std::out_of_range&) {
    }
    const auto observed = co_await program(call);
    auto input = co_await notice_evidence(call, selected, observed.owner, observed.artifact);
    *call.response.mutable_task() = (co_await submit(call, [&] {
        return storage_->submission(submission.id(), std::move(input));
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
    try {
        const auto existing = storage_->describe(submission.id());
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
    } catch (const std::out_of_range&) {
    }
    const auto observed = co_await program(call);
    auto input = co_await event_set_evidence(call, selected, observed.owner, observed.artifact);
    *call.response.mutable_task() = (co_await submit(call, [&] {
        return storage_->submission(submission.id(), std::move(input));
    })).task();
}

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
    call.admitted();
    call.task_id = named_task(request);
    co_await ready(call, call.deadline);
    if (request.has_submit() && request.submit().has_strategy_use()
        && !request.submit().has_backtest_reuse() && !request.submit().has_backtest())
        throw std::invalid_argument("strategy delivery requires explicit backtest admission");
    if (request.has_submit() && request.submit().has_research_plan()
        && !request.submit().has_factor_request() && !request.submit().has_sample()
        && !request.submit().has_statistics() && !request.submit().has_backtest())
        throw std::invalid_argument("research plan task kind is unsupported");
    if (request.has_submit() && request.submit().derivation().has_batch()
        && !request.submit().has_backtest_reuse())
        throw std::invalid_argument("batch member requires fixed backtest reuse");
    if (request.has_retry()
        && (storage_->describe(request.retry().id()).kind() == wire::MODEL_TRAINING
            || storage_->describe(request.retry().id()).kind() == wire::MODEL_TIME_VALIDATION))
        throw Error(
            ErrorCode::conflict,
            "model training retry requires a new explicit task admission");
    if (request.has_retry()
        && storage_->describe(request.retry().id()).metadata().derivation().has_batch())
        throw Error(
            ErrorCode::conflict,
            "batch member retry requires a new explicitly budgeted batch");
    call.admitted();
    if ((request.has_project() && request.project().has_mutation())
        || (request.has_model() && request.model().has_mutation()) || request.has_submit()
        || request.has_metadata_edit() || request.has_retry() || request.has_recover_submission()
        || request.has_abandon_preparation() || request.has_reconcile_notice_custody()
        || (request.has_retirement() && request.retirement().execute()))
        refuse_during_upgrade(quiescing_);
    std::optional<VerificationSlots::Lease> preparation;
    if (request.has_recover_submission() || request.has_abandon_preparation()
        || request.has_reconcile_notice_custody() || request.has_notice_source_changes()
        || request.has_retirement()
        || (request.has_strategy() && request.strategy().has_mutation()
            && (request.strategy().mutation().has_freeze()
                || request.strategy().mutation().has_logic()))
        || (request.has_submit()
            && (request.submit().has_backtest() || request.submit().has_backtest_reuse()
                || request.submit().has_factor_request() || request.submit().has_account_review()
                || request.submit().has_notice_review() || request.submit().has_event_set_review()
                || request.submit().has_statistics() || request.submit().has_sample()
                || request.submit().has_logic_trial() || request.submit().has_model_training()
                || request.submit().has_model_time() || request.submit().has_evaluation())))
        preparation.emplace(input_slots_.acquire(false, request.correlation_id()));
    if (request.has_retry()
        && (storage_->describe(request.retry().id()).kind() != wire::BACKTEST
            && storage_->describe(request.retry().id()).kind() != wire::FACTOR
            && storage_->describe(request.retry().id()).kind() != wire::ACCOUNT_REVIEW
            && storage_->describe(request.retry().id()).kind() != wire::NOTICE_REVIEW
            && storage_->describe(request.retry().id()).kind() != wire::EVENT_SET_REVIEW
            && storage_->describe(request.retry().id()).kind() != wire::STATISTICS
            && storage_->describe(request.retry().id()).kind() != wire::PERFORMANCE_EVALUATION
            && storage_->describe(request.retry().id()).kind() != wire::SAMPLE
            && storage_->describe(request.retry().id()).kind() != wire::LOGIC_TRIAL))
        throw Error(ErrorCode::conflict, "acquisition belongs to the data service");
    if (request.has_project())
        co_await project_operation(call);
    else if (request.has_model())
        co_await model_operation(call);
    else if (request.has_variable())
        co_await variable_operation(call);
    else if (request.has_logic())
        co_await logic_operation(call);
    else if (request.has_strategy())
        co_await strategy_operation(call);
    else if (request.has_batch())
        co_await batch_operation(call);
    else if (request.has_assessment())
        co_await assessment_operation(call);
    else if (request.has_project_tasks()) {
        co_await wait_for_store({}, call.deadline);
        const auto& query = request.project_tasks();
        *response.mutable_project_tasks() =
            storage_->project_tasks(query.project_id(), query.limit(), query.before_sequence());
    } else if (request.has_release_decision())
        release_decision(call);
    else if (request.has_resources()) {
        const auto& kind = request.resources().kind();
        if (kind == "backtest_batch" || kind == "backtest_batch_assessment") {
            co_await batch_operation(call);
            co_return;
        }
        if (kind == "strategy_package" || kind == "strategy_delivery") {
            co_await strategy_operation(call);
            co_return;
        }
        if (kind == "logic_definition" || kind == "logic_case") {
            co_await logic_operation(call);
            co_return;
        }
        if (kind == "model_draft" || kind == "model_plan" || kind == "model_mapping"
            || kind == "model_time_plan") {
            co_await model_operation(call);
            co_return;
        }
        if (kind == "research_variable") {
            co_await variable_operation(call);
            co_return;
        }
        if (kind == "research_project" || kind == "research_plan"
            || kind == "research_conclusion") {
            co_await project_operation(call);
            co_return;
        }
        const auto node = co_await data_.node_id(
            std::chrono::ceil<std::chrono::milliseconds>(call.input_deadline - Clock::now()),
            request.correlation_id());
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
            *response.mutable_resources() = storage_->resources(node, request.resources());
    } else if (request.has_retirement())
        co_await retirement(call);
    else if (request.has_abandon_preparation())
        co_await abandon_preparation(call);
    else if (request.has_recover_submission())
        co_await recover_submission(call);
    else if (request.has_reconcile_notice_custody())
        co_await reconcile_notice_custody(call);
    else if (request.has_notice_source_changes())
        co_await notice_source_changes(call);
    else if (request.has_history_usage())
        co_await history_usage(call);
    else if (request.has_submit()) {
        const auto& p = request.submit();
        if (p.has_backtest())
            co_await submit_backtest(call);
        else if (p.has_backtest_reuse())
            co_await reuse_backtest(call);
        else if (p.has_factor_request())
            co_await submit_factor(call);
        else if (p.has_model_training() || p.has_model_time())
            co_await submit_model(call);
        else if (p.has_statistics())
            co_await submit_statistics(call);
        else if (p.has_evaluation())
            co_await submit_evaluation(call);
        else if (p.has_sample())
            co_await submit_sample(call);
        else if (p.has_logic_trial())
            co_await submit_logic_trial(call);
        else if (p.has_account_review())
            co_await submit_account_review(call);
        else if (p.has_notice_review())
            co_await submit_notice_review(call);
        else if (p.has_event_set_review())
            co_await submit_event_set_review(call);
        else
            throw std::invalid_argument("missing task input");
    } else if (request.has_metadata_get()) {
        *response.mutable_task() = storage_->describe(request.metadata_get().id());
    } else if (request.has_metadata_edit()) {
        *response.mutable_task() =
            (co_await commit(storage_->edit_metadata(request.metadata_edit()))).task();
    } else if (request.has_get()) {
        auto slot = input_slots_.acquire(false, request.get().id());
        auto read = storage_->prepare_input(request.get().id());
        co_await file_work([&] {
            read.load();
        });
        co_await ready(call);
        call.input_in_time();
        *response.mutable_task() = storage_->confirm_input(std::move(read));
    } else if (request.has_list())
        *response.mutable_tasks() =
            storage_->list(request.list().limit(), request.list().before_sequence());
    else if (request.has_cancel())
        *response.mutable_task() =
            (co_await commit(storage_->cancel(request.cancel().id()))).task();
    else if (request.has_retry())
        *response.mutable_task() = (co_await commit(storage_->retry(request.retry().id()))).task();
    else if (request.has_result()) {
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
    } else
        throw std::invalid_argument("missing task operation");
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
                log_process_event(
                    "research-service",
                    LogLevel::error,
                    "task.execution_failed",
                    {{"task_id", it->first}, {"message", error.what()}});
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
