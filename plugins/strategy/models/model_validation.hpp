#pragma once
#include "model_regression.hpp"
#include <vector>

namespace asterion::models {

struct TimedRegressionPoint
{
    std::optional<double> feature, target;
    std::optional<std::size_t> label_end;
    bool warmup = false;
};

struct RegressionFold
{
    std::size_t training_begin, training_end, validation_begin, validation_end;
};
enum class ValidationExclusion
{
    included,
    warmup,
    feature_unavailable,
    label_unavailable,
    crossed_label,
    non_finite_prediction
};

struct ValidationPrediction
{
    std::size_t index;
    std::optional<double> prediction;
    ValidationExclusion exclusion;
};

struct RegressionFoldResult
{
    RegressionFold scope;
    std::vector<std::size_t> training_members;
    RegressionFit fit;
    std::optional<Standardization> standardization;
    std::vector<ValidationPrediction> predictions;
    std::size_t evaluated = 0;
    double residual_sum_squares = 0;
    std::optional<double> mean_squared_error;
};

// Explicit chronological development folds. The owning domain retains historical
// availability limitations; this routine cannot certify real historical availability.
std::vector<RegressionFoldResult> validate_regression_time(
    std::span<const TimedRegressionPoint>,
    std::span<const RegressionFold>,
    std::size_t development_end,
    const RegressionOptions&,
    bool standardize,
    std::size_t run_budget,
    std::stop_token = {},
    const std::function<void(std::size_t, std::size_t)>& progress = {},
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max());

} // namespace asterion::models
