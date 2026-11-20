#include <asterion/futures/order_limits.hpp>
#include <gtest/gtest.h>
#include <asterion/system/process/child.hpp>
using namespace asterion;

namespace {

Decimal d(const char* text)
{
    return Decimal::parse(text);
}

Instrument instrument()
{
    return {{"SHFE", "rb2610"}, "CNY", d("1"), d("1"), d("10")};
}

} // namespace

TEST(OrderLimits, exact_bounds)
{
    auto spec = instrument();
    LimitOrder order{"test", spec.id, Side::buy, d("2"), d("100")};
    PreTradeRiskContext context{spec, order, Offset::open, d("4"), d("4"), 1};
    OrderLimits risk({d("2"), d("10"), 2, d("10")});
    EXPECT_TRUE(risk.evaluate(context).allowed()); // Exactly 10 total and 2 working.
    context.pending_open_quantity = d("5");
    EXPECT_EQ(risk.evaluate(context).reason, RiskReason::gross_quantity);
    context.pending_open_quantity = d("4");
    context.working_orders = 2;
    EXPECT_EQ(risk.evaluate(context).reason, RiskReason::working_orders);
    context.working_orders = 1;
    order.quantity = d("3");
    EXPECT_EQ(risk.evaluate(context).reason, RiskReason::order_quantity);
}

TEST(OrderLimits, gross_risk_does_not_net_sides_or_credit_pending_closes)
{
    auto spec = instrument();
    LimitOrder sell{"test", spec.id, Side::sell, d("1"), d("100")};
    PreTradeRiskContext context{spec, sell, Offset::open, d("10"), d("0"), 0};
    OrderLimits risk({d("3"), d("10"), 5, d("10")});
    EXPECT_EQ(risk.evaluate(context).reason, RiskReason::gross_quantity);
    context.offset = Offset::close_today;
    EXPECT_TRUE(risk.evaluate(context).allowed());
    context.gross_position_quantity = d("20");
    context.working_orders = 5;
    EXPECT_TRUE(risk.evaluate(context)
            .allowed()); // Permit reducing over-limit risk even when working orders are full.
    context.offset = Offset::open;
    EXPECT_EQ(risk.evaluate(context).reason, RiskReason::working_orders);
    context.working_orders = 0;
    EXPECT_EQ(risk.evaluate(context).reason, RiskReason::gross_quantity);
}

TEST(OrderLimits, orthogonal_contract_and_account_gross_limits_across_contracts)
{
    auto rb = instrument();
    Instrument hc{{"SHFE", "hc2610"}, "CNY", d("1"), d("1"), d("10")};
    FuturesAccount account(
        d("100000"),
        {{rb, {d("100"), d("0"), d("0"), d("0")}}, {hc, {d("100"), d("0"), d("0"), d("0")}}});
    account.mark(rb.id, d("100"));
    account.mark(hc.id, d("100"));
    OrderLimits risk({d("5"), d("8"), 2, d("5")});
    LimitOrder first{"rb.1", rb.id, Side::buy, d("5"), d("100")};
    ASSERT_TRUE(assess_order(risk, account, first, Offset::open).allowed());
    account.submit(first, Offset::open);
    account.fill({"fill.rb", "rb.1", d("5"), d("100")});
    LimitOrder rb_more{"rb.2", rb.id, Side::buy, d("1"), d("100")};
    EXPECT_EQ(
        assess_order(risk, account, rb_more, Offset::open).reason,
        RiskReason::gross_quantity);
    LimitOrder hc_over{"hc.over", hc.id, Side::buy, d("4"), d("100")};
    EXPECT_EQ(
        assess_order(risk, account, hc_over, Offset::open).reason,
        RiskReason::gross_quantity);
    LimitOrder hc_fit{"hc.fit", hc.id, Side::buy, d("3"), d("100")};
    ASSERT_TRUE(assess_order(risk, account, hc_fit, Offset::open).allowed());
    account.submit(hc_fit, Offset::open);
    LimitOrder second_working{"rb.close.1", rb.id, Side::sell, d("2"), d("100")};
    account.submit(second_working, Offset::close_today);
    ASSERT_EQ(account.working_orders().size(), 2U);
    LimitOrder close_at_full{"rb.close.2", rb.id, Side::sell, d("2"), d("100")};
    EXPECT_TRUE(assess_order(risk, account, close_at_full, Offset::close_today).allowed());
}

TEST(OrderLimits, rejects_bad_configuration_and_context)
{
    EXPECT_THROW((OrderLimits({d("0"), d("10"), 1, d("10")})), std::invalid_argument);
    EXPECT_THROW((OrderLimits({d("1"), d("-1"), 1, d("-1")})), std::invalid_argument);
    EXPECT_THROW((OrderLimits({d("1"), d("10"), 0, d("10")})), std::invalid_argument);
    EXPECT_THROW((OrderLimits({d("1"), d("10"), 1, d("11")})), std::invalid_argument);
    auto spec = instrument();
    LimitOrder order{"test", spec.id, Side::buy, d("1"), d("100")};
    PreTradeRiskContext context{spec, order, Offset::open, d("-1"), d("0"), 0};
    OrderLimits risk({d("1"), d("10"), 1, d("10")});
    EXPECT_EQ(risk.evaluate(context).reason, RiskReason::invalid_context);
    context.gross_position_quantity = d("0");
    context.pending_open_quantity = d("0.5");
    EXPECT_EQ(risk.evaluate(context).reason, RiskReason::invalid_context);
    context.pending_open_quantity = d("0");
    context.offset = static_cast<Offset>(50);
    EXPECT_EQ(risk.evaluate(context).reason, RiskReason::invalid_context);
    context.offset = Offset::open;
    order.quantity = d("0");
    EXPECT_THROW(risk.evaluate(context), std::invalid_argument);
}

TEST(OrderLimits, exposure_check_does_not_overflow_before_rejecting)
{
    auto spec = instrument();
    LimitOrder order{"test", spec.id, Side::buy, d("1"), d("100")};
    const auto huge = d("92233720368");
    PreTradeRiskContext context{spec, order, Offset::open, huge, huge, 0};
    OrderLimits risk({d("1"), huge, 1, huge});
    EXPECT_EQ(risk.evaluate(context).reason, RiskReason::gross_quantity);
}

TEST(OrderLimits, derives_exposure_from_real_ledger_through_partial_fill_and_cancel)
{
    auto spec = instrument();
    FuturesAccount account(d("10000"), {{spec, {d("100"), d("0"), d("0"), d("0")}}});
    account.mark(spec.id, d("100"));
    OrderLimits risk({d("5"), d("5"), 3, d("5")});
    LimitOrder first{"first", spec.id, Side::buy, d("4"), d("100")};
    ASSERT_TRUE(assess_order(risk, account, first, Offset::open).allowed());
    account.submit(first, Offset::open);
    LimitOrder next{"next", spec.id, Side::sell, d("2"), d("100")};
    const auto before = account.snapshot();
    EXPECT_EQ(assess_order(risk, account, next, Offset::open).reason, RiskReason::gross_quantity);
    EXPECT_EQ(account.snapshot(), before);
    account.fill({"fill.1", "first", d("2"), d("100")});
    // A partial fill moves pending exposure to positions, without freeing quota.
    EXPECT_EQ(assess_order(risk, account, next, Offset::open).reason, RiskReason::gross_quantity);
    account.cancel("first");
    EXPECT_TRUE(assess_order(risk, account, next, Offset::open).allowed());
    account.submit(next, Offset::open);
    account.fill({"fill.2", "next", d("2"), d("100")});
    // Long 2 + short 2 is gross 4, never net zero.
    LimitOrder third{"third", spec.id, Side::buy, d("2"), d("100")};
    EXPECT_EQ(assess_order(risk, account, third, Offset::open).reason, RiskReason::gross_quantity);
    LimitOrder close{"close", spec.id, Side::sell, d("2"), d("100")};
    account.submit(close, Offset::close_today);
    EXPECT_EQ(assess_order(risk, account, third, Offset::open).reason, RiskReason::gross_quantity);
    account.fill({"fill.3", "close", d("2"), d("100")});
    EXPECT_TRUE(assess_order(risk, account, third, Offset::open).allowed());
}

TEST(OrderLimits, configuration_roundtrips_without_defaults_or_rewriting)
{
    const OrderLimitsConfig original{d("3"), d("10"), 7, d("4")};
    const auto encoded = encode_order_limits(original);
    const auto decoded = decode_order_limits(encoded);
    EXPECT_EQ(decoded.max_order_quantity, original.max_order_quantity);
    EXPECT_EQ(decoded.max_gross_quantity, original.max_gross_quantity);
    EXPECT_EQ(decoded.max_working_orders, original.max_working_orders);
    EXPECT_EQ(decoded.max_contract_quantity, original.max_contract_quantity);
    EXPECT_EQ(encode_order_limits(decoded), encoded);
    for (const auto* key :
        {"max_order_quantity",
            "max_gross_quantity",
            "max_contract_quantity",
            "max_working_orders"}) {
        auto missing = encoded;
        missing.erase(key);
        EXPECT_THROW(decode_order_limits(missing), std::exception);
    }
    auto invalid = encoded;
    invalid["extra"] = true;
    EXPECT_THROW(decode_order_limits(invalid), std::exception);
    invalid = encoded;
    invalid["max_working_orders"] = -1;
    EXPECT_THROW(decode_order_limits(invalid), std::exception);
    invalid = encoded;
    invalid["max_working_orders"] = 1.5;
    EXPECT_THROW(decode_order_limits(invalid), std::exception);
    invalid = encoded;
    invalid["max_order_quantity"] = "3.0";
    EXPECT_THROW(decode_order_limits(invalid), std::exception);
    invalid = encoded;
    invalid["max_gross_quantity"] = "0";
    EXPECT_THROW(decode_order_limits(invalid), std::exception);
}

TEST(OrderLimits, capital_and_cancel_limits_reject_opens_and_exempt_closes)
{
    const OrderLimitsConfig config{d("5"), d("20"), 4, d("10"), d("5000"), d("12000"), d("0.7"), 3};
    const auto roundtripped = decode_order_limits(encode_order_limits(config));
    EXPECT_EQ(roundtripped.max_daily_loss, d("5000"));
    EXPECT_EQ(roundtripped.max_drawdown, d("12000"));
    EXPECT_EQ(roundtripped.max_margin_ratio, d("0.7"));
    EXPECT_EQ(roundtripped.max_daily_cancels_per_contract, 3U);

    OrderLimits risk(roundtripped);
    const auto spec = instrument();
    LimitOrder order{"test", spec.id, Side::buy, d("2"), d("3500")};
    PreTradeRiskContext base{
        spec,
        order,
        Offset::open,
        d("2"),
        d("0"),
        0,
        d("2"),
        d("0"),
        d("4999.99"),
        d("11999.99"),
        d("100000"),
        d("70000"),
        2
    };
    EXPECT_TRUE(risk.evaluate(base).allowed());

    {
        auto ctx = base;
        ctx.daily_loss = d("5000");
        EXPECT_EQ(risk.evaluate(ctx).reason, RiskReason::daily_loss);
    }
    {
        auto ctx = base;
        ctx.drawdown = d("12000");
        EXPECT_EQ(risk.evaluate(ctx).reason, RiskReason::drawdown);
    }
    {
        auto ctx = base;
        ctx.account_margin = d("70000.01");
        EXPECT_EQ(risk.evaluate(ctx).reason, RiskReason::margin_ratio);
    }
    {
        auto ctx = base;
        ctx.account_equity = d("0");
        EXPECT_EQ(risk.evaluate(ctx).reason, RiskReason::margin_ratio);
    }
    {
        auto ctx = base;
        ctx.contract_daily_cancels = 3;
        EXPECT_EQ(risk.evaluate(ctx).reason, RiskReason::daily_cancels);
    }

    // Simultaneous breaches of all capital and cancel caps never block valid reductions.
    PreTradeRiskContext closing = base;
    closing.daily_loss = d("50000");
    closing.drawdown = d("80000");
    closing.account_equity = d("0");
    closing.account_margin = d("90000");
    closing.contract_daily_cancels = 99;
    for (const auto off : {Offset::close, Offset::close_today, Offset::close_yesterday}) {
        closing.offset = off;
        EXPECT_TRUE(risk.evaluate(closing).allowed());
    }
}

TEST(OrderLimits, risk_exposure_aggregates_contract_account_unreconciled_and_working_orders)
{
    const auto rb = instrument();
    auto hc = instrument();
    hc.id.symbol = "hc2610";

    RiskExposure exposure;
    exposure.add_unreconciled_open(d("2"));
    exposure.add_position(rb.id, rb.id, d("3"));
    exposure.add_position(rb.id, hc.id, d("4"));
    exposure.add_working_order(rb.id, rb.id, Offset::open, d("2"));
    exposure.add_working_order(rb.id, rb.id, Offset::close_yesterday, d("1"));
    exposure.add_working_order(rb.id, hc.id, Offset::open, d("3"));

    LimitOrder order{"test", rb.id, Side::buy, d("1"), d("3500")};
    const auto context = exposure.context(rb, order, Offset::open);
    EXPECT_EQ(context.gross_position_quantity, d("3"));
    EXPECT_EQ(context.pending_open_quantity, d("2"));
    EXPECT_EQ(context.working_orders, 3U);
    EXPECT_EQ(context.account_gross_quantity, d("9"));
    EXPECT_EQ(context.account_pending_open_quantity, d("5"));
}
