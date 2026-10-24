#include "model_regression.hpp"
#include <asterion/foundation/error.hpp>
#include <cmath>
#include <stdexcept>

namespace asterion::models {
namespace {

double finite(long double value)
{
    const auto result = static_cast<double>(value);
    if (!std::isfinite(result))
        throw std::invalid_argument("regression result is not finite");
    return result;
}

} // namespace

Standardization fit_standardization(
    std::span<const RegressionPoint> points,
    std::stop_token stop,
    const std::function<void(std::size_t, std::size_t)>& progress,
    std::chrono::steady_clock::time_point deadline)
{
    if (points.size() < 2)
        throw std::invalid_argument("training standardization requires at least two samples");
    const auto check = [&] {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "model standardization cancelled");
        if (std::chrono::steady_clock::now() >= deadline)
            throw Error(
                ErrorCode::resource_exhausted,
                "model standardization time budget exhausted");
    };
    long double mean = 0, squared = 0;
    for (std::size_t i = 0; i < points.size(); ++i) {
        check();
        const long double feature = points[i].feature;
        if (!std::isfinite(feature))
            throw std::invalid_argument("training standardization requires finite features");
        const auto delta = feature - mean;
        mean += delta / (i + 1);
        squared += delta * (feature - mean);
        if (progress && ((i + 1) % 1024 == 0 || i + 1 == points.size()))
            progress(i + 1, points.size());
    }
    check();
    const auto scale = finite(std::sqrt(squared / (points.size() - 1)));
    if (scale <= 0)
        throw std::invalid_argument("training standardization requires a positive sample scale");
    return {points.size(), finite(mean), scale};
}

double standardize_feature(const Standardization& fitted, double feature)
{
    if (fitted.samples < 2 || !std::isfinite(fitted.mean)
        || !std::isfinite(fitted.sample_standard_deviation) || fitted.sample_standard_deviation <= 0
        || !std::isfinite(feature))
        throw std::invalid_argument("standardization requires fixed finite training parameters");
    return finite(
        (static_cast<long double>(feature) - fitted.mean) / fitted.sample_standard_deviation);
}

double predict_regression(const RegressionFit& model, double feature)
{
    if (model.samples < 2 || !std::isfinite(model.intercept) || !std::isfinite(model.coefficient)
        || !std::isfinite(feature))
        throw std::invalid_argument(
            "regression prediction requires a complete finite model and feature");
    return finite(std::fma(model.coefficient, feature, model.intercept));
}

RegressionFit fit_regression(
    std::span<const RegressionPoint> points,
    const RegressionOptions& options,
    std::stop_token stop,
    const std::function<void(std::size_t, std::size_t)>& progress,
    std::chrono::steady_clock::time_point deadline)
{
    if (points.size() < 2)
        throw std::invalid_argument("regression requires at least two valid training samples");
    if ((options.method != RegressionMethod::linear && options.method != RegressionMethod::ridge)
        || (options.method == RegressionMethod::linear && options.alpha)
        || (options.method == RegressionMethod::ridge && !options.alpha)
        || (options.alpha && (!std::isfinite(*options.alpha) || *options.alpha < 0)))
        throw std::invalid_argument(
            "regression method and explicit nonnegative alpha do not match");
    const auto check = [&] {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "model training cancelled");
        if (std::chrono::steady_clock::now() >= deadline)
            throw Error(ErrorCode::resource_exhausted, "model training time budget exhausted");
    };
    const auto report = [&](std::size_t done) {
        check();
        if (progress)
            progress(done, points.size() * 2);
    };
    report(0);
    long double mean_x = 0, mean_y = 0, xx = 0, xy = 0;
    for (std::size_t i = 0; i < points.size(); ++i) {
        check();
        const long double x = points[i].feature, y = points[i].target;
        if (!std::isfinite(x) || !std::isfinite(y))
            throw std::invalid_argument("regression training sample is not finite");
        if (options.fit_intercept) {
            const auto dx = x - mean_x, dy = y - mean_y;
            mean_x += dx / (i + 1);
            mean_y += dy / (i + 1);
            xx += dx * (x - mean_x);
            xy += dx * (y - mean_y);
        } else {
            xx += x * x;
            xy += x * y;
        }
        if (!std::isfinite(xx) || !std::isfinite(xy) || xx < 0)
            throw std::invalid_argument("regression training moments are not finite");
        if ((i + 1) % 1024 == 0)
            report(i + 1);
    }
    report(points.size());
    const long double alpha = options.alpha.value_or(0);
    if (!alpha && !xx)
        throw std::invalid_argument("linear model design is rank deficient");
    RegressionFit result{points.size(), 0, finite(xy / (xx + alpha)), 0, 0, 0};
    if (options.fit_intercept)
        result.intercept = finite(mean_y - result.coefficient * mean_x);
    long double squared = 0;
    for (std::size_t i = 0; i < points.size(); ++i) {
        check();
        const long double residual =
            points[i].target - predict_regression(result, points[i].feature);
        squared += residual * residual;
        if ((i + 1) % 1024 == 0)
            report(points.size() + i + 1);
    }
    result.residual_sum_squares = finite(squared);
    result.regularization = finite(alpha * result.coefficient * result.coefficient);
    result.objective = finite(result.residual_sum_squares + result.regularization);
    report(points.size() * 2);
    return result;
}

PredictionIntent map_prediction(const ThresholdMapping& mapping, std::optional<double> prediction)
{
    if (!std::isfinite(mapping.short_threshold) || !std::isfinite(mapping.long_threshold)
        || mapping.short_threshold >= mapping.long_threshold)
        throw std::invalid_argument("prediction short threshold must be less than long threshold");
    if (!prediction || !std::isfinite(*prediction))
        return PredictionIntent::unavailable;
    if (*prediction >= mapping.long_threshold)
        return PredictionIntent::long_direction;
    if (*prediction <= mapping.short_threshold)
        return PredictionIntent::short_direction;
    return PredictionIntent::no_new_direction;
}

} // namespace asterion::models
