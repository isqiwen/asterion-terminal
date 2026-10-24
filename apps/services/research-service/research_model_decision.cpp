#include "research_models.hpp"
#include "model_regression.hpp"
#include "program_files.hpp"
#include "task_payload.hpp"
#include <asterion/protocol/research_model.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/foundation/error.hpp>
#include <cmath>

namespace asterion::research {

proto::ModelDecisionTrialResult compute_model_decision_trial(
    const proto::ModelDecisionTrialInput& input,
    const std::filesystem::path& root,
    std::stop_token stop,
    const tasks::TaskProgress& progress,
    const Clock& clock)
{
    protocol::validate_model_decision_input(input);
    const auto& mapping = input.mapping();
    const auto& request = input.request();
    const auto& model = mapping.specification().model();
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(request.time_budget_seconds());
    const auto check = [&] {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "model decision trial cancelled");
        if (std::chrono::steady_clock::now() >= deadline)
            throw Error(
                ErrorCode::resource_exhausted,
                "model decision trial time budget exhausted");
        if (clock.utc_now() / 1000000 >= mapping.specification().valid_until_ms())
            throw Error(
                ErrorCode::conflict,
                "model mapping expired before decision trial completion");
    };
    check();
    const bool time = input.has_time();
    const auto& recipe = time ? input.time().plan().recipe() : input.training().plan();
    (void)programs::Files(root / "programs", mapping.owner())
        .verify(recipe.program_artifact(), stop);
    check();
    const auto original_input =
        time ? input.time().SerializeAsString() : input.training().SerializeAsString();
    (void)tasks::payload::read(
        root / model.id() / "input.pb",
        sha256_bytes(original_input),
        tasks::payload::max_input_bytes,
        original_input.size());
    check();
    const auto original_result = tasks::payload::read(
        root / model.id() / "results" / (std::to_string(input.model_attempt()) + ".pb"),
        model.version(),
        tasks::payload::max_result_bytes);
    check();
    models::RegressionFit fitted{};
    std::optional<models::Standardization> scale;
    const auto standardization = [&](const auto& source) {
        if (source.has_standardization()) {
            const auto& s = source.standardization();
            scale = models::Standardization{
                s.training_samples(),
                s.mean(),
                s.sample_standard_deviation()
            };
        }
    };
    if (time) {
        proto::ModelTimeResult original;
        tasks::payload::parse(original_result, original);
        protocol::validate_model_time_result(input.time(), original);
        const auto& fit = original.folds(static_cast<int>(request.fold_index()));
        fitted = {
            static_cast<std::size_t>(fit.training_members_size()),
            fit.intercept(),
            fit.coefficient(),
            fit.training_residual_sum_squares(),
            fit.regularization(),
            fit.training_objective()
        };
        standardization(fit);
    } else {
        proto::ModelTrainingResult original;
        tasks::payload::parse(original_result, original);
        protocol::validate_model_training_result(input.training(), original);
        fitted = {
            original.training_samples(),
            original.intercept(),
            original.coefficient(),
            original.residual_sum_squares(),
            original.regularization(),
            original.objective()
        };
        standardization(original);
    }
    const auto model_ready = mapping.model_published_at_ms() * 1000000;
    const auto mapping_ready = mapping.recorded_at_ms() * 1000000;
    const auto expires = mapping.specification().valid_until_ms() * 1000000;
    const models::ThresholdMapping thresholds{
        static_cast<double>(mapping.specification().short_threshold().units()) / 100000000.0,
        static_cast<double>(mapping.specification().long_threshold().units()) / 100000000.0
    };
    proto::ModelDecisionTrialResult result;
    unsigned matched = 0;
    for (int i = 0; i < request.cases_size(); ++i) {
        check();
        const auto& item = request.cases(i);
        auto& row = *result.add_rows();
        row.set_index(static_cast<unsigned>(i));
        row.set_intent(proto::ModelMappedRow::PREDICTION_UNAVAILABLE);
        if (item.boundary_ns() < model_ready)
            row.set_reason(proto::MODEL_NOT_READY);
        else if (item.boundary_ns() < mapping_ready)
            row.set_reason(proto::MODEL_MAPPING_NOT_READY);
        else if (item.boundary_ns() >= expires) {
            row.set_reason(proto::MODEL_DECISION_EXPIRED);
            row.set_intent(proto::ModelMappedRow::EXPIRED);
        } else if (!item.has_feature())
            row.set_reason(proto::MODEL_FEATURE_UNAVAILABLE);
        else if (item.feature_available_at_ns() > item.boundary_ns())
            row.set_reason(proto::MODEL_INPUT_NOT_READY);
        else {
            std::optional<double> prediction;
            try {
                // The fixed source parameters and finite feature were checked above.
                // Only numeric overflow remains unavailable; source/IO failures are not caught.
                const auto feature =
                    scale ? models::standardize_feature(*scale, item.feature()) : item.feature();
                prediction = models::predict_regression(fitted, feature);
            } catch (const std::invalid_argument&) {
                row.set_reason(proto::MODEL_NON_FINITE_PREDICTION);
            }
            if (prediction) {
                row.set_prediction(*prediction);
                row.set_reason(proto::MODEL_DECISION_READY);
                switch (models::map_prediction(thresholds, prediction)) {
                    case models::PredictionIntent::long_direction:
                        row.set_intent(proto::ModelMappedRow::LONG_DIRECTION);
                        break;
                    case models::PredictionIntent::short_direction:
                        row.set_intent(proto::ModelMappedRow::SHORT_DIRECTION);
                        break;
                    case models::PredictionIntent::no_new_direction:
                        row.set_intent(proto::ModelMappedRow::NO_NEW_DIRECTION);
                        break;
                    case models::PredictionIntent::unavailable:
                        throw Error(
                            ErrorCode::internal_error,
                            "finite model decision prediction became unavailable");
                }
            }
        }
        const bool same =
            row.intent() == item.expected_intent() && row.reason() == item.expected_reason();
        row.set_matches(same);
        matched += same;
        if (progress)
            progress(
                static_cast<std::size_t>(i + 1),
                static_cast<std::size_t>(request.cases_size()));
    }
    check();
    result.set_matched(matched);
    result.set_all_match(matched == static_cast<unsigned>(request.cases_size()));
    result.set_computed_at_ms(clock.utc_now() / 1000000);
    protocol::validate_model_decision_result(input, result);
    return result;
}

} // namespace asterion::research
