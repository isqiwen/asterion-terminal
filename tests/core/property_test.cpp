#include <asterion/base/decimal.hpp>
#include <asterion/futures/order_limits.hpp>
#include <asterion/futures/position_target.hpp>
#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <algorithm>
#include <cstdint>

namespace {

using namespace asterion;

HeldPosition
apply_batch(HeldPosition held, const std::vector<TargetOrder>& orders, ClosePolicy policy)
{
    Decimal today = held.today;
    Decimal yesterday = held.yesterday;
    Side side = held.side;
    for (const auto& order : orders) {
        if (order.offset == Offset::open) {
            if (today == Decimal{} && yesterday == Decimal{})
                side = order.side;
            today = today + order.quantity;
        } else if (order.offset == Offset::close_yesterday) {
            yesterday = yesterday - order.quantity;
        } else if (order.offset == Offset::close_today) {
            today = today - order.quantity;
        } else if (policy == ClosePolicy::today_first) {
            const auto from_today = std::min(today, order.quantity);
            today = today - from_today;
            yesterday = yesterday - (order.quantity - from_today);
        } else {
            const auto from_yesterday = std::min(yesterday, order.quantity);
            yesterday = yesterday - from_yesterday;
            today = today - (order.quantity - from_yesterday);
        }
    }
    if (today == Decimal{} && yesterday == Decimal{})
        return {};
    return {side, today, yesterday};
}

Decimal signed_position(const HeldPosition& held)
{
    const auto total = held.today + held.yesterday;
    return held.side == Side::buy ? total : Decimal{} - total;
}

TEST(Property, decimal_roundtrip_and_rounding_order)
{
    ASSERT_TRUE(
        rc::check("canonical text roundtrips and quantization preserves monotonic bounds", [] {
            const auto raw = *rc::gen::arbitrary<std::int64_t>();
            const auto value = Decimal::from_raw(raw);
            RC_ASSERT(Decimal::parse(value.str()) == value);

            const auto left = Decimal::from_raw(
                *rc::gen::inRange<std::int64_t>(-1'000'000'000'000'000LL, 1'000'000'000'000'000LL));
            const auto right = Decimal::from_raw(
                *rc::gen::inRange<std::int64_t>(-1'000'000'000'000'000LL, 1'000'000'000'000'000LL));
            RC_ASSERT((left + right) - right == left);

            const auto bounded = Decimal::from_raw(
                *rc::gen::inRange<std::int64_t>(-100'000'000'000'000LL, 100'000'000'000'000LL));
            const auto step =
                Decimal::from_raw(*rc::gen::inRange<std::int64_t>(1, 10'000'000'000LL));
            const auto low = quantize(bounded, step, Rounding::floor);
            const auto zero = quantize(bounded, step, Rounding::toward_zero);
            const auto even = quantize(bounded, step, Rounding::half_even);
            const auto up = quantize(bounded, step, Rounding::half_up);
            const auto high = quantize(bounded, step, Rounding::ceiling);
            RC_ASSERT(low.multiple_of(step));
            RC_ASSERT(zero.multiple_of(step));
            RC_ASSERT(even.multiple_of(step));
            RC_ASSERT(up.multiple_of(step));
            RC_ASSERT(high.multiple_of(step));
            RC_ASSERT(low <= zero && zero <= high);
            RC_ASSERT(low <= even && even <= high);
            RC_ASSERT(low <= up && up <= high);
            RC_ASSERT(quantize(Decimal{} - bounded, step, Rounding::floor) == Decimal{} - high);
        }));
}

TEST(Property, target_orders_close_yesterday_first_and_reach_target_without_netting)
{
    ASSERT_TRUE(
        rc::check(
            "target_orders never opens while closing and reaches target in at most two steps",
            [] {
                const auto side = *rc::gen::element(Side::buy, Side::sell);
                const auto today_lots = *rc::gen::inRange(0, 40);
                const auto yesterday_lots = *rc::gen::inRange(0, 40);
                const auto target_lots = *rc::gen::inRange(-40, 41);
                const auto policy = *rc::gen::element(
                    ClosePolicy::explicit_buckets,
                    ClosePolicy::today_first,
                    ClosePolicy::yesterday_first);

                const HeldPosition initial{
                    side,
                    Decimal::from_raw(today_lots * 100'000'000LL),
                    Decimal::from_raw(yesterday_lots * 100'000'000LL)
                };
                const auto target = Decimal::from_raw(target_lots * 100'000'000LL);

                const auto first = target_orders(target, initial, policy);
                bool has_open = false;
                bool has_close = false;
                Decimal closed{};
                for (const auto& order : first) {
                    RC_ASSERT(order.quantity > Decimal{});
                    if (order.offset == Offset::open)
                        has_open = true;
                    else {
                        has_close = true;
                        closed = closed + order.quantity;
                    }
                }
                RC_ASSERT(!(has_open && has_close));
                if (has_close && policy == ClosePolicy::explicit_buckets
                    && initial.yesterday > Decimal{}) {
                    RC_ASSERT(first.front().offset == Offset::close_yesterday);
                    RC_ASSERT(first.front().quantity == std::min(closed, initial.yesterday));
                }

                const auto mid = apply_batch(initial, first, policy);
                const auto second = target_orders(target, mid, policy);
                const auto final_held = apply_batch(mid, second, policy);
                RC_ASSERT(signed_position(final_held) == target);
                RC_ASSERT(target_orders(target, final_held, policy).empty());
            }));
}

TEST(Property, order_limits_never_block_valid_closes_and_cap_gross_exposure)
{
    ASSERT_TRUE(
        rc::check(
            "OrderLimits exempts valid closes from position and working caps while capping opens",
            [] {
                const auto max_order = *rc::gen::inRange(1, 20);
                const auto max_contract = *rc::gen::inRange(1, 50);
                const auto max_gross = *rc::gen::inRange(max_contract, 80);
                const auto max_working = *rc::gen::inRange<std::size_t>(1, 10);

                const auto held = *rc::gen::inRange(0, 100);
                const auto pending = *rc::gen::inRange(0, 50);
                const auto extra_account_held = *rc::gen::inRange(0, 40);
                const auto extra_account_pending = *rc::gen::inRange(0, 20);
                const auto working = *rc::gen::inRange<std::size_t>(0, 15);
                const auto lots = *rc::gen::inRange(1, 25);
                const auto offset = *rc::gen::element(
                    Offset::open,
                    Offset::close,
                    Offset::close_today,
                    Offset::close_yesterday);
                const auto side = *rc::gen::element(Side::buy, Side::sell);

                const auto qty = [](int n) {
                    return Decimal::from_raw(static_cast<std::int64_t>(n) * 100'000'000LL);
                };
                OrderLimits risk({qty(max_order), qty(max_gross), max_working, qty(max_contract)});

                const Instrument spec{{"SHFE", "rb2610"}, "CNY", qty(1), qty(1), qty(10)};
                const LimitOrder order{"prop", spec.id, side, qty(lots), qty(3000)};
                const PreTradeRiskContext context{
                    spec,
                    order,
                    offset,
                    qty(held),
                    qty(pending),
                    working,
                    qty(held + extra_account_held),
                    qty(pending + extra_account_pending)
                };
                const auto decision = risk.evaluate(context);

                if (lots > max_order) {
                    RC_ASSERT(decision.reason == RiskReason::order_quantity);
                } else if (offset != Offset::open) {
                    RC_ASSERT(decision.allowed());
                } else {
                    const bool within_working = working < max_working;
                    const bool within_contract = held + pending + lots <= max_contract;
                    const bool within_account =
                        held + extra_account_held + pending + extra_account_pending + lots
                        <= max_gross;
                    RC_ASSERT(
                        decision.allowed()
                        == (within_working && within_contract && within_account));
                }
            }));
}

} // namespace
