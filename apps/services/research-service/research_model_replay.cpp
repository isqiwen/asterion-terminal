#include "research_models.hpp"
#include "backtest/model_strategy.hpp"
#include "program_files.hpp"
#include "task_payload.hpp"
#include <asterion/protocol/research_model.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/foundation/error.hpp>

namespace asterion::research {

backtest::ModelReplayData resolve_model_replay_data(
    const backtest::proto::BacktestInput& input,
    const std::filesystem::path& root,
    const std::string& loaded_program,
    std::stop_token stop)
{
    if (!input.has_model_replay() || input.paper().contracts_size() != 1)
        throw std::invalid_argument("model replay requires one fixed account contract");
    const auto& definition = input.model_replay();
    protocol::validate_model_replay_definition(definition);
    const auto& plan = definition.training().plan();
    const auto& source = plan.recipe().source();
    if (loaded_program.empty() || loaded_program != plan.recipe().program_artifact())
        throw Error(ErrorCode::conflict, "model replay requires its fixed training program");
    backtest::ModelReplayData data;
    data.deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(definition.time_budget_seconds());
    const auto check = [&] {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "model replay cancelled");
        if (std::chrono::steady_clock::now() >= data.deadline)
            throw Error(ErrorCode::resource_exhausted, "model replay time budget exhausted");
        if (SystemClock{}.utc_now() / 1000000
            >= definition.mapping().specification().valid_until_ms())
            throw Error(ErrorCode::conflict, "model replay mapping expired before completion");
    };
    check();
    (void)programs::Files(root / "programs", plan.owner()).verify(loaded_program, stop);
    if (source.program_artifact() != loaded_program)
        (void)programs::Files(root / "programs", plan.owner())
            .verify(source.program_artifact(), stop);
    check();
    const auto& model = definition.mapping().specification().model();
    const auto original_time_input = definition.training().SerializeAsString();
    (void)tasks::payload::read(
        root / model.id() / "input.pb",
        sha256_bytes(original_time_input),
        tasks::payload::max_input_bytes,
        original_time_input.size());
    proto::ModelTimeResult reference;
    tasks::payload::parse(
        tasks::payload::read(
            root / model.id() / "results" / (std::to_string(definition.model_attempt()) + ".pb"),
            model.version(),
            tasks::payload::max_result_bytes),
        reference);
    protocol::validate_model_time_result(definition.training(), reference);
    check();
    factor::proto::FactorInput original;
    factor::proto::FactorResult observations;
    tasks::payload::parse(
        tasks::payload::read(
            root / source.result().id() / "input.pb",
            source.input_sha256(),
            tasks::payload::max_input_bytes,
            source.input_bytes()),
        original);
    check();
    tasks::payload::parse(
        tasks::payload::read(
            root / source.result().id() / "results" / (std::to_string(source.attempt()) + ".pb"),
            source.result().version(),
            tasks::payload::max_result_bytes),
        observations);
    protocol::validate_model_source(source, original, observations);
    if (original.series_size() != 1 || !original.series(0).has_bars()
        || original.series(0).bars().interval_minutes() != 1
        || original.series(0).bars().SerializeAsString()
            != input.paper().contracts(0).dataset().SerializeAsString()
        || original.data_owner().SerializeAsString() != input.data_owner().SerializeAsString())
        throw Error(
            ErrorCode::conflict,
            "model replay account bars differ from original fixed factor data");
    check();
    data.training_members =
        model_time_training_indices(plan, observations, definition.fold_index());
    for (const auto member : data.training_members) {
        const auto& row = observations.observations(static_cast<int>(member));
        data.training.push_back({row.value(), row.forward_return()});
    }
    const auto& fold = plan.specification().folds(static_cast<int>(definition.fold_index()));
    data.features.reserve(fold.validation_end());
    for (unsigned i = 0; i < fold.validation_end(); ++i) {
        if (i % 1024 == 0)
            check();
        const auto& row = observations.observations(static_cast<int>(i));
        data.features.push_back(row.has_value() ? std::optional(row.value()) : std::nullopt);
    }
    data.source_revision = original.series(0).bars().revision();
    data.data_owner = original.data_owner();
    data.plan_digest = plan.digest();
    data.fold_index = definition.fold_index();
    check();
    return data;
}

} // namespace asterion::research
