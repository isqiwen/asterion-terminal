#include "feature_expression.hpp"
#include <asterion/base/error.hpp>
#include <cmath>
#include <gtest/gtest.h>
using namespace asterion;

namespace {

std::vector<Decimal> prices(std::initializer_list<int> values)
{
    std::vector<Decimal> result;
    for (const auto value : values)
        result.push_back(Decimal::from_raw(static_cast<std::int64_t>(value) * 100000000));
    return result;
}

} // namespace

TEST(FeatureExpressions, ActualDeclaredMomentumHasOriginalUnitsWarmupAndKnownValues)
{
    const FeatureExpression expression("close / lag(close, window) - 1");
    const auto values = expression.evaluate(prices({100, 102, 101, 103}), 2);
    EXPECT_EQ(expression.price_power(), 0);
    EXPECT_EQ(expression.history(2), 2U);
    ASSERT_EQ(values.size(), 4U);
    EXPECT_FALSE(values[0].value);
    EXPECT_EQ(values[0].reason, FeatureUndefined::warmup);
    EXPECT_FALSE(values[1].value);
    EXPECT_EQ(values[1].reason, FeatureUndefined::warmup);
    ASSERT_TRUE(values[2].value);
    ASSERT_TRUE(values[3].value);
    EXPECT_NEAR(*values[2].value, 0.01, 1e-12);
    EXPECT_NEAR(*values[3].value, 103.0 / 102.0 - 1, 1e-12);
    EXPECT_EQ(values[2].reason, FeatureUndefined::none);
}

TEST(FeatureExpressions, NestedHistoryAndFutureSuffixCannotLeakIntoEarlierFeatures)
{
    const FeatureExpression expression("mean(return(close, 2), 3)");
    const auto original = expression.evaluate(prices({100, 102, 101, 103, 104, 105, 106}), 2);
    const auto changed = expression.evaluate(prices({100, 102, 101, 103, 104, 900, 1}), 2);
    EXPECT_EQ(expression.history(2), 4U);
    for (std::size_t i = 0; i < 5; ++i) {
        EXPECT_EQ(original[i].value, changed[i].value);
        EXPECT_EQ(original[i].reason, changed[i].reason);
    }
    EXPECT_FALSE(original[3].value);
    ASSERT_TRUE(original[4].value);
    EXPECT_NEAR(
        *original[4].value,
        ((101.0 / 100 - 1) + (103.0 / 102 - 1) + (104.0 / 101 - 1)) / 3,
        1e-12);
}

TEST(FeatureExpressions, RealZeroUndefinedDenominatorAndOverflowRemainDifferent)
{
    const auto output = FeatureExpression("return(close,1)").evaluate(prices({0, 0, 2, 2}), 2);
    EXPECT_EQ(output[0].reason, FeatureUndefined::warmup);
    EXPECT_EQ(output[1].reason, FeatureUndefined::zero_denominator);
    EXPECT_EQ(output[2].reason, FeatureUndefined::zero_denominator);
    ASSERT_TRUE(output[3].value);
    EXPECT_EQ(*output[3].value, 0);
    EXPECT_EQ(output[3].reason, FeatureUndefined::none);
    const auto window =
        FeatureExpression("mean(close / lag(close,1),2)").evaluate(prices({0, 0, 2, 2, 2}), 2);
    EXPECT_FALSE(window[3].value);
    EXPECT_EQ(window[3].reason, FeatureUndefined::zero_denominator);
    ASSERT_TRUE(window[4].value);
    EXPECT_EQ(*window[4].value, 1);
    const auto huge = FeatureExpression("1e308 * 1e308").evaluate(prices({1}), 2);
    EXPECT_FALSE(huge[0].value);
    EXPECT_EQ(huge[0].reason, FeatureUndefined::non_finite);
    const auto early =
        FeatureExpression("(1/0) + return(close,window)").evaluate(prices({100, 101, 102}), 2);
    EXPECT_EQ(early[0].reason, FeatureUndefined::warmup);
    EXPECT_EQ(early[1].reason, FeatureUndefined::warmup);
    EXPECT_EQ(early[2].reason, FeatureUndefined::zero_denominator);
}

TEST(FeatureExpressions, NumericConstantsAcceptFractionsAndExponents)
{
    const auto output = FeatureExpression("0.5 + .25 + 2.5e-1 + 1E1").evaluate(prices({1}), 2);
    ASSERT_TRUE(output[0].value);
    EXPECT_EQ(*output[0].value, 11);
    EXPECT_THROW(FeatureExpression("1e"), std::exception);
    EXPECT_THROW(FeatureExpression("."), std::exception);
}

TEST(FeatureExpressions, RollingMeanSampleDeviationAndExtremesKeepPriceUnits)
{
    const auto input = prices({100, 102, 101, 103});
    const FeatureExpression average("mean(close,window)");
    EXPECT_EQ(average.price_power(), 1);
    EXPECT_EQ(average.history(2), 1U);
    const auto means = average.evaluate(input, 2);
    ASSERT_TRUE(means[1].value);
    EXPECT_EQ(*means[1].value, 101);
    EXPECT_EQ(*means[2].value, 101.5);
    EXPECT_EQ(*means[3].value, 102);
    const auto deviations = FeatureExpression("std(close,2)").evaluate(input, 2);
    EXPECT_NEAR(*deviations[1].value, std::sqrt(2.0), 1e-12);
    EXPECT_NEAR(*deviations[2].value, std::sqrt(0.5), 1e-12);
    EXPECT_NEAR(*deviations[3].value, std::sqrt(2.0), 1e-12);
    const auto lows = FeatureExpression("min(close,2)").evaluate(input, 2);
    const auto highs = FeatureExpression("max(close,2)").evaluate(input, 2);
    EXPECT_EQ(*lows[1].value, 100);
    EXPECT_EQ(*lows[2].value, 101);
    EXPECT_EQ(*lows[3].value, 101);
    EXPECT_EQ(*highs[1].value, 102);
    EXPECT_EQ(*highs[2].value, 102);
    EXPECT_EQ(*highs[3].value, 103);
}

TEST(FeatureExpressions, InvalidUnitsFutureFieldsAndUnboundedSyntaxGiveSourceLocations)
{
    try {
        (void)FeatureExpression("close + 1");
        FAIL();
    } catch (const FeatureExpressionError& error) {
        EXPECT_EQ(error.position(), 6U);
        EXPECT_EQ(error.reason(), "addition and subtraction require equal units");
    }
    try {
        (void)FeatureExpression("forward_return");
        FAIL();
    } catch (const FeatureExpressionError& error) {
        EXPECT_EQ(error.position(), 0U);
        EXPECT_EQ(error.reason(), "unknown field or function");
    }
    EXPECT_THROW(FeatureExpression("lag(close,-1)"), FeatureExpressionError);
    EXPECT_THROW(FeatureExpression("std(close,1)"), FeatureExpressionError);
    EXPECT_THROW(FeatureExpression("load(close,2)"), FeatureExpressionError);
    EXPECT_THROW(
        FeatureExpression(std::string(40, '(') + "close" + std::string(40, ')')),
        FeatureExpressionError);
    EXPECT_THROW(FeatureExpression(std::string(8193, ' ')), FeatureExpressionError);
    const FeatureExpression expression("std(close,window)");
    EXPECT_THROW(expression.history(1), FeatureExpressionError);
    EXPECT_THROW(expression.history(0), FeatureExpressionError);
}

TEST(FeatureExpressions, CooperativeStopIsObservedBeforePublishingAnyValues)
{
    std::stop_source stop;
    stop.request_stop();
    const FeatureExpression expression("return(close,2)");
    try {
        (void)expression.evaluate(prices({100, 101, 102}), 2, stop.get_token());
        FAIL();
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::cancelled);
    }
}

TEST(FeatureExpressions, LongWindowsHaveExactNestedWarmupAndConstantOutput)
{
    const FeatureExpression expression("std(mean(close,window),window)");
    std::vector<Decimal> input(200000, Decimal::parse("100"));
    EXPECT_EQ(expression.history(10000), 19998U);
    const auto result = expression.evaluate(input, 10000);
    EXPECT_FALSE(result[19997].value);
    ASSERT_TRUE(result[19998].value);
    EXPECT_EQ(*result[19998].value, 0);
    ASSERT_TRUE(result.back().value);
    EXPECT_EQ(*result.back().value, 0);
}
