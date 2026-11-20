#include <asterion/futures/portfolio_targets.hpp>
#include <gtest/gtest.h>
#include <limits>

using namespace asterion;

namespace {

Decimal d(const char* value)
{
    return Decimal::parse(value);
}

WeightedContract
member(const char* weight, const char* price = "3000.01", const char* symbol = "rb2610")
{
    return {{"SHFE", symbol}, d(weight), d(price), d("10")};
}

PortfolioTargets size(std::initializer_list<WeightedContract> members)
{
    return fixed_weight_targets(d("100000"), members, d("1"), d("1"));
}

} // namespace

TEST(PortfolioTargets, SizesSingleContractLongAndShortWithUnallocatedRemainder)
{
    for (const auto* weight : {"0.5", "-0.5"}) {
        const auto result = size({member(weight)});
        ASSERT_EQ(result.members.size(), 1);
        const auto& target = result.members[0];
        EXPECT_EQ(target.quantity, weight[0] == '-' ? d("-1") : d("1"));
        EXPECT_EQ(target.requested_notional, d("100000") * d(weight));
        EXPECT_EQ(target.allocated_notional + target.nominal_remainder, target.requested_notional);
        EXPECT_EQ(result.unallocated_nominal_budget, d("50000"));
    }
}

TEST(PortfolioTargets, DoesNotRoundAFractionalLotUpAtEitherSign)
{
    const auto result = size({member("0.3"), member("-0.3", "3000.01", "cu2610")});
    ASSERT_EQ(result.members.size(), 2);
    EXPECT_EQ(result.members[0].quantity, Decimal{});
    EXPECT_EQ(result.members[1].quantity, Decimal{});
    EXPECT_EQ(result.allocated_gross_notional, Decimal{});
    EXPECT_EQ(result.requested_gross_notional, d("60000"));
    EXPECT_EQ(result.requested_net_notional, Decimal{});
}

TEST(PortfolioTargets, SizesEightPlaceWeightsOfEquityWithCents)
{
    // 999996.75 x 0.33333333 = 333332.2499...: more than eight places.
    const WeightedContract third = member("0.33333333", "3000");
    const auto result = fixed_weight_targets(d("999996.75"), {&third, 1}, d("1"), d("1"));
    EXPECT_EQ(result.members[0].requested_notional, d("333332.24666667"));
    EXPECT_EQ(result.members[0].quantity, d("11"));
}

TEST(PortfolioTargets, ExplicitZeroWeightProducesAFlatTarget)
{
    const auto result = size({member("0"), member("-0.6", "3000", "cu2610")});
    EXPECT_EQ(result.members[0].quantity, Decimal{});
    EXPECT_EQ(result.members[1].quantity, d("-2"));
    EXPECT_EQ(result.members[1].nominal_remainder, Decimal{});
}

TEST(PortfolioTargets, RefusesExcessGrossOrNetBudgetRatherThanRescaling)
{
    EXPECT_THROW(size({member("0.6"), member("-0.5", "3000", "cu2610")}), std::invalid_argument);
    const auto contract = member("-0.6");
    EXPECT_THROW(
        fixed_weight_targets(d("100000"), {&contract, 1}, d("1"), d("0.5")),
        std::invalid_argument);
}

TEST(PortfolioTargets, RequiresExplicitUsableInputs)
{
    EXPECT_THROW(size({}), std::invalid_argument);
    EXPECT_THROW(size({member("0.1"), member("0.2")}), std::invalid_argument);
    EXPECT_THROW(size({member("0.5", "0")}), std::invalid_argument);
    EXPECT_THROW(size({member("0.5", "-1")}), std::invalid_argument);
    auto contract = member("0.5");
    EXPECT_THROW(
        fixed_weight_targets(Decimal{}, {&contract, 1}, d("1"), d("1")),
        std::invalid_argument);
    contract.multiplier = Decimal{};
    EXPECT_THROW(
        fixed_weight_targets(d("100000"), {&contract, 1}, d("1"), d("1")),
        std::invalid_argument);
}

TEST(PortfolioTargets, RejectsOverflow)
{
    auto contract = member("1", "92233720368");
    EXPECT_THROW(
        fixed_weight_targets(d("100000"), {&contract, 1}, d("1"), d("1")),
        std::overflow_error);
    contract.weight = Decimal::from_raw(std::numeric_limits<std::int64_t>::min());
    EXPECT_THROW(
        fixed_weight_targets(d("100000"), {&contract, 1}, d("1"), d("1")),
        std::overflow_error);
}

TEST(PortfolioTargets, ScalesWeightsUniformlyToRespectMaxMarginRatio)
{
    // Without margin cap: 1,000,000 * 1.5 / (3000 * 10) = 50 lots each (margin = 50*30000*0.12 = 180,000 each, 360,000 total = 36%).
    // With max_margin_ratio = 0.18: implied margin 2 * (1.5 * 0.12) = 0.36 -> scale = 0.5 -> 25 lots each (margin = 90,000 each = 180,000 total <= 18%).
    const WeightedContract a{{"SHFE", "rb2701"}, d("1.5"), d("3000"), d("10"), d("0.12"), d("0")};
    const WeightedContract b{{"DCE", "i2701"}, d("-1.5"), d("3000"), d("10"), d("0.12"), d("0")};
    const WeightedContract contracts[] = {a, b};
    const auto result =
        fixed_weight_targets(d("1000000"), contracts, d("4"), d("1"), {}, d("0.18"), d("0"));
    ASSERT_EQ(result.members.size(), 2U);
    EXPECT_EQ(result.members[0].quantity, d("25"));
    EXPECT_EQ(result.members[1].quantity, d("-25"));
}

TEST(PortfolioTargets, MinRebalanceWeightSuppressesRoundingChurnExceptFlatAndSignFlip)
{
    // Held: 3 lots (notional 90,000 = 9% of 1,000,000).
    // Requested weight 0.119 -> 119,000 (truncates to 3 lots anyway); requested weight 0.121 -> 121,000 (4 lots), drift = 31,000 (3.1%).
    // With min_rebalance_weight = 0.04 (40,000), drift 31,000 < 40,000 keeps 3 lots; flat (0) and sign flip (-0.01) execute immediately.
    const WeightedContract
        small_drift{{"SHFE", "rb2701"}, d("0.121"), d("3000"), d("10"), d("0.10"), d("3")};
    const WeightedContract
        flat_exit{{"SHFE", "cu2612"}, d("0"), d("3000"), d("10"), d("0.10"), d("1")};
    const WeightedContract
        sign_flip{{"DCE", "m2701"}, d("-0.035"), d("3000"), d("10"), d("0.10"), d("1")};
    const WeightedContract contracts[] = {small_drift, flat_exit, sign_flip};
    const auto result =
        fixed_weight_targets(d("1000000"), contracts, d("2"), d("2"), {}, d("0"), d("0.04"));
    ASSERT_EQ(result.members.size(), 3U);
    EXPECT_EQ(result.members[0].quantity, d("3"));
    EXPECT_EQ(result.members[1].quantity, d("0"));
    EXPECT_EQ(result.members[2].quantity, d("-1"));
}
