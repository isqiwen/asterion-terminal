#include "research_models.hpp"
#include <asterion/protocol/research_model.hpp>
#include <asterion/protocol/factor.hpp>
#include <array>
#include <asterion/protocol/research_variable.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/foundation/error.hpp>
#include "model_regression.hpp"
#include "model_validation.hpp"
#include "program_files.hpp"
#include "task_payload.hpp"
#include <cmath>
#include <algorithm>

namespace asterion::research {

namespace {

void capture_time_boundaries(proto::ModelTimePlan& plan, const factor::proto::FactorInput& input)
{
    const auto clocks = protocol::factor_observation_clocks(input);
    for (const auto& fold : plan.specification().folds()) {
        for (const auto index : std::array{
                 fold.training_begin(),
                 fold.training_end(),
                 fold.validation_begin(),
                 fold.validation_end()
             }) {
            if (index >= clocks.size())
                throw std::invalid_argument("model time boundary is outside original observations");
            const auto& clock = clocks[index];
            auto& boundary = *plan.add_boundaries();
            boundary.set_event_index(index);
            boundary.set_trading_day(clock.trading_day);
            if (clock.timestamp_ns)
                boundary.set_timestamp_ns(*clock.timestamp_ns);
        }
    }
}

std::vector<std::uint32_t>
training_indices(const proto::ModelSpecification& scope, const factor::proto::FactorResult& result)
{
    std::vector<std::uint32_t> indices;
    for (const auto& row : result.observations()) {
        if (row.event_index() < scope.training_begin() || row.event_index() >= scope.training_end())
            continue;
        if (row.role() == factor::proto::FactorObservation::DEVELOPMENT && !row.evaluation_warmup()
            && row.has_value() && row.has_forward_return() && row.has_label_end_index()
            && row.label_end_index() < scope.training_end())
            indices.push_back(row.event_index());
    }
    return indices;
}

} // namespace

std::vector<std::uint32_t> model_time_training_indices(
    const proto::ModelTimePlan& plan,
    const factor::proto::FactorResult& result,
    unsigned selected)
{
    const auto& fold = plan.specification().folds(static_cast<int>(selected));
    auto scope = plan.recipe().specification();
    scope.set_training_begin(fold.training_begin());
    scope.set_training_end(fold.training_end());
    auto members = training_indices(scope, result);
    if (members.size() != plan.training_counts(static_cast<int>(selected)))
        throw Error(ErrorCode::conflict, "model time mature fold membership changed");
    return members;
}

std::vector<std::uint32_t> model_training_indices(
    const proto::ModelPlan& plan,
    const factor::proto::FactorInput& input,
    const factor::proto::FactorResult& result)
{
    protocol::validate_model_plan(plan);
    protocol::validate_model_source(plan.source(), input, result);
    auto indices = training_indices(plan.specification(), result);
    if (indices.size() != plan.training_samples())
        throw Error(ErrorCode::conflict, "model mature training membership changed");
    return indices;
}

proto::ModelPlan capture_model_plan(
    const resource::proto::Owner& owner,
    const proto::ModelDraft& draft,
    const std::string& plan_id,
    const task::proto::TaskResponse& verified,
    const std::string& input_digest,
    std::uint64_t input_bytes,
    const std::string& current_program,
    std::int64_t recorded_at_ms)
{
    protocol::validate_model_draft(draft);
    protocol::validate_model_specification(draft.specification());
    const auto& selected = draft.specification().source();
    const auto& task = verified.result_task();
    if (draft.owner().SerializeAsString() != owner.SerializeAsString()
        || selected.owner().SerializeAsString() != owner.SerializeAsString()
        || !verified.has_result_task() || !verified.has_factor() || !task.has_factor()
        || task.kind() != task::proto::FACTOR || task.state() != task::proto::SUCCEEDED
        || task.has_retirement() || task.id() != selected.id()
        || task.result_digest() != selected.version()
        || input_digest != sha256_bytes(task.factor().SerializeAsString())
        || input_bytes != task.factor().ByteSizeLong()
        || selected.version() != sha256_bytes(verified.factor().SerializeAsString()))
        throw Error(
            ErrorCode::conflict,
            "model requires its verified original factor input and result");
    proto::ModelPlan plan;
    plan.set_id(plan_id);
    plan.set_draft_id(draft.id());
    plan.set_draft_digest(draft.digest());
    *plan.mutable_owner() = owner;
    *plan.mutable_specification() = draft.specification();
    plan.set_program_artifact(current_program);
    plan.set_recorded_at_ms(recorded_at_ms);
    auto& source = *plan.mutable_source();
    *source.mutable_result() = selected;
    source.set_input_sha256(input_digest);
    source.set_input_bytes(input_bytes);
    source.set_attempt(task.attempt());
    source.set_program_artifact(task.program_artifact());
    source.set_observations(verified.factor().input_count());
    source.set_holdout_start(task.factor().holdout_start());
    source.set_horizon(task.factor().horizon());
    source.set_required_history(verified.factor().evaluation_warmup());
    if (task.factor().lookbacks_size() == 1)
        source.set_lookback(task.factor().lookbacks(0));
    source.set_factor(task.factor().factor());
    if (task.factor().has_variable_definition()) {
        source.set_price_power(task.factor().variable_definition().price_power());
        *source.mutable_variable_definition() =
            protocol::variable_object(task.factor().variable_definition());
    }
    source.set_historical_availability_unknown(true);
    protocol::validate_model_source(source, task.factor(), verified.factor());
    const auto samples = training_indices(plan.specification(), verified.factor()).size();
    if (samples < 2)
        throw Error(
            ErrorCode::conflict,
            "model has fewer than two eligible mature training labels");
    plan.set_training_samples(static_cast<unsigned>(samples));
    plan.set_digest(protocol::model_plan_digest(plan));
    protocol::validate_model_plan(plan);
    return plan;
}

proto::ModelTimePlan capture_model_time_plan(
    const resource::proto::Owner& owner,
    const proto::ModelTimeSave& request,
    const proto::ModelPlan& recipe,
    const task::proto::TaskResponse& verified,
    std::int64_t recorded_at_ms,
    std::stop_token stop)
{
    protocol::validate_model_time_specification(request.specification());
    protocol::validate_model_plan(recipe);
    const auto& source = recipe.source();
    const auto& task = verified.result_task();
    if (recipe.owner().SerializeAsString() != owner.SerializeAsString()
        || request.specification().recipe().SerializeAsString()
            != protocol::model_plan_object(recipe).SerializeAsString()
        || !verified.has_result_task() || !verified.has_factor() || !task.has_factor()
        || task.kind() != task::proto::FACTOR || task.state() != task::proto::SUCCEEDED
        || task.has_retirement() || task.id() != source.result().id()
        || task.result_digest() != source.result().version()
        || task.result_digest() != sha256_bytes(verified.factor().SerializeAsString())
        || source.input_sha256() != sha256_bytes(task.factor().SerializeAsString())
        || source.input_bytes() != task.factor().ByteSizeLong()
        || source.attempt() != task.attempt()
        || source.program_artifact() != task.program_artifact())
        throw Error(
            ErrorCode::conflict,
            "model time plan requires its verified original causal source and recipe");
    protocol::validate_model_source(source, task.factor(), verified.factor());
    proto::ModelTimePlan plan;
    plan.set_id(request.plan_id());
    *plan.mutable_owner() = owner;
    *plan.mutable_specification() = request.specification();
    *plan.mutable_recipe() = recipe;
    plan.set_recorded_at_ms(recorded_at_ms);
    for (const auto& fold : request.specification().folds()) {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "model time plan verification cancelled");
        auto scope = recipe.specification();
        scope.set_training_begin(fold.training_begin());
        scope.set_training_end(fold.training_end());
        plan.add_training_counts(
            static_cast<unsigned>(training_indices(scope, verified.factor()).size()));
    }
    capture_time_boundaries(plan, task.factor());
    plan.set_digest(protocol::model_time_plan_digest(plan));
    protocol::validate_model_time_plan(plan);
    return plan;
}

proto::ModelTimeResult validate_model_time(
    const proto::ModelTimeInput& input,
    const std::filesystem::path& root,
    std::stop_token stop,
    const tasks::TaskProgress& progress,
    const Clock& clock)
{
    protocol::validate_model_time_input(input);
    const auto& plan = input.plan();
    const auto& recipe = plan.recipe();
    const auto& source = recipe.source();
    const auto& specification = plan.specification();
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(specification.time_budget_seconds());
    const auto check = [&] {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "model time validation cancelled");
        if (std::chrono::steady_clock::now() >= deadline)
            throw Error(ErrorCode::resource_exhausted, "model time validation budget exhausted");
        if (clock.utc_now() / 1000000 >= specification.valid_until_ms())
            throw Error(ErrorCode::conflict, "model time plan expired before completion");
    };
    check();
    (void)programs::Files(root / "programs", plan.owner()).verify(recipe.program_artifact(), stop);
    if (source.program_artifact() != recipe.program_artifact())
        (void)programs::Files(root / "programs", plan.owner())
            .verify(source.program_artifact(), stop);
    factor::proto::FactorInput original;
    factor::proto::FactorResult evidence;
    check();
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
        evidence);
    check();
    protocol::validate_model_source(source, original, evidence);
    auto original_boundaries = plan;
    original_boundaries.clear_boundaries();
    capture_time_boundaries(original_boundaries, original);
    if (original_boundaries.SerializeAsString() != plan.SerializeAsString())
        throw Error(ErrorCode::conflict, "model time source boundary mismatch");
    std::vector<models::RegressionFold> scopes;
    scopes.reserve(specification.folds_size());
    for (int i = 0; i < specification.folds_size(); ++i) {
        check();
        const auto& fold = specification.folds(i);
        (void)model_time_training_indices(plan, evidence, static_cast<unsigned>(i));
        scopes.push_back(
            {fold.training_begin(),
                fold.training_end(),
                fold.validation_begin(),
                fold.validation_end()});
    }
    std::vector<models::TimedRegressionPoint> points;
    points.reserve(source.holdout_start());
    // No final-holdout feature or label value enters the fitting/evaluation kernel.
    for (unsigned i = 0; i < source.holdout_start(); ++i) {
        if (i % 1024 == 0)
            check();
        const auto& row = evidence.observations(i);
        points.push_back(
            {row.has_value() ? std::optional(row.value()) : std::nullopt,
                row.has_forward_return() ? std::optional(row.forward_return()) : std::nullopt,
                row.has_label_end_index() ? std::optional<std::size_t>(row.label_end_index())
                                          : std::nullopt,
                row.evaluation_warmup()});
    }
    const auto& spec = recipe.specification();
    models::RegressionOptions options{
        spec.method() == proto::MODEL_LINEAR ? models::RegressionMethod::linear
                                             : models::RegressionMethod::ridge,
        spec.fit_intercept(),
        {}
    };
    if (spec.has_alpha())
        options.alpha = static_cast<double>(spec.alpha().units()) / 100000000.0;
    const auto fits = models::validate_regression_time(
        points,
        scopes,
        source.holdout_start(),
        options,
        spec.standardize(),
        specification.run_budget(),
        stop,
        [&](auto done, auto total) {
            check();
            if (progress)
                progress(done, total);
        },
        deadline);
    proto::ModelTimeResult result;
    *result.mutable_plan() = protocol::model_time_plan_object(plan);
    const proto::ModelTimeRow::Exclusion exclusions[] = {
        proto::ModelTimeRow::INCLUDED,
        proto::ModelTimeRow::WARMUP,
        proto::ModelTimeRow::FEATURE_UNAVAILABLE,
        proto::ModelTimeRow::LABEL_UNAVAILABLE,
        proto::ModelTimeRow::CROSSED_LABEL,
        proto::ModelTimeRow::NON_FINITE_PREDICTION
    };
    for (std::size_t i = 0; i < fits.size(); ++i) {
        check();
        const auto& fit = fits[i];
        auto& out = *result.add_folds();
        *out.mutable_scope() = specification.folds(static_cast<int>(i));
        for (const auto member : fit.training_members)
            out.add_training_members(static_cast<unsigned>(member));
        out.set_intercept(fit.fit.intercept);
        out.set_coefficient(fit.fit.coefficient);
        out.set_training_residual_sum_squares(fit.fit.residual_sum_squares);
        out.set_regularization(fit.fit.regularization);
        out.set_training_objective(fit.fit.objective);
        if (fit.standardization) {
            auto& scale = *out.mutable_standardization();
            scale.set_training_samples(static_cast<unsigned>(fit.standardization->samples));
            scale.set_mean(fit.standardization->mean);
            scale.set_sample_standard_deviation(fit.standardization->sample_standard_deviation);
        }
        for (const auto& row : fit.predictions) {
            auto& prediction = *out.add_predictions();
            prediction.set_event_index(static_cast<unsigned>(row.index));
            if (row.prediction)
                prediction.set_prediction(*row.prediction);
            prediction.set_exclusion(exclusions[static_cast<unsigned>(row.exclusion)]);
        }
        out.set_evaluated(static_cast<unsigned>(fit.evaluated));
        out.set_validation_residual_sum_squares(fit.residual_sum_squares);
        if (fit.mean_squared_error)
            out.set_validation_mean_squared_error(*fit.mean_squared_error);
    }
    check();
    result.set_computed_at_ms(clock.utc_now() / 1000000);
    protocol::validate_model_time_result(input, result);
    return result;
}

proto::ModelTrainingResult train_model(
    const proto::ModelTrainingInput& input,
    const std::filesystem::path& root,
    std::stop_token stop,
    const tasks::TaskProgress& progress,
    const Clock& clock)
{
    protocol::validate_model_training_input(input);
    const auto& plan = input.plan();
    const auto& source = plan.source();
    const auto& spec = plan.specification();
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(spec.time_budget_seconds());
    const auto check = [&] {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "model training cancelled");
        if (std::chrono::steady_clock::now() >= deadline)
            throw Error(ErrorCode::resource_exhausted, "model training time budget exhausted");
        if (clock.utc_now() / 1000000 >= spec.valid_until_ms())
            throw Error(ErrorCode::conflict, "model validity ended before training completion");
    };
    const auto total = protocol::model_training_work_units(input);
    const auto report = [&](std::size_t done) {
        check();
        if (progress)
            progress(done, total);
    };
    report(0);
    check();
    (void)programs::Files(root / "programs", plan.owner()).verify(plan.program_artifact(), stop);
    if (source.program_artifact() != plan.program_artifact()) {
        check();
        (void)programs::Files(root / "programs", plan.owner())
            .verify(source.program_artifact(), stop);
    }
    factor::proto::FactorInput original;
    factor::proto::FactorResult evidence;
    check();
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
        evidence);
    check();
    const auto members = model_training_indices(plan, original, evidence);
    check();
    std::vector<models::RegressionPoint> points;
    points.reserve(members.size());
    for (const auto index : members) {
        check();
        const auto& row = evidence.observations(static_cast<int>(index));
        points.push_back({row.value(), row.forward_return()});
    }
    std::optional<models::Standardization> standardized;
    std::size_t preprocessing_work = 0;
    if (spec.standardize()) {
        standardized = models::fit_standardization(
            points,
            stop,
            [&](auto done, auto) {
                report(done);
            },
            deadline);
        for (std::size_t i = 0; i < points.size(); ++i) {
            check();
            points[i].feature = models::standardize_feature(*standardized, points[i].feature);
            if ((i + 1) % 1024 == 0 || i + 1 == points.size())
                report(points.size() + i + 1);
        }
        preprocessing_work = points.size() * 2;
    }
    models::RegressionOptions options{
        spec.method() == proto::MODEL_LINEAR ? models::RegressionMethod::linear
                                             : models::RegressionMethod::ridge,
        spec.fit_intercept(),
        {}
    };
    if (spec.has_alpha())
        options.alpha = static_cast<double>(spec.alpha().units()) / 100000000.0;
    const auto fit = models::fit_regression(
        points,
        options,
        stop,
        [&](auto done, auto) {
            report(preprocessing_work + done);
        },
        deadline);
    proto::ModelTrainingResult result;
    *result.mutable_plan() = protocol::model_plan_object(plan);
    result.set_training_samples(static_cast<unsigned>(fit.samples));
    result.set_intercept(fit.intercept);
    result.set_coefficient(fit.coefficient);
    result.set_residual_sum_squares(fit.residual_sum_squares);
    result.set_regularization(fit.regularization);
    result.set_objective(fit.objective);
    if (standardized) {
        auto& fitted = *result.mutable_standardization();
        fitted.set_training_samples(static_cast<unsigned>(standardized->samples));
        fitted.set_mean(standardized->mean);
        fitted.set_sample_standard_deviation(standardized->sample_standard_deviation);
    }
    std::size_t fitted = 0;
    for (int i = 0; i < evidence.observations_size(); ++i) {
        check();
        const auto& source_row = evidence.observations(i);
        auto& row = *result.add_rows();
        row.set_event_index(source_row.event_index());
        const bool holdout = source_row.event_index() >= source.holdout_start();
        const bool training = source_row.event_index() >= spec.training_begin()
            && source_row.event_index() < spec.training_end();
        row.set_role(
            holdout        ? proto::ModelTrainingRow::HOLDOUT
                : training ? proto::ModelTrainingRow::TRAINING
                           : proto::ModelTrainingRow::OTHER_DEVELOPMENT);
        if (!training)
            row.set_reason(proto::ModelTrainingRow::NOT_TRAINING);
        else if (fitted < members.size() && members[fitted] == source_row.event_index()) {
            row.set_reason(proto::ModelTrainingRow::FITTED);
            ++fitted;
        } else if (source_row.evaluation_warmup())
            row.set_reason(proto::ModelTrainingRow::WARMUP);
        else if (!source_row.has_value())
            row.set_reason(proto::ModelTrainingRow::MISSING_FEATURE);
        else if (!source_row.has_forward_return() || !source_row.has_label_end_index())
            row.set_reason(proto::ModelTrainingRow::MISSING_LABEL);
        else if (source_row.label_end_index() >= spec.training_end())
            row.set_reason(proto::ModelTrainingRow::CROSSED_LABEL);
        else
            throw std::logic_error("qualified original row omitted from model training");
        if (!source_row.has_value())
            row.set_prediction_state(proto::ModelTrainingRow::FEATURE_UNAVAILABLE);
        else {
            try {
                const auto feature = standardized
                    ? models::standardize_feature(*standardized, source_row.value())
                    : source_row.value();
                row.set_prediction(models::predict_regression(fit, feature));
                row.set_prediction_state(proto::ModelTrainingRow::PREDICTED);
            } catch (const std::invalid_argument&) {
                // The complete fit and finite original feature are already verified.
                // An out-of-training overflow is an unavailable prediction, not zero.
                row.set_prediction_state(proto::ModelTrainingRow::NON_FINITE_PREDICTION);
            }
        }
        if ((i + 1) % 1024 == 0 || i + 1 == evidence.observations_size())
            report(preprocessing_work + points.size() * 2 + static_cast<std::size_t>(i) + 1);
    }
    result.set_fitted_at_ms(clock.utc_now() / 1000000);
    check();
    protocol::validate_model_training_result(input, result);
    check();
    return result;
}

proto::ModelMappingProjection project_model_mapping(
    const proto::ModelMapping& mapping,
    const task::proto::TaskResponse& original,
    std::uint32_t begin,
    std::uint32_t limit,
    const Clock& clock)
{
    protocol::validate_model_mapping(mapping);
    const bool time = mapping.folds_size() != 0;
    if (!original.has_result_task()
        || (time
                ? (!original.has_model_time() || !original.result_task().has_model_time())
                : (!original.has_model_training() || !original.result_task().has_model_training())))
        throw std::invalid_argument("prediction mapping requires original model input and result");
    const auto& task = original.result_task();
    const auto& selected = mapping.specification().model();
    if (time) {
        protocol::validate_model_time_result(task.model_time(), original.model_time());
        const auto& folds = task.model_time().plan().specification().folds();
        if (mapping.folds_size() != folds.size())
            throw Error(ErrorCode::conflict, "prediction mapping original fold scope mismatch");
        for (int i = 0; i < folds.size(); ++i)
            if (mapping.folds(i).SerializeAsString() != folds[i].SerializeAsString())
                throw Error(ErrorCode::conflict, "prediction mapping original fold scope mismatch");
    } else
        protocol::validate_model_training_result(task.model_training(), original.model_training());
    const auto bytes = time ? original.model_time().SerializeAsString()
                            : original.model_training().SerializeAsString();
    const auto plan = time ? protocol::model_time_plan_object(task.model_time().plan())
                           : protocol::model_plan_object(task.model_training().plan());
    const auto& recipe = time ? task.model_time().plan().recipe() : task.model_training().plan();
    if (task.kind() != (time ? task::proto::MODEL_TIME_VALIDATION : task::proto::MODEL_TRAINING)
        || task.state() != task::proto::SUCCEEDED || task.has_retirement()
        || task.id() != selected.id() || task.result_digest() != selected.version()
        || task.result_digest() != sha256_bytes(bytes)
        || plan.SerializeAsString() != mapping.plan().SerializeAsString()
        || recipe.owner().SerializeAsString() != mapping.owner().SerializeAsString()
        || recipe.source().SerializeAsString() != mapping.source().SerializeAsString())
        throw Error(ErrorCode::conflict, "diagnostic mapping original model identity changed");
    const auto total = protocol::model_mapping_rows(mapping);
    if (!limit || limit > 200 || begin >= total)
        throw std::invalid_argument("diagnostic prediction page outside original rows");
    proto::ModelMappingProjection result;
    *result.mutable_mapping() = mapping;
    result.set_total(total);
    result.set_begin(begin);
    result.set_observed_at_ms(clock.utc_now() / 1000000);
    const bool expired = result.observed_at_ms() >= mapping.specification().valid_until_ms();
    const models::ThresholdMapping thresholds{
        static_cast<double>(mapping.specification().short_threshold().units()) / 100000000.0,
        static_cast<double>(mapping.specification().long_threshold().units()) / 100000000.0
    };
    const auto end = std::min(total, begin + limit);
    for (auto i = begin; i < end; ++i) {
        auto& row = *result.add_rows();
        std::optional<double> prediction;
        if (time) {
            const auto [fold, index] = protocol::model_mapping_time_position(mapping, i);
            const auto& source = original.model_time().folds(fold).predictions(
                static_cast<int>(index - mapping.folds(fold).validation_begin()));
            auto& mapped = *row.mutable_time_observation();
            mapped.set_fold_index(fold);
            *mapped.mutable_observation() = source;
            if (source.has_prediction())
                prediction = source.prediction();
        } else {
            *row.mutable_observation() = original.model_training().rows(i);
            if (row.observation().has_prediction())
                prediction = row.observation().prediction();
        }
        if (expired) {
            row.set_intent(proto::ModelMappedRow::EXPIRED);
            continue;
        }
        const auto intent = models::map_prediction(thresholds, prediction);
        switch (intent) {
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
                row.set_intent(proto::ModelMappedRow::PREDICTION_UNAVAILABLE);
                break;
        }
    }
    if (end < total)
        result.set_next_begin(end);
    return result;
}

} // namespace asterion::research
