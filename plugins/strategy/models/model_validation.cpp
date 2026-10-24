#include "model_validation.hpp"
#include <asterion/foundation/error.hpp>
#include <cmath>

namespace asterion::models {

std::vector<RegressionFoldResult> validate_regression_time(
    std::span<const TimedRegressionPoint> points,
    std::span<const RegressionFold> folds,
    std::size_t development_end,
    const RegressionOptions& options,
    bool standardize,
    std::size_t run_budget,
    std::stop_token stop,
    const std::function<void(std::size_t, std::size_t)>& progress,
    std::chrono::steady_clock::time_point deadline)
{
    if (points.empty() || points.size() > 200000 || !development_end
        || development_end > points.size() || folds.empty() || folds.size() > 32
        || run_budget < folds.size())
        throw std::invalid_argument("explicit model folds exceed source scope or run budget");
    std::size_t previous_end = 0;
    for (const auto& fold : folds) {
        if (fold.training_begin >= fold.training_end || fold.training_end > fold.validation_begin
            || fold.validation_begin >= fold.validation_end || fold.validation_end > development_end
            || fold.validation_begin < previous_end)
            throw std::invalid_argument(
                "model training and validation folds must be chronological and nonoverlapping");
        previous_end = fold.validation_end;
    }
    const auto check = [&] {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "model time validation cancelled");
        if (std::chrono::steady_clock::now() >= deadline)
            throw Error(ErrorCode::resource_exhausted, "model time validation budget exhausted");
    };
    const auto label = [&](const TimedRegressionPoint& point, std::size_t index) {
        if (point.target.has_value() != point.label_end.has_value()
            || (point.label_end && *point.label_end <= index))
            throw std::invalid_argument(
                "model validation label requires its actual future end index");
    };
    std::vector<RegressionFoldResult> results;
    results.reserve(folds.size());
    check();
    if (progress)
        progress(0, folds.size());
    for (const auto& fold : folds) {
        check();
        RegressionFoldResult result{};
        result.scope = fold;
        std::vector<RegressionPoint> training;
        training.reserve(fold.training_end - fold.training_begin);
        for (auto index = fold.training_begin; index < fold.training_end; ++index) {
            if (index % 1024 == 0)
                check();
            const auto& point = points[index];
            label(point, index);
            if (point.warmup || !point.feature || !point.target
                || *point.label_end >= fold.training_end)
                continue;
            training.push_back({*point.feature, *point.target});
            result.training_members.push_back(index);
        }
        if (standardize) {
            result.standardization = fit_standardization(training, stop, {}, deadline);
            for (auto& point : training) {
                check();
                point.feature = standardize_feature(*result.standardization, point.feature);
            }
        }
        result.fit = fit_regression(training, options, stop, {}, deadline);
        long double squared = 0;
        result.predictions.reserve(fold.validation_end - fold.validation_begin);
        for (auto index = fold.validation_begin; index < fold.validation_end; ++index) {
            if (index % 1024 == 0)
                check();
            const auto& point = points[index];
            label(point, index);
            ValidationPrediction prediction{index, {}, ValidationExclusion::feature_unavailable};
            if (point.feature) {
                if (!std::isfinite(*point.feature))
                    throw std::invalid_argument(
                        "model validation feature must be finite or explicitly missing");
                try {
                    const auto value = standardize
                        ? standardize_feature(*result.standardization, *point.feature)
                        : *point.feature;
                    prediction.prediction = predict_regression(result.fit, value);
                } catch (const std::invalid_argument&) {
                    // Valid finite source inputs may overflow this model's numeric prediction.
                    prediction.exclusion = ValidationExclusion::non_finite_prediction;
                }
            }
            if (prediction.prediction) {
                if (point.warmup)
                    prediction.exclusion = ValidationExclusion::warmup;
                else if (!point.target)
                    prediction.exclusion = ValidationExclusion::label_unavailable;
                else if (*point.label_end >= fold.validation_end)
                    prediction.exclusion = ValidationExclusion::crossed_label;
                else {
                    if (!std::isfinite(*point.target))
                        throw std::invalid_argument(
                            "model validation target must be finite or explicitly missing");
                    const long double error = *point.target - *prediction.prediction;
                    squared += error * error;
                    ++result.evaluated;
                    prediction.exclusion = ValidationExclusion::included;
                }
            }
            result.predictions.push_back(std::move(prediction));
        }
        result.residual_sum_squares = static_cast<double>(squared);
        if (!std::isfinite(result.residual_sum_squares))
            throw std::invalid_argument("model validation residual sum is not finite");
        if (result.evaluated)
            result.mean_squared_error = result.residual_sum_squares / result.evaluated;
        check();
        results.push_back(std::move(result));
        if (progress)
            progress(results.size(), folds.size());
    }
    check();
    return results;
}

} // namespace asterion::models
