#include "model_regression.hpp"
#include "model_validation.hpp"
#include <asterion/foundation/error.hpp>
#include <gtest/gtest.h>
#include <limits>
#include <cmath>
#include <vector>

using namespace asterion;
using namespace asterion::models;

namespace {

const std::vector<RegressionPoint> straight{{1, 3}, {2, 5}, {3, 7}};

} // namespace

TEST(ModelRegression, LinearAndUnnormalizedRidgeKeepInterceptAndObjectiveMeaning)
{
    const auto linear = fit_regression(straight, {RegressionMethod::linear, true, {}});
    EXPECT_DOUBLE_EQ(linear.coefficient, 2);
    EXPECT_DOUBLE_EQ(linear.intercept, 1);
    EXPECT_DOUBLE_EQ(linear.residual_sum_squares, 0);
    EXPECT_DOUBLE_EQ(linear.regularization, 0);
    const auto ridge = fit_regression(straight, {RegressionMethod::ridge, true, 2});
    EXPECT_DOUBLE_EQ(ridge.coefficient, 1);
    EXPECT_DOUBLE_EQ(ridge.intercept, 3);
    EXPECT_DOUBLE_EQ(ridge.residual_sum_squares, 2);
    EXPECT_DOUBLE_EQ(ridge.regularization, 2);
    EXPECT_DOUBLE_EQ(ridge.objective, 4);
    EXPECT_DOUBLE_EQ(predict_regression(ridge, 5), 8);
    const auto through_zero = fit_regression(straight, {RegressionMethod::linear, false, {}});
    EXPECT_DOUBLE_EQ(through_zero.intercept, 0);
    EXPECT_DOUBLE_EQ(through_zero.coefficient, 17.0 / 7);
}

TEST(ModelRegression, PositiveRidgeSolvesConstantFeatureWithoutSilentlyRelaxingOlsRank)
{
    const std::vector<RegressionPoint> constant{{2, 1}, {2, 3}};
    EXPECT_THROW(
        fit_regression(constant, {RegressionMethod::linear, true, {}}),
        std::invalid_argument);
    EXPECT_THROW(
        fit_regression(constant, {RegressionMethod::ridge, true, 0}),
        std::invalid_argument);
    const auto ridge = fit_regression(constant, {RegressionMethod::ridge, true, 1});
    EXPECT_DOUBLE_EQ(ridge.coefficient, 0);
    EXPECT_DOUBLE_EQ(ridge.intercept, 2);
    EXPECT_DOUBLE_EQ(ridge.residual_sum_squares, 2);
    EXPECT_DOUBLE_EQ(ridge.objective, 2);
}

TEST(ModelRegression, NoMethodOrMissingValueFallbackAndNonfinitePredictionsAreRejected)
{
    EXPECT_THROW(
        fit_regression(straight, {RegressionMethod::linear, true, 0}),
        std::invalid_argument);
    EXPECT_THROW(
        fit_regression(straight, {RegressionMethod::ridge, true, {}}),
        std::invalid_argument);
    EXPECT_THROW(
        fit_regression(straight, {RegressionMethod::ridge, true, -1}),
        std::invalid_argument);
    EXPECT_THROW(
        fit_regression(std::span(straight).first(1), {RegressionMethod::linear, true, {}}),
        std::invalid_argument);
    auto invalid = straight;
    invalid[1].target = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW(
        fit_regression(invalid, {RegressionMethod::linear, true, {}}),
        std::invalid_argument);
    const auto model = fit_regression(straight, {RegressionMethod::linear, true, {}});
    EXPECT_THROW(
        predict_regression(model, std::numeric_limits<double>::infinity()),
        std::invalid_argument);
    EXPECT_THROW(
        predict_regression(model, std::numeric_limits<double>::max()),
        std::invalid_argument);
}

TEST(ModelRegression, CenteredFitPreservesSmallVariationAtLargeFeatureOffset)
{
    const std::vector<RegressionPoint> shifted{
        {1000000000001., 3},
        {1000000000002., 5},
        {1000000000003., 7}
    };
    const auto model = fit_regression(shifted, {RegressionMethod::linear, true, {}});
    EXPECT_DOUBLE_EQ(model.coefficient, 2);
    EXPECT_DOUBLE_EQ(model.intercept, -1999999999999.);
    EXPECT_DOUBLE_EQ(model.residual_sum_squares, 0);
    EXPECT_DOUBLE_EQ(predict_regression(model, 1000000000004.), 9);
}

TEST(ModelRegression, CancellationAndExpiredBudgetCannotReturnAPartialModel)
{
    std::stop_source stopped;
    std::vector<std::size_t> completed;
    try {
        (void)fit_regression(
            straight,
            {RegressionMethod::linear, true, {}},
            stopped.get_token(),
            [&](auto done, auto total) {
                EXPECT_EQ(total, 6);
                completed.push_back(done);
                if (done == 3)
                    stopped.request_stop();
            });
        FAIL() << "cancelled fit returned a model";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::cancelled);
    }
    EXPECT_EQ(completed, (std::vector<std::size_t>{0, 3}));
    try {
        (void)fit_regression(
            straight,
            {RegressionMethod::linear, true, {}},
            {},
            {},
            std::chrono::steady_clock::time_point::min());
        FAIL() << "exhausted budget returned a model";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::resource_exhausted);
    }
}

TEST(ModelRegression, StandardizationUsesOnlySelectedTrainingMembersAndSampleDenominator)
{
    const std::vector<RegressionPoint> training{{1, 10}, {3, 20}};
    const auto fitted = fit_standardization(training);
    EXPECT_EQ(fitted.samples, 2U);
    EXPECT_DOUBLE_EQ(fitted.mean, 2);
    EXPECT_DOUBLE_EQ(fitted.sample_standard_deviation, std::sqrt(2.0));
    EXPECT_DOUBLE_EQ(standardize_feature(fitted, 5), 3 / std::sqrt(2.0));
    EXPECT_DOUBLE_EQ(standardize_feature(fitted, 1000), 998 / std::sqrt(2.0));
    EXPECT_DOUBLE_EQ(fitted.mean, 2);
    EXPECT_DOUBLE_EQ(fitted.sample_standard_deviation, std::sqrt(2.0));
    EXPECT_THROW(fit_standardization(std::span(training).first(1)), std::invalid_argument);
    const std::vector<RegressionPoint> constant{{2, 0}, {2, 1}};
    EXPECT_THROW(fit_standardization(constant), std::invalid_argument);
    auto invalid = training;
    invalid[1].feature = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW(fit_standardization(invalid), std::invalid_argument);
    std::stop_source stop;
    stop.request_stop();
    EXPECT_THROW(fit_standardization(training, stop.get_token()), Error);
    EXPECT_THROW(
        fit_standardization(training, {}, {}, std::chrono::steady_clock::time_point::min()),
        Error);
}

TEST(ModelPredictionMapping, ExplicitThresholdsSeparateDirectionsNoNewIntentAndUnavailable)
{
    const ThresholdMapping mapping{0.4, 0.6};
    EXPECT_EQ(map_prediction(mapping, 0.7), PredictionIntent::long_direction);
    EXPECT_EQ(map_prediction(mapping, 0.6), PredictionIntent::long_direction);
    EXPECT_EQ(map_prediction(mapping, 0.5), PredictionIntent::no_new_direction);
    EXPECT_EQ(map_prediction(mapping, 0.4), PredictionIntent::short_direction);
    EXPECT_EQ(map_prediction(mapping, 0.0), PredictionIntent::short_direction);
    EXPECT_EQ(map_prediction(mapping, {}), PredictionIntent::unavailable);
    EXPECT_EQ(
        map_prediction(mapping, std::numeric_limits<double>::quiet_NaN()),
        PredictionIntent::unavailable);
    EXPECT_EQ(
        map_prediction(mapping, std::numeric_limits<double>::infinity()),
        PredictionIntent::unavailable);
    EXPECT_THROW(map_prediction({0.6, 0.6}, 0.7), std::invalid_argument);
    EXPECT_THROW(map_prediction({0.7, 0.6}, {}), std::invalid_argument);
}

namespace {

std::vector<TimedRegressionPoint> chronological_points()
{
    std::vector<TimedRegressionPoint> points;
    for (std::size_t i = 0; i < 20; ++i)
        points.push_back({static_cast<double>(i), 1 + 2.0 * i, i + 2, false});
    return points;
}

const std::vector<RegressionFold> development_folds{{0, 6, 6, 10}, {4, 10, 10, 14}};

} // namespace

TEST(ModelTimeValidation, EachFoldFitsOnlyMatureMembersAndPredictsLaterRowsWithoutLeakingHoldout)
{
    auto points = chronological_points();
    const auto original = validate_regression_time(
        points,
        development_folds,
        16,
        {RegressionMethod::linear, true, {}},
        true,
        2);
    ASSERT_EQ(original.size(), 2U);
    EXPECT_EQ(original[0].training_members, (std::vector<std::size_t>{0, 1, 2, 3}));
    EXPECT_EQ(original[1].training_members, (std::vector<std::size_t>{4, 5, 6, 7}));
    EXPECT_DOUBLE_EQ(original[0].standardization->mean, 1.5);
    EXPECT_NEAR(original[0].standardization->sample_standard_deviation, std::sqrt(5.0 / 3), 1e-14);
    EXPECT_EQ(original[0].evaluated, 2U);
    EXPECT_NEAR(*original[0].mean_squared_error, 0, 1e-25);
    EXPECT_NEAR(*original[0].predictions[0].prediction, 13, 1e-12);
    EXPECT_EQ(original[0].predictions[2].exclusion, ValidationExclusion::crossed_label);
    EXPECT_TRUE(original[0].predictions[2].prediction);
    for (std::size_t i = 6; i < 16; ++i) {
        points[i].feature = 1000.0 + i;
        points[i].target = 5000.0 - i;
    }
    const auto changed = validate_regression_time(
        points,
        development_folds,
        16,
        {RegressionMethod::linear, true, {}},
        true,
        2);
    EXPECT_DOUBLE_EQ(changed[0].fit.coefficient, original[0].fit.coefficient);
    EXPECT_DOUBLE_EQ(changed[0].fit.intercept, original[0].fit.intercept);
    EXPECT_DOUBLE_EQ(changed[0].standardization->mean, original[0].standardization->mean);
    EXPECT_DOUBLE_EQ(
        changed[0].standardization->sample_standard_deviation,
        original[0].standardization->sample_standard_deviation);
    points = chronological_points();
    for (std::size_t i = 16; i < 20; ++i) {
        points[i].feature = std::numeric_limits<double>::quiet_NaN();
        points[i].target = std::numeric_limits<double>::infinity();
        points[i].label_end.reset();
    }
    const auto held = validate_regression_time(
        points,
        development_folds,
        16,
        {RegressionMethod::linear, true, {}},
        true,
        2);
    EXPECT_DOUBLE_EQ(held[1].fit.coefficient, original[1].fit.coefficient);
    EXPECT_DOUBLE_EQ(*held[1].predictions[0].prediction, *original[1].predictions[0].prediction);
}

TEST(ModelTimeValidation, MissingFutureLabelsKeepPredictionsAndNeverBecomeZeroScores)
{
    auto points = chronological_points();
    points[6].feature.reset();
    points[7].target.reset();
    points[7].label_end.reset();
    const auto result = validate_regression_time(
        points,
        std::span(development_folds).first(1),
        16,
        {RegressionMethod::linear, true, {}},
        false,
        1);
    EXPECT_FALSE(result[0].predictions[0].prediction);
    EXPECT_EQ(result[0].predictions[0].exclusion, ValidationExclusion::feature_unavailable);
    EXPECT_TRUE(result[0].predictions[1].prediction);
    EXPECT_EQ(result[0].predictions[1].exclusion, ValidationExclusion::label_unavailable);
    EXPECT_EQ(result[0].evaluated, 0U);
    EXPECT_FALSE(result[0].mean_squared_error);
    points[6].feature = 0;
    const auto zero = validate_regression_time(
        points,
        std::span(development_folds).first(1),
        16,
        {RegressionMethod::linear, true, {}},
        false,
        1);
    EXPECT_NEAR(*zero[0].predictions[0].prediction, 1, 1e-12);
    EXPECT_EQ(zero[0].evaluated, 1U);
}

TEST(ModelTimeValidation, InvalidScopesBudgetsAndCancellationCannotSilentlyReduceTheDeclaredFolds)
{
    const auto points = chronological_points();
    const RegressionOptions options{RegressionMethod::linear, true, {}};
    EXPECT_THROW(
        validate_regression_time(points, development_folds, 16, options, false, 1),
        std::invalid_argument);
    auto overlap = development_folds;
    overlap[1].validation_begin = 9;
    EXPECT_THROW(
        validate_regression_time(points, overlap, 16, options, false, 2),
        std::invalid_argument);
    EXPECT_THROW(
        validate_regression_time(points, development_folds, 12, options, false, 2),
        std::invalid_argument);
    const std::vector<RegressionFold> immature{{0, 2, 2, 4}};
    EXPECT_THROW(
        validate_regression_time(points, immature, 16, options, false, 1),
        std::invalid_argument);
    std::stop_source stop;
    stop.request_stop();
    EXPECT_THROW(
        validate_regression_time(
            points,
            development_folds,
            16,
            options,
            false,
            2,
            stop.get_token()),
        Error);
    EXPECT_THROW(
        validate_regression_time(
            points,
            development_folds,
            16,
            options,
            false,
            2,
            {},
            {},
            std::chrono::steady_clock::time_point::min()),
        Error);
}
