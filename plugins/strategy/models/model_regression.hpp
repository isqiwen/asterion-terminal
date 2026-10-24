#pragma once
#include <chrono>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>

namespace asterion::models {

enum class RegressionMethod
{
    linear,
    ridge
};

struct RegressionOptions
{
    RegressionMethod method;
    bool fit_intercept;
    std::optional<double> alpha;
};

// The owning Research task has already selected exact eligible training rows.
// No time split, missing-value fill or feature selection is hidden in this plugin.
struct RegressionPoint
{
    double feature, target;
};

struct Standardization
{
    std::size_t samples;
    double mean, sample_standard_deviation;
};

Standardization fit_standardization(
    std::span<const RegressionPoint>,
    std::stop_token = {},
    const std::function<void(std::size_t, std::size_t)>& progress = {},
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max());
double standardize_feature(const Standardization&, double feature);

struct RegressionFit
{
    std::size_t samples;
    double intercept, coefficient;
    double residual_sum_squares, regularization, objective;
};

RegressionFit fit_regression(
    std::span<const RegressionPoint>,
    const RegressionOptions&,
    std::stop_token = {},
    const std::function<void(std::size_t, std::size_t)>& progress = {},
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max());
double predict_regression(const RegressionFit&, double feature);

// Direction intent only: no_new_direction never means a flat position target.
enum class PredictionIntent
{
    long_direction,
    short_direction,
    no_new_direction,
    unavailable
};

struct ThresholdMapping
{
    double short_threshold, long_threshold;
};

PredictionIntent map_prediction(const ThresholdMapping&, std::optional<double> prediction);

} // namespace asterion::models
