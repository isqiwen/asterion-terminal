#include "account_exposure.hpp"
#include <asterion/foundation/error.hpp>
#include <gtest/gtest.h>
#include <array>
using namespace asterion;

namespace {

Decimal d(const char* value)
{
    return Decimal::parse(value);
}

} // namespace

TEST(AccountExposure, NetZeroKeepsObservedGrossBothSidesAndActualMarginAmount)
{
    const std::array positions{ExposurePosition{d("100"), d("10"), d("0"), d("4"), d("200")}};
    const auto result = account_exposure(d("1000"), d("200"), positions);
    EXPECT_EQ(result.covered_net_notional, d("0"));
    EXPECT_EQ(result.covered_gross_notional, d("4000"));
    EXPECT_EQ(result.positions[0].long_quantity, d("2"));
    EXPECT_EQ(result.positions[0].short_quantity, d("2"));
    EXPECT_DOUBLE_EQ(*result.gross_to_equity.value, 4);
    EXPECT_DOUBLE_EQ(*result.margin_to_equity.value, 0.2);
    EXPECT_DOUBLE_EQ(*result.positions[0].covered_gross_share.value, 1);
}

TEST(AccountExposure, MissingGrossAndMarginStayUnknownEvenWhenObservedNetIsZero)
{
    const std::array positions{
        ExposurePosition{d("100"), d("10"), d("0"), {}, {}},
        ExposurePosition{d("200"), d("10"), d("1"), d("1"), d("100")}
    };
    const auto result = account_exposure(d("1000"), {}, positions);
    EXPECT_EQ(result.covered_net_notional, d("2000"));
    EXPECT_FALSE(result.covered_gross_notional);
    EXPECT_FALSE(result.positions[0].long_quantity);
    EXPECT_FALSE(result.positions[0].short_quantity);
    EXPECT_EQ(result.gross_to_equity.unavailable, ExposureUnavailable::gross_not_recorded);
    EXPECT_EQ(result.margin_to_equity.unavailable, ExposureUnavailable::margin_not_recorded);
    EXPECT_FALSE(result.positions[1].covered_gross_share.value);
    EXPECT_EQ(result.positions[1].gross_notional, d("2000"));
}

TEST(AccountExposure, ListedOppositeContractsKeepGrossAndDoNotImplyCrossContractMarginOffsets)
{
    const std::array positions{
        ExposurePosition{d("100"), d("10"), d("2"), d("2"), d("200")},
        ExposurePosition{d("100"), d("10"), d("-2"), d("2"), d("300")}
    };
    const auto result = account_exposure(d("1000"), d("500"), positions);
    EXPECT_EQ(result.covered_net_notional, d("0"));
    EXPECT_EQ(result.covered_gross_notional, d("4000"));
    EXPECT_DOUBLE_EQ(*result.margin_to_equity.value, 0.5);
    EXPECT_DOUBLE_EQ(*result.positions[0].covered_gross_share.value, 0.5);
    EXPECT_DOUBLE_EQ(*result.positions[1].covered_gross_share.value, 0.5);
    EXPECT_EQ(result.positions[1].short_quantity, d("2"));
}

TEST(
    AccountExposure,
    LegalNonpositivePricesAndNonpositiveEquityDoNotBecomeMissingMoneyOrDefinedRatios)
{
    const std::array positions{
        ExposurePosition{d("-10"), d("10"), d("2"), d("2"), d("100")},
        ExposurePosition{d("0"), d("10"), d("-1"), d("1"), d("20")}
    };
    const auto result = account_exposure(d("-1"), d("120"), positions);
    EXPECT_EQ(result.covered_net_notional, d("-200"));
    EXPECT_EQ(result.covered_gross_notional, d("200"));
    EXPECT_EQ(result.positions[0].long_quantity, d("2"));
    EXPECT_FALSE(result.net_to_equity.value);
    EXPECT_EQ(result.net_to_equity.unavailable, ExposureUnavailable::non_positive_equity);
    const std::array flat{positions[1]};
    const auto zero = account_exposure(d("1000"), d("20"), flat);
    EXPECT_DOUBLE_EQ(*zero.gross_to_equity.value, 0);
    EXPECT_FALSE(zero.positions[0].covered_gross_share.value);
    EXPECT_EQ(
        zero.positions[0].covered_gross_share.unavailable,
        ExposureUnavailable::zero_gross_notional);
}

TEST(AccountExposure, InconsistentOriginalLotsAndCancellationCannotProduceACompletedSlice)
{
    const std::array invalid{ExposurePosition{d("100"), d("10"), d("3"), d("2"), d("0")}};
    EXPECT_THROW(account_exposure(d("1000"), {}, invalid), std::invalid_argument);
    EXPECT_THROW(account_exposure(d("1000"), {}, {}), std::invalid_argument);
    std::stop_source stop;
    stop.request_stop();
    const std::array original{ExposurePosition{d("100"), d("10"), d("1"), d("1"), d("100")}};
    EXPECT_THROW(account_exposure(d("1000"), d("100"), original, stop.get_token()), Error);
}
