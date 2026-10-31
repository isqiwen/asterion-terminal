#include "static_stress.hpp"
#include <asterion/base/error.hpp>
#include <gtest/gtest.h>
using namespace asterion;

namespace {

Decimal d(const char* value)
{
    return Decimal::parse(value);
}

} // namespace

TEST(StaticStress, ExplicitRequirementCaseSeparatesLossMarginAndFundingGap)
{
    const ExposurePosition position{d("100"), d("10"), d("2"), d("2"), d("200")};
    const auto result = static_stress(d("1000"), d("1000"), d("0"), position, d("80"), d("0.4"));
    EXPECT_EQ(result.mark_change, d("-400"));
    EXPECT_EQ(result.equity, d("600"));
    EXPECT_EQ(result.margin, d("640"));
    EXPECT_EQ(result.available, d("-40"));
    EXPECT_EQ(result.gap, d("40"));
}

TEST(StaticStress, HedgedNetZeroKeepsGrossMarginAndOriginalFrozenAndBalanceLimit)
{
    const ExposurePosition position{d("100"), d("10"), d("0"), d("4"), d("200")};
    const auto result = static_stress(d("800"), d("1000"), d("50"), position, d("80"), d("0.4"));
    EXPECT_EQ(result.mark_change, d("0"));
    EXPECT_EQ(result.equity, d("1000"));
    EXPECT_EQ(result.margin, d("1280"));
    EXPECT_EQ(result.available, d("-530"));
    EXPECT_EQ(result.gap, d("530"));
}

TEST(StaticStress, LegalNegativePriceAndEquityAreAmountsNotMissingOrForcedLiquidation)
{
    const ExposurePosition position{d("10"), d("10"), d("2"), d("2"), d("20")};
    const auto result = static_stress(d("100"), d("100"), d("0"), position, d("-10"), d("1.5"));
    EXPECT_EQ(result.mark_change, d("-400"));
    EXPECT_EQ(result.equity, d("-300"));
    EXPECT_EQ(result.margin, d("300"));
    EXPECT_EQ(result.available, d("-600"));
    EXPECT_EQ(result.gap, d("600"));
}

TEST(StaticStress, MissingGrossInvalidRateAndCancellationDoNotCreateAResult)
{
    ExposurePosition position{d("100"), d("10"), d("0"), {}, {}};
    EXPECT_THROW(
        static_stress(d("1000"), d("1000"), d("0"), position, d("80"), d("0.4")),
        std::invalid_argument);
    position.gross_quantity = d("4");
    EXPECT_THROW(
        static_stress(d("1000"), d("1000"), d("0"), position, d("80"), d("-0.4")),
        std::invalid_argument);
    std::stop_source stop;
    stop.request_stop();
    EXPECT_THROW(
        static_stress(d("1000"), d("1000"), d("0"), position, d("80"), d("0.4"), stop.get_token()),
        Error);
}
