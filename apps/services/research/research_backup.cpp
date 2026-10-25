#include "research_backup.hpp"
#include "research_data.hpp"
#include "project_store.hpp"
#include "variable_store.hpp"
#include "strategy_store.hpp"
#include "logic_store.hpp"
#include "batch_store.hpp"
#include "model_store.hpp"
#include <asterion/protocol/research_model.hpp>
#include <asterion/protocol/research_batch.hpp>
#include <asterion/protocol/research_assessment.hpp>
#include <asterion/protocol/research_logic.hpp>
#include "risk_module.hpp"
#include <asterion/protocol/research_strategy.hpp>
#include <asterion/protocol/research_variable.hpp>
#include <asterion/protocol/research_statistics.hpp>
#include <asterion/protocol/research_evaluation.hpp>
#include "task_store.hpp"
#include "task_payload.hpp"
#include "program_files.hpp"
#include <asterion/protocol/account_review.hpp>
#include <asterion/protocol/task_execution.hpp>
#include <asterion/foundation/error.hpp>
#include <asterion/foundation/id.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <algorithm>
#include <map>
#include <set>

namespace asterion::research {
namespace fs = std::filesystem;

ColdBackup
cold_backup(const fs::path& directory, const resource::proto::Owner& owner, std::stop_token stop)
{
    validate_id(owner.node_id());
    validate_id(owner.service_id());
    if (owner.domain() != "research" || !directory.is_absolute() || fs::is_symlink(directory)
        || !fs::is_directory(directory))
        throw std::invalid_argument("Research cold backup requires its owned root");
    const auto root = fs::canonical(directory);
    ColdBackup result;
    auto& snapshot = result.snapshot;
    snapshot.owner = owner;
    snapshot.directory = root;
    snapshot.ownership =
        std::make_unique<FileLock>(root, "manager.lock", FileLock::Access::shared_existing);
    for (const auto& suffix : {"-wal", "-journal"}) {
        const fs::path journal((root / "tasks.sqlite").string() + suffix);
        if (fs::exists(journal) && fs::file_size(journal))
            throw Error(
                ErrorCode::unavailable,
                "Research requires owner recovery before cold backup");
    }
    std::set<std::string> dependencies;
    std::map<std::string, resource::proto::Object> inputs;
    std::vector<task::proto::TaskMetadata> origins;
    std::map<std::string, task::proto::StoredTaskRecord> source_records;
    std::vector<proto::StatisticsSource> statistics_sources;
    std::vector<proto::EvaluationInput> evaluation_sources;
    std::vector<proto::LogicTrialInput> required_logic;
    std::vector<proto::ModelTrainingInput> required_models;
    std::vector<proto::ModelTimeInput> required_times;
    std::vector<proto::ModelReplayDefinition> required_replays;
    std::map<std::string, proto::ModelReplayDefinition> replay_sources;
    std::map<std::string, proto::ModelMapping> retained_mappings;
    std::map<std::string, proto::ModelTimePlan> retained_time_plans;
    std::vector<task::proto::StoredTaskRecord> required_batches;
    bool missing_original_program = false;
    std::set<std::string> required_programs;
    std::vector<proto::ProjectBinding> required_plans;
    std::vector<resource::proto::Object> required_variables;
    std::vector<std::pair<std::string, proto::StrategyUse>> required_strategies;
    const auto depend = [&](resource::proto::Object value) {
        if (dependencies.insert(value.SerializeAsString()).second)
            snapshot.dependencies.push_back({std::move(value), false});
    };
    const auto inspect_review = [&](const task::proto::StoredTaskRecord& record,
                                    const task::proto::TaskExecution& execution) {
        if (!execution.has_account_review())
            throw std::invalid_argument("account review backup lacks its execution identity");
        const auto& fixed = execution.account_review();
        protocol::validate_account_review_execution(fixed);
        if (fixed.owner().SerializeAsString() != owner.SerializeAsString()
            || execution.has_data_owner() || execution.input_sha256() != record.input_digest())
            throw Error(ErrorCode::conflict, "account review backup owner or input mismatch");
        required_programs.insert(fixed.program_artifact());
        resource::proto::Object input;
        *input.mutable_owner() = owner;
        input.set_kind("account_review");
        input.set_id(record.task().id());
        input.set_version(record.input_digest());
        if (record.task().has_retirement()
            && record.task().retirement().SerializeAsString() != input.SerializeAsString())
            throw Error(ErrorCode::conflict, "account review retirement backup mismatch");
        result.objects.push_back(input);
        inputs.emplace(input.id(), input);
        depend(
            fixed.request().source()); // Embedded evidence is not a backup of the Trading ledger.
        resource::proto::Object program;
        *program.mutable_owner() = owner;
        program.set_kind("research_program");
        program.set_id(fixed.program_artifact());
        program.set_version(fixed.program_artifact());
        depend(std::move(program));
        const auto file = root / input.id() / "input.pb";
        if (fs::exists(file)) {
            research::proto::AccountReviewInput evidence;
            tasks::payload::parse(
                tasks::payload::read(
                    file,
                    record.input_digest(),
                    tasks::payload::max_input_bytes,
                    record.input_bytes()),
                evidence);
            if (protocol::account_review_execution(evidence).SerializeAsString()
                != fixed.SerializeAsString())
                throw Error(
                    ErrorCode::conflict,
                    "account review backup input differs from its execution");
            if (record.task().state() == task::proto::SUCCEEDED) {
                research::proto::AccountReviewResult output;
                tasks::payload::parse(
                    tasks::payload::read(
                        root / input.id() / "results"
                            / (std::to_string(record.task().attempt()) + ".pb"),
                        record.task().result_digest(),
                        tasks::payload::max_result_bytes),
                    output);
                protocol::validate_account_review_result(evidence, output);
                auto object = input;
                object.set_kind("account_review_result");
                object.set_version(record.task().result_digest());
                result.objects.push_back(std::move(object));
            }
        }
        return input;
    };
    const auto inspect_calculation_program = [&](const task::proto::StoredTaskRecord& record,
                                                 const task::proto::TaskExecution& execution) {
        if (record.task().strategy_use().SerializeAsString()
            != execution.strategy_use().SerializeAsString())
            throw Error(ErrorCode::conflict, "strategy use differs from its fixed execution");
        if (execution.has_strategy_use()) {
            if (record.task().kind() != task::proto::BACKTEST)
                throw Error(ErrorCode::conflict, "strategy use requires a fixed backtest");
            required_strategies.emplace_back(record.task().id(), execution.strategy_use());
            depend(execution.strategy_use().package());
            depend(execution.strategy_use().delivery());
        }
        if (record.task().research_plan().SerializeAsString()
            != execution.research_plan().SerializeAsString())
            throw Error(ErrorCode::conflict, "research plan differs from its fixed execution");
        if (execution.has_research_plan()) {
            required_plans.push_back(execution.research_plan());
            resource::proto::Object plan;
            *plan.mutable_owner() = execution.research_plan().owner();
            plan.set_kind("research_plan");
            plan.set_id(execution.research_plan().plan_id());
            plan.set_version(execution.research_plan().definition_digest());
            depend(std::move(plan));
        }
        if (record.task().has_variable_definition() != execution.has_variable_definition())
            throw Error(ErrorCode::conflict, "research variable differs from its fixed execution");
        if (execution.has_variable_definition()) {
            if (record.task().kind() != task::proto::FACTOR)
                throw std::invalid_argument("invalid fixed research variable owner or task kind");
            protocol::verify_variable_reference(
                record.task().variable_definition(),
                execution.variable_definition());
            required_variables.push_back(record.task().variable_definition());
            depend(record.task().variable_definition());
        }
        if (record.task().program_artifact() != execution.program_artifact())
            throw Error(
                ErrorCode::conflict,
                "calculation backup program differs from fixed execution");
        if (execution.program_artifact().empty()) {
            if (record.task().kind() == task::proto::SAMPLE)
                throw Error(
                    ErrorCode::conflict,
                    "calculation requires its fixed execution program");
            missing_original_program = true;
            return; // Preserve genuine unknown evidence; never attribute the current executable.
        }
        (void)protocol::task_data_references(execution);
        required_programs.insert(execution.program_artifact());
        resource::proto::Object program;
        *program.mutable_owner() = owner;
        program.set_kind("research_program");
        program.set_id(execution.program_artifact());
        program.set_version(execution.program_artifact());
        depend(std::move(program));
    };
    const auto inspect_logic = [&](const task::proto::StoredTaskRecord& record,
                                   const task::proto::TaskExecution& execution) {
        if (!execution.has_logic_trial())
            throw Error(ErrorCode::conflict, "logic trial backup lacks its fixed input");
        const auto& fixed = execution.logic_trial();
        protocol::validate_logic_trial_input(fixed);
        (void)protocol::task_data_references(execution);
        if (fixed.owner().SerializeAsString() != owner.SerializeAsString()
            || execution.input_sha256() != record.input_digest()
            || sha256_bytes(fixed.SerializeAsString()) != record.input_digest())
            throw Error(ErrorCode::conflict, "logic trial backup identity mismatch");
        inspect_calculation_program(record, execution);
        required_logic.push_back(fixed);
        resource::proto::Object input;
        *input.mutable_owner() = owner;
        input.set_kind("logic_trial");
        input.set_id(record.task().id());
        input.set_version(record.input_digest());
        if (record.task().has_retirement()
            && record.task().retirement().SerializeAsString() != input.SerializeAsString())
            throw Error(ErrorCode::conflict, "logic trial retirement identity mismatch");
        result.objects.push_back(input);
        inputs.emplace(input.id(), input);
        depend(protocol::logic_object(fixed.definition()));
        depend(protocol::logic_object(fixed.example()));
        if (record.task().state() == task::proto::SUCCEEDED) {
            auto output = input;
            output.set_kind("logic_trial_result");
            output.set_version(record.task().result_digest());
            result.objects.push_back(std::move(output));
        }
        return input;
    };
    const auto inspect_statistics = [&](const task::proto::StoredTaskRecord& record,
                                        const task::proto::TaskExecution& execution) {
        if (!execution.has_statistics())
            throw Error(ErrorCode::conflict, "invalid statistics source identity");
        const auto& fixed = execution.statistics();
        protocol::validate_statistics_input(fixed);
        if (fixed.owner().SerializeAsString() != owner.SerializeAsString()
            || execution.input_sha256() != record.input_digest()
            || execution.program_artifact().empty() || !record.task().metadata().has_derivation()
            || record.task().metadata().derivation().source_id() != fixed.source().result().id()
            || record.task().metadata().derivation().source_result_digest()
                != fixed.source().result().version())
            throw Error(
                ErrorCode::conflict,
                "statistics source evidence does not match its captured identity");
        inspect_calculation_program(record, execution);
        statistics_sources.push_back(fixed.source());
        required_programs.insert(fixed.source().program_artifact());
        resource::proto::Object input;
        *input.mutable_owner() = owner;
        input.set_kind("statistics");
        input.set_id(record.task().id());
        input.set_version(record.input_digest());
        if (record.task().has_retirement()
            && record.task().retirement().SerializeAsString() != input.SerializeAsString())
            throw Error(ErrorCode::conflict, "invalid statistics source identity");
        result.objects.push_back(input);
        inputs.emplace(input.id(), input);
        depend(fixed.source().result());
        resource::proto::Object source_input = fixed.source().result();
        source_input.set_kind(
            fixed.source().result().kind() == "sample_result" ? "sample" : "factor");
        source_input.set_version(fixed.source().input_sha256());
        depend(std::move(source_input));
        resource::proto::Object source_program = fixed.source().result();
        source_program.set_kind("research_program");
        source_program.set_id(fixed.source().program_artifact());
        source_program.set_version(fixed.source().program_artifact());
        depend(std::move(source_program));
        if (record.task().state() == task::proto::SUCCEEDED) {
            auto output = input;
            output.set_kind("statistics_result");
            output.set_version(record.task().result_digest());
            result.objects.push_back(std::move(output));
        }
        return input;
    };
    const auto inspect_evaluation = [&](const task::proto::StoredTaskRecord& record,
                                        const task::proto::TaskExecution& execution) {
        if (!execution.has_evaluation())
            throw Error(ErrorCode::conflict, "evaluation backup lacks its fixed input");
        const auto& fixed = execution.evaluation();
        protocol::validate_evaluation_input(fixed);
        const auto& original = fixed.request().source();
        if (original.owner().SerializeAsString() != owner.SerializeAsString()
            || execution.input_sha256() != record.input_digest()
            || execution.program_artifact().empty() || !record.task().metadata().has_derivation()
            || record.task().metadata().derivation().source_id() != original.id()
            || record.task().metadata().derivation().source_result_digest() != original.version())
            throw Error(ErrorCode::conflict, "evaluation backup source identity mismatch");
        inspect_calculation_program(record, execution);
        evaluation_sources.push_back(fixed);
        required_programs.insert(fixed.source().program_artifact());
        resource::proto::Object input;
        *input.mutable_owner() = owner;
        input.set_kind("performance_evaluation");
        input.set_id(record.task().id());
        input.set_version(record.input_digest());
        if (record.task().has_retirement()
            && record.task().retirement().SerializeAsString() != input.SerializeAsString())
            throw Error(ErrorCode::conflict, "evaluation retirement backup identity mismatch");
        result.objects.push_back(input);
        inputs.emplace(input.id(), input);
        depend(original);
        auto original_input = original;
        original_input.set_kind("backtest");
        original_input.set_version(fixed.source().input_sha256());
        depend(std::move(original_input));
        auto original_program = original;
        original_program.set_kind("research_program");
        original_program.set_id(fixed.source().program_artifact());
        original_program.set_version(fixed.source().program_artifact());
        depend(std::move(original_program));
        if (record.task().state() == task::proto::SUCCEEDED) {
            auto output = input;
            output.set_kind("performance_evaluation_result");
            output.set_version(record.task().result_digest());
            result.objects.push_back(std::move(output));
        }
        return input;
    };
    const auto inspect_model = [&](const task::proto::StoredTaskRecord& record,
                                   const task::proto::TaskExecution& execution) {
        if (!execution.has_model_training())
            throw Error(ErrorCode::conflict, "model backup lacks its fixed training input");
        const auto& fixed = execution.model_training();
        protocol::validate_model_training_input(fixed);
        if (fixed.plan().owner().SerializeAsString() != owner.SerializeAsString()
            || execution.input_sha256() != record.input_digest()
            || sha256_bytes(fixed.SerializeAsString()) != record.input_digest()
            || record.task().model_plan().SerializeAsString()
                != protocol::model_plan_object(fixed.plan()).SerializeAsString()
            || !record.task().metadata().has_derivation()
            || record.task().metadata().derivation().source_id()
                != fixed.plan().source().result().id()
            || record.task().metadata().derivation().source_result_digest()
                != fixed.plan().source().result().version())
            throw Error(ErrorCode::conflict, "model backup differs from its fixed plan and source");
        inspect_calculation_program(record, execution);
        required_models.push_back(fixed);
        depend(protocol::model_plan_object(fixed.plan()));
        resource::proto::Object input;
        *input.mutable_owner() = owner;
        input.set_kind("model_training");
        input.set_id(record.task().id());
        input.set_version(record.input_digest());
        if (record.task().has_retirement()
            && record.task().retirement().SerializeAsString() != input.SerializeAsString())
            throw Error(ErrorCode::conflict, "model retirement backup identity mismatch");
        result.objects.push_back(input);
        inputs.emplace(input.id(), input);
        if (record.task().state() == task::proto::SUCCEEDED) {
            auto output = input;
            output.set_kind("model_training_result");
            output.set_version(record.task().result_digest());
            result.objects.push_back(std::move(output));
        }
        return input;
    };
    const auto inspect_model_time = [&](const task::proto::StoredTaskRecord& record,
                                        const task::proto::TaskExecution& execution) {
        if (!execution.has_model_time())
            throw Error(ErrorCode::conflict, "model backup lacks its fixed training input");
        const auto& fixed = execution.model_time();
        protocol::validate_model_time_input(fixed);
        if (fixed.plan().owner().SerializeAsString() != owner.SerializeAsString()
            || execution.input_sha256() != record.input_digest()
            || sha256_bytes(fixed.SerializeAsString()) != record.input_digest()
            || record.task().model_time_plan().SerializeAsString()
                != protocol::model_time_plan_object(fixed.plan()).SerializeAsString()
            || !record.task().metadata().has_derivation()
            || record.task().metadata().derivation().source_id()
                != fixed.plan().recipe().source().result().id()
            || record.task().metadata().derivation().source_result_digest()
                != fixed.plan().recipe().source().result().version())
            throw Error(ErrorCode::conflict, "model backup differs from its fixed plan and source");
        inspect_calculation_program(record, execution);
        required_times.push_back(fixed);
        depend(protocol::model_time_plan_object(fixed.plan()));
        resource::proto::Object input;
        *input.mutable_owner() = owner;
        input.set_kind("model_time_validation");
        input.set_id(record.task().id());
        input.set_version(record.input_digest());
        if (record.task().has_retirement()
            && record.task().retirement().SerializeAsString() != input.SerializeAsString())
            throw Error(ErrorCode::conflict, "model retirement backup identity mismatch");
        result.objects.push_back(input);
        inputs.emplace(input.id(), input);
        if (record.task().state() == task::proto::SUCCEEDED) {
            auto output = input;
            output.set_kind("model_time_validation_result");
            output.set_version(record.task().result_digest());
            result.objects.push_back(std::move(output));
        }
        return input;
    };
    const auto inspect_replay = [&](const task::proto::StoredTaskRecord& record,
                                    const task::proto::TaskExecution& execution) {
        if (!execution.has_model_replay())
            return;
        const auto& fixed = execution.model_replay();
        protocol::validate_model_replay_definition(fixed);
        if (record.task().kind() != task::proto::BACKTEST || !execution.has_backtest()
            || fixed.mapping().owner().SerializeAsString() != owner.SerializeAsString()
            || execution.program_artifact() != fixed.training().plan().recipe().program_artifact())
            throw Error(ErrorCode::conflict, "model replay backup identity mismatch");
        required_replays.push_back(fixed);
        replay_sources.emplace(record.task().id(), fixed);
        required_times.push_back(fixed.training());
        depend(protocol::model_mapping_object(fixed.mapping()));
        depend(fixed.mapping().specification().model());
    };
    const auto inspect = [&](const task::proto::StoredTaskRecord& record,
                             const task::proto::TaskExecution& execution) {
        const auto& task = record.task();
        if (task.metadata().derivation().has_batch()) {
            required_batches.push_back(record);
            depend(task.metadata().derivation().batch());
        }
        source_records.emplace(task.id(), record);
        inspect_replay(record, execution);
        if (task.kind() == task::proto::MODEL_TRAINING) {
            (void)inspect_model(record, execution);
            origins.push_back(task.metadata());
            return;
        }
        if (task.kind() == task::proto::MODEL_TIME_VALIDATION) {
            (void)inspect_model_time(record, execution);
            origins.push_back(task.metadata());
            return;
        }
        if (task.kind() == task::proto::LOGIC_TRIAL) {
            (void)inspect_logic(record, execution);
            return;
        }
        if (task.kind() == task::proto::PERFORMANCE_EVALUATION) {
            (void)inspect_evaluation(record, execution);
            origins.push_back(task.metadata());
            return;
        }
        if (task.kind() == task::proto::STATISTICS) {
            (void)inspect_statistics(record, execution);
            origins.push_back(task.metadata());
            return;
        }
        if (task.kind() == task::proto::ACCOUNT_REVIEW) {
            (void)inspect_review(record, execution);
            return;
        }
        inspect_calculation_program(record, execution);
        if ((task.kind() != task::proto::BACKTEST && task.kind() != task::proto::FACTOR
                && task.kind() != task::proto::SAMPLE)
            || execution.data_owner().node_id().empty()
            || execution.data_owner().service_id().empty())
            throw std::invalid_argument(
                "Research backup requires a fixed Data owner and calculation kind");
        if (task.kind() == task::proto::SAMPLE
            && (!execution.has_sample()
                || execution.sample().owner().SerializeAsString() != owner.SerializeAsString()))
            throw Error(ErrorCode::conflict, "invalid research sample owner");
        tasks::ResearchDataRoute route{execution.data_owner(), {}, owner.node_id()};
        const auto retention =
            tasks::research_retention(execution, route, owner.service_id(), task.id());
        tasks::verify_research_retention(root / task.id(), retention);
        if (task.has_retirement()
            && task.retirement().SerializeAsString() != retention.consumer().SerializeAsString())
            throw std::invalid_argument("retired Research backup owner mismatch");
        result.references.push_back(
            {retention.source(),
                retention.consumer(),
                sha256_bytes(retention.SerializeAsString()),
                task.has_retirement()});
        result.objects.push_back(retention.consumer());
        inputs.emplace(task.id(), retention.consumer());
        for (const auto& version : retention.versions()) {
            resource::proto::Object required;
            auto* source = required.mutable_owner();
            source->set_node_id(retention.source().node_id());
            source->set_service_id(retention.source().service_id());
            source->set_domain("data");
            required.set_kind(
                version.kind() == data::proto::DataVersion::MINUTE_VERSION ? "minute_version"
                                                                           : "table_snapshot");
            required.set_id(version.version());
            required.set_version(version.version());
            depend(std::move(required));
        }
        if (task.state() == task::proto::SUCCEEDED) {
            resource::proto::Object output;
            *output.mutable_owner() = owner;
            output.set_kind(
                task.kind() == task::proto::BACKTEST     ? "backtest_result"
                    : task.kind() == task::proto::SAMPLE ? "sample_result"
                                                         : "factor_result");
            output.set_id(task.id());
            output.set_version(task.result_digest());
            result.objects.push_back(std::move(output));
        }
        if (task.has_metadata() && !task.metadata().derivation().source_id().empty())
            origins.push_back(task.metadata());
    };
    const auto inspect_abandoned = [&](const task::proto::PreparedTaskSubmission& evidence,
                                       const resource::proto::Object& decision) {
        const auto& execution = evidence.execution();
        if (evidence.record().task().metadata().derivation().has_batch()) {
            required_batches.push_back(evidence.record());
            depend(evidence.record().task().metadata().derivation().batch());
        }
        if (evidence.record().task().kind() == task::proto::MODEL_TRAINING) {
            if (inspect_model(evidence.record(), execution).SerializeAsString()
                != decision.SerializeAsString())
                throw Error(ErrorCode::conflict, "model abandonment backup identity mismatch");
            return;
        }
        if (evidence.record().task().kind() == task::proto::MODEL_TIME_VALIDATION) {
            if (inspect_model_time(evidence.record(), execution).SerializeAsString()
                != decision.SerializeAsString())
                throw Error(ErrorCode::conflict, "model abandonment backup identity mismatch");
            return;
        }
        if (evidence.record().task().kind() == task::proto::LOGIC_TRIAL) {
            if (inspect_logic(evidence.record(), execution).SerializeAsString()
                != decision.SerializeAsString())
                throw Error(ErrorCode::conflict, "logic trial abandonment backup mismatch");
            return;
        }
        if (evidence.record().task().kind() == task::proto::PERFORMANCE_EVALUATION) {
            if (inspect_evaluation(evidence.record(), execution).SerializeAsString()
                != decision.SerializeAsString())
                throw Error(ErrorCode::conflict, "evaluation abandonment backup identity mismatch");
            return;
        }
        if (evidence.record().task().kind() == task::proto::STATISTICS) {
            if (inspect_statistics(evidence.record(), execution).SerializeAsString()
                != decision.SerializeAsString())
                throw Error(ErrorCode::conflict, "invalid statistics source identity");
            return;
        }
        if (evidence.record().task().kind() == task::proto::ACCOUNT_REVIEW) {
            if (inspect_review(evidence.record(), execution).SerializeAsString()
                != decision.SerializeAsString())
                throw Error(ErrorCode::conflict, "account review abandonment backup mismatch");
            return;
        }
        inspect_calculation_program(evidence.record(), execution);
        inspect_replay(evidence.record(), execution);
        tasks::ResearchDataRoute route{execution.data_owner(), {}, owner.node_id()};
        const auto retention = tasks::research_retention(
            execution,
            route,
            owner.service_id(),
            evidence.record().task().id());
        if (decision.SerializeAsString() != retention.consumer().SerializeAsString())
            throw std::invalid_argument("Research abandonment owner mismatch");
        if (fs::exists(root / decision.id() / "data-retention.pb"))
            tasks::verify_research_retention(root / decision.id(), retention);
        result.references.push_back(
            {retention.source(), decision, sha256_bytes(retention.SerializeAsString()), true});
        result.objects.push_back(decision);
    };
    const auto catalogue = tasks::Store::inspect_backup(
        root,
        {owner.service_id()},
        inspect,
        inspect,
        inspect_abandoned,
        stop);
    for (const auto& source : statistics_sources) {
        const auto found = source_records.find(source.result().id());
        if (found == source_records.end())
            throw std::invalid_argument("Research origin record is missing");
        const auto& record = found->second;
        if (record.task().kind()
                != (source.result().kind() == "sample_result" ? task::proto::SAMPLE
                                                              : task::proto::FACTOR)
            || record.task().state() != task::proto::SUCCEEDED
            || record.task().result_digest() != source.result().version()
            || record.input_digest() != source.input_sha256()
            || record.input_bytes() != source.input_bytes()
            || record.task().attempt() != source.attempt()
            || record.task().program_artifact() != source.program_artifact())
            throw Error(
                ErrorCode::conflict,
                "statistics source evidence does not match its captured identity");
    }
    for (const auto& fixed : evaluation_sources) {
        const auto& source = fixed.request().source();
        const auto found = source_records.find(source.id());
        if (found == source_records.end())
            throw std::invalid_argument("Research origin record is missing");
        const auto& record = found->second;
        if (record.task().kind() != task::proto::BACKTEST
            || record.task().state() != task::proto::SUCCEEDED
            || record.task().result_digest() != source.version()
            || record.input_digest() != fixed.source().input_sha256()
            || record.input_bytes() != fixed.source().input_bytes()
            || record.task().attempt() != fixed.source().attempt()
            || record.task().program_artifact() != fixed.source().program_artifact())
            throw Error(ErrorCode::conflict, "evaluation backup source identity mismatch");
    }
    Projects::inspect_backup(
        root,
        owner,
        required_plans,
        [&](const proto::ProjectDraft& draft) {
            resource::proto::Object object;
            *object.mutable_owner() = owner;
            object.set_kind("research_project");
            object.set_id(draft.id());
            object.set_version(draft.digest());
            result.objects.push_back(std::move(object));
        },
        [&](const proto::HypothesisPlan& plan) {
            resource::proto::Object object;
            *object.mutable_owner() = owner;
            object.set_kind("research_plan");
            object.set_id(plan.id());
            object.set_version(plan.definition_digest());
            result.objects.push_back(std::move(object));
        },
        [&](const proto::ProjectConclusion& conclusion) {
            resource::proto::Object object;
            *object.mutable_owner() = owner;
            object.set_kind("research_conclusion");
            object.set_id(conclusion.declaration().id());
            object.set_version(conclusion.digest());
            result.objects.push_back(std::move(object));
            depend(conclusion.declaration().result());
            resource::proto::Object plan;
            *plan.mutable_owner() = owner;
            plan.set_kind("research_plan");
            plan.set_id(conclusion.declaration().plan().plan_id());
            plan.set_version(conclusion.declaration().plan().definition_digest());
            depend(std::move(plan));
        },
        stop);
    Models::inspect_backup(
        root,
        owner,
        required_models,
        [&](const proto::ModelDraft& draft) {
            result.objects.push_back(protocol::model_draft_object(draft));
            if (draft.specification().has_source())
                depend(draft.specification().source());
        },
        [&](const proto::ModelPlan& plan) {
            result.objects.push_back(protocol::model_plan_object(plan));
            depend(plan.source().result());
            auto source = plan.source().result();
            source.set_kind("factor");
            source.set_version(plan.source().input_sha256());
            depend(std::move(source));
            if (plan.source().has_variable_definition())
                depend(plan.source().variable_definition());
            for (const auto& artifact :
                {plan.program_artifact(), plan.source().program_artifact()}) {
                required_programs.insert(artifact);
                resource::proto::Object program;
                *program.mutable_owner() = owner;
                program.set_kind("research_program");
                program.set_id(artifact);
                program.set_version(artifact);
                depend(std::move(program));
            }
        },
        stop,
        [&](const proto::ModelMapping& mapping) {
            result.objects.push_back(protocol::model_mapping_object(mapping));
            retained_mappings.emplace(mapping.id(), mapping);
            depend(mapping.specification().model());
            depend(mapping.plan());
        },
        [&](const proto::ModelTimePlan& time_plan) {
            result.objects.push_back(protocol::model_time_plan_object(time_plan));
            depend(time_plan.specification().recipe());
            retained_time_plans.emplace(time_plan.id(), time_plan);
        });
    for (const auto& replay : required_replays) {
        const auto found = retained_mappings.find(replay.mapping().id());
        if (found == retained_mappings.end()
            || found->second.SerializeAsString() != replay.mapping().SerializeAsString())
            throw Error(
                ErrorCode::conflict,
                "model replay differs from its retained immutable mapping");
    }
    for (const auto& input : required_times) {
        const auto found = retained_time_plans.find(input.plan().id());
        if (found == retained_time_plans.end()
            || found->second.SerializeAsString() != input.plan().SerializeAsString())
            throw Error(
                ErrorCode::conflict,
                "model time task differs from its retained immutable plan");
    }

    Variables::inspect_backup(
        root,
        owner,
        required_variables,
        [&](const proto::VariableDefinition& value) {
            result.objects.push_back(protocol::variable_object(value));
        },
        stop);
    Logic::inspect_backup(
        root,
        owner,
        required_logic,
        [&](const proto::LogicDefinition& value) {
            result.objects.push_back(protocol::logic_object(value));
        },
        [&](const proto::LogicExample& value) {
            result.objects.push_back(protocol::logic_object(value));
            depend(value.definition());
        },
        stop);
    Batches::inspect_backup(
        root,
        owner,
        required_batches,
        [&](const proto::BacktestBatchState& value) {
            const auto& definition = value.definition();
            result.objects.push_back(protocol::backtest_batch_object(definition));
            depend(definition.request().source());
            auto input = definition.request().source();
            input.set_kind("backtest");
            input.set_version(definition.source_input_sha256());
            depend(std::move(input));
            required_programs.insert(definition.program_artifact());
            resource::proto::Object program;
            *program.mutable_owner() = owner;
            program.set_kind("research_program");
            program.set_id(definition.program_artifact());
            program.set_version(definition.program_artifact());
            depend(std::move(program));
        },
        stop);
    std::vector<std::string> assessment_parents;
    Batches::inspect_assessments(
        root,
        owner,
        [&](const proto::BatchAssessment& value) {
            result.objects.push_back(protocol::batch_assessment_object(value));
            depend(value.declaration().batch());
            if (!value.declaration().previous_id().empty())
                assessment_parents.push_back(value.declaration().previous_id());
            const auto keep_program = [&](const std::string& artifact) {
                required_programs.insert(artifact);
                resource::proto::Object program;
                *program.mutable_owner() = owner;
                program.set_kind("research_program");
                program.set_id(artifact);
                program.set_version(artifact);
                depend(std::move(program));
            };
            keep_program(value.program_artifact());
            const auto keep_original = [&](const proto::BatchAssessmentTask& observed) {
                resource::proto::Object input;
                *input.mutable_owner() = owner;
                input.set_kind("backtest");
                input.set_id(observed.id());
                input.set_version(observed.input_sha256());
                depend(input);
                if (observed.state() == task::proto::SUCCEEDED) {
                    input.set_kind("backtest_result");
                    input.set_version(observed.result_sha256());
                    depend(std::move(input));
                }
                keep_program(observed.program_artifact());
            };
            keep_original(value.snapshot().source());
            for (const auto& observed : value.snapshot().members())
                keep_original(observed);
        },
        stop);
    for (const auto& id : assessment_parents) {
        const auto parent = std::ranges::find_if(result.objects, [&](const auto& object) {
            return object.kind() == "backtest_batch_assessment" && object.id() == id;
        });
        if (parent == result.objects.end())
            throw Error(ErrorCode::conflict, "batch assessment retained source task is missing");
        depend(*parent);
    }
    Strategies::inspect_backup(
        root,
        owner,
        required_strategies,
        [&](const proto::StrategyPackage& value) {
            result.objects.push_back(protocol::strategy_package_object(value));
            depend(protocol::strategy_source(value));
            if (value.definition().has_model_replay()) {
                const auto& fixed = value.definition().model_replay();
                const auto source = replay_sources.find(value.reference_result().id());
                if (source == replay_sources.end()
                    || source->second.SerializeAsString() != fixed.SerializeAsString())
                    throw Error(
                        ErrorCode::conflict,
                        "model package differs from its actual reference simulation");
                const auto mapping = retained_mappings.find(fixed.mapping().id());
                if (mapping == retained_mappings.end()
                    || mapping->second.SerializeAsString() != fixed.mapping().SerializeAsString())
                    throw Error(
                        ErrorCode::conflict,
                        "model package fixed mapping is missing or changed");
                depend(protocol::model_mapping_object(fixed.mapping()));
                depend(protocol::model_time_plan_object(fixed.training().plan()));
                depend(fixed.mapping().specification().model());
            }
            if (value.has_logic()) {
                depend(protocol::logic_object(value.logic().definition()));
                depend(value.logic().example());
                auto input = value.logic().result();
                input.set_kind("logic_trial");
                input.set_version(value.source_input_sha256());
                depend(std::move(input));
                required_programs.insert(value.definition().risk_artifact());
                resource::proto::Object risk;
                *risk.mutable_owner() = owner;
                risk.set_kind("research_program");
                risk.set_id(value.definition().risk_artifact());
                risk.set_version(value.definition().risk_artifact());
                depend(std::move(risk));
            }
            required_programs.insert(value.definition().program_artifact());
            resource::proto::Object program;
            *program.mutable_owner() = owner;
            program.set_kind("research_program");
            program.set_id(value.definition().program_artifact());
            program.set_version(value.definition().program_artifact());
            depend(std::move(program));
            // The TaskStore inspection already verifies existing module bytes.
            // A retired source may otherwise omit its released module: a retained
            // candidate still declares the original implementation as evidence.
            if (value.has_logic())
                return; // The preserved native risk bytes are checked below.
            const auto source = source_records.find(value.reference_result().id());
            if (source == source_records.end()
                || source->second.task().risk_artifact() != value.definition().risk_artifact()
                || !fs::is_regular_file(
                    risk_providers::Module::filename(root / value.reference_result().id())))
                throw Error(
                    ErrorCode::conflict,
                    "strategy original risk implementation is missing");
        },
        [&](const proto::StrategyDelivery& value) {
            result.objects.push_back(protocol::strategy_delivery_object(value));
            depend(value.package());
            depend(protocol::strategy_source(value));
        },
        stop);
    const auto add = [&](const fs::path& relative, const std::string& format) {
        const auto source = root / relative;
        if (fs::is_symlink(source) || !fs::is_regular_file(source))
            throw std::invalid_argument("Research backup evidence missing");
        if (format == "coordination" && fs::file_size(source))
            throw std::invalid_argument("Research coordination slot is not empty");
        snapshot.files.push_back({source, relative, format});
    };
    for (const auto& file : catalogue.files)
        add(file,
            file.filename() == "manager.lock"                               ? "coordination"
                : file.extension() == ".sqlite"                             ? "sqlite"
                : file.extension() == ".json"                               ? "json"
                : file.extension() == ".so" || file.extension() == ".dylib" ? "binary"
                                                                            : "protobuf");
    snapshot.directories = catalogue.directories;
    if (fs::exists(root / "programs") || !required_programs.empty()) {
        programs::Files programs(root / "programs", owner);
        std::string cursor;
        std::set<std::string> preserved;
        for (;;) {
            const auto page = programs.catalogue(cursor, 200);
            if (page.empty())
                break;
            for (const auto& entry : page) {
                (void)programs.verify(entry.digest, stop);
                preserved.insert(entry.digest);
                add(fs::path("programs") / entry.digest, "binary");
                resource::proto::Object object;
                *object.mutable_owner() = owner;
                object.set_kind("research_program");
                object.set_id(entry.digest);
                object.set_version(entry.digest);
                result.objects.push_back(std::move(object));
            }
            cursor = page.back().digest;
        }
        for (const auto& required : required_programs)
            if (!preserved.contains(required))
                throw std::invalid_argument("Research original execution program is missing");
        add("programs/owner.pb", "protobuf");
        snapshot.directories.push_back("programs");
        snapshot.directories.push_back("programs/staging");
    }
    for (const auto& reference : result.references) {
        const auto relative = fs::path(reference.consumer.id()) / "data-retention.pb";
        if (!reference.released || fs::exists(root / relative))
            add(relative, "protobuf");
    }
    // Current derivation admission is scoped to this Research ledger. Preserve
    // exact parent input/result identities; do not infer an unrelated node.
    for (const auto& metadata : origins) {
        const auto& origin = metadata.derivation();
        if (origin.source_instance() != owner.service_id())
            throw Error(
                ErrorCode::conflict,
                "cross-owner Research origin needs explicit stable ownership");
        const auto found = inputs.find(origin.source_id());
        if (found == inputs.end())
            throw std::invalid_argument("Research origin record is missing");
        auto required = found->second;
        if (!origin.source_result_digest().empty()) {
            required.set_kind(
                required.kind() == "backtest"     ? "backtest_result"
                    : required.kind() == "sample" ? "sample_result"
                                                  : "factor_result");
            required.set_version(origin.source_result_digest());
        }
        depend(std::move(required));
    }
    snapshot.exclusions = {"runtime_sockets", "logs", "temporary_files", "process_and_lease_state"};
    if (missing_original_program)
        snapshot.limitations.push_back("original_execution_program_identity_not_recorded");
    std::set<fs::path> declared;
    for (const auto& file : snapshot.files)
        declared.insert(file.relative);
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "Research cold backup interrupted");
        if (entry.is_symlink())
            throw std::invalid_argument("Research backup rejects symbolic links");
        if (!entry.is_regular_file())
            continue;
        const auto name = entry.path().filename().string();
        const auto relative = entry.path().lexically_relative(root);
        if (relative.parent_path() == "programs/staging"
            || (relative.parent_path() == "programs" && name.starts_with("owner.pb.tmp.")))
            continue; // Unpublished programme preparation, excluded rather than restored as an asset.
        if (name.ends_with("-shm"))
            continue;
        if (name.ends_with("-wal") || name.ends_with("-journal")) {
            if (fs::file_size(entry.path()))
                throw Error(ErrorCode::unavailable, "Research writer recovery required");
            continue;
        }
        if (!declared.contains(entry.path().lexically_relative(root)))
            throw std::invalid_argument("undeclared Research evidence; owner inspection required");
    }
    return result;
}

} // namespace asterion::research
