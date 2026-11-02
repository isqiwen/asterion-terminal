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
