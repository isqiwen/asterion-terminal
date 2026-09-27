#include <gtest/gtest.h>

#include <asterion/domain/order.hpp>
#include <asterion/kernel/event_bus.hpp>

#include <iostream>
#include <limits>
#include <vector>

using namespace asterion;
namespace {
Decimal d(const char *value) { return Decimal::parse(value); }
Instrument instrument() {
  return {{"TEST", "TEST-202612"},
          AssetClass::futures,
          "CNY",
          d("0.2"),
          d("1"),
          d("10")};
}
TEST(Domain, decimal_contract) {
  for (const char *value :
       {"0", "0.00000001", "-0.00000001", "12.3456", "-12.3456",
        "92233720368.54775807", "-92233720368.54775808"}) {
    EXPECT_TRUE((Decimal::parse(value).str() == value))
        << "decimal must roundtrip exactly";
  }
  EXPECT_TRUE((d("1.25000000").str() == "1.25")) << "decimal formatting";
  for (const char *value : {"", "-", "+1", " 1", "1 ", "1e3", "1.", ".1", "01",
                            "1.000000000", "nan", "1.2.3"}) {
    EXPECT_THROW(([&] { Decimal::parse(value); })(), std::invalid_argument);
  }
  for (const char *value : {"92233720368.54775808", "-92233720368.54775809",
                            "999999999999999999999999999"}) {
    EXPECT_THROW(([&] { Decimal::parse(value); })(), std::overflow_error);
  }
  EXPECT_TRUE((d("0.1") + d("0.2") == d("0.3"))) << "exact addition";
  EXPECT_TRUE((d("-0.1") - d("0.2") == d("-0.3"))) << "exact subtraction";
  EXPECT_TRUE((d("123.45") * d("10") == d("1234.5"))) << "exact notional";
  EXPECT_TRUE((d("0.0001") * d("0.0001") == d("0.00000001")))
      << "small exact product";
  EXPECT_TRUE((d("-1.25") * d("-0.8") == d("1"))) << "signed multiplication";
  const auto maximum =
      Decimal::from_raw(std::numeric_limits<std::int64_t>::max());
  const auto minimum =
      Decimal::from_raw(std::numeric_limits<std::int64_t>::min());
  const auto quantum = Decimal::from_raw(1);
  EXPECT_TRUE((maximum * d("1") == maximum))
      << "large multiplication without intermediate overflow";
  EXPECT_TRUE((minimum * d("1") == minimum)) << "minimum multiplication";
  EXPECT_TRUE((minimum - minimum == Decimal{}))
      << "minimum subtraction without negation overflow";
  EXPECT_TRUE((minimum + maximum == Decimal::from_raw(-1)))
      << "extreme addition";
  EXPECT_THROW(([&] { static_cast<void>(maximum + quantum); })(),
               std::overflow_error);
  EXPECT_THROW(([&] { static_cast<void>(minimum - quantum); })(),
               std::overflow_error);
  EXPECT_THROW(([&] { static_cast<void>(maximum - minimum); })(),
               std::overflow_error);
  EXPECT_THROW(([&] { static_cast<void>(minimum * d("-1")); })(),
               std::overflow_error);
  EXPECT_THROW(([&] { static_cast<void>(maximum * d("2")); })(),
               std::overflow_error);
  EXPECT_THROW(([&] { static_cast<void>(quantum * quantum); })(),
               std::domain_error);
  EXPECT_TRUE((d("1.2").multiple_of(d("0.2")))) << "valid tick grid";
  EXPECT_TRUE((!d("1.3").multiple_of(d("0.2")))) << "invalid tick grid";
  EXPECT_THROW(([&] { static_cast<void>(d("1").multiple_of(Decimal{})); })(),
               std::invalid_argument);
  // Independent integer oracle: cents times cents yields exactly four places.
  for (std::int64_t a = -100; a <= 100; ++a) {
    for (std::int64_t b = -100; b <= 100; ++b) {
      EXPECT_TRUE(
          (Decimal::from_raw(a * 1000000) * Decimal::from_raw(b * 1000000) ==
           Decimal::from_raw(a * b * 10000)))
          << "decimal product integer oracle";
    }
  }
}
TEST(Domain, market_contract) {
  auto spec = instrument();
  spec.validate();
  TradeTick{spec.id, 0, d("-0.2"), d("1")}.validate(spec);
  EXPECT_THROW(
      ([&] { TradeTick{spec.id, -1, d("1"), d("1")}.validate(spec); })(),
      std::invalid_argument);
  EXPECT_THROW(
      ([&] { TradeTick{spec.id, 1, d("1.1"), d("1")}.validate(spec); })(),
      std::invalid_argument);
  EXPECT_THROW(
      ([&] {
        TradeTick{{"OTHER", spec.id.symbol}, 1, d("1"), d("1")}.validate(spec);
      })(),
      std::invalid_argument);
  EXPECT_THROW(
      ([&] { TradeTick{spec.id, 1, d("1"), d("0.5")}.validate(spec); })(),
      std::invalid_argument);
  spec.price_increment = Decimal{};
  EXPECT_THROW(([&] { spec.validate(); })(), std::invalid_argument);
}
TEST(Domain, order_contract) {
  const auto spec = instrument();
  const LimitOrder request{"order-1", spec.id, Side::buy, d("3"), d("10")};
  Order order(request, spec);
  const Fill first{"fill-1", request.id, d("1"), d("9.8")};
  EXPECT_THROW(([&] { order.apply(first); })(), std::logic_error);
  order.accept();
  EXPECT_TRUE((order.apply(first))) << "first fill applied";
  EXPECT_TRUE((!order.apply(first))) << "exact duplicate ignored";
  EXPECT_TRUE((order.state() == OrderState::partially_filled &&
               order.remaining_quantity() == d("2")))
      << "partial fill state";
  EXPECT_THROW(
      ([&] { order.apply({"fill-1", request.id, d("2"), d("9.8")}); })(),
      std::invalid_argument);
  EXPECT_THROW(
      ([&] { order.apply({"fill-2", request.id, d("3"), d("9.8")}); })(),
      std::invalid_argument);
  EXPECT_THROW(
      ([&] { order.apply({"fill-2", request.id, d("1"), d("10.2")}); })(),
      std::invalid_argument);
  EXPECT_TRUE((order.filled_quantity() == d("1")))
      << "bad reports cannot mutate state";
  EXPECT_TRUE((order.apply({"fill-2", request.id, d("2"), d("10")})))
      << "final fill";
  EXPECT_TRUE((order.state() == OrderState::filled &&
               order.remaining_quantity() == Decimal{}))
      << "fully filled";
  EXPECT_TRUE((!order.apply(first)))
      << "duplicate still ignored after terminal state";
  EXPECT_THROW(([&] { order.cancel(); })(), std::logic_error);
  EXPECT_THROW(
      ([&] { order.apply({"fill-3", request.id, d("1"), d("10")}); })(),
      std::logic_error);
  Order cancelled(request, spec);
  cancelled.accept();
  cancelled.apply(first);
  cancelled.cancel();
  EXPECT_TRUE((cancelled.remaining_quantity() == d("2")))
      << "cancel retains executed quantity";
  EXPECT_THROW(
      ([&] { cancelled.apply({"fill-3", request.id, d("1"), d("10")}); })(),
      std::logic_error);
  Order rejected(request, spec);
  rejected.reject();
  EXPECT_THROW(([&] { rejected.accept(); })(), std::logic_error);
  auto sell = request;
  sell.side = Side::sell;
  Order sell_order(sell, spec);
  sell_order.accept();
  EXPECT_THROW(([&] { sell_order.apply(first); })(), std::invalid_argument);
}
TEST(Domain, event_contract) {
  EventBus<int> bus;
  std::vector<int> seen;
  const auto first = bus.subscribe([&](int event) { seen.push_back(event); });
  bus.subscribe([&, count = 0](int) mutable { seen.push_back(++count); });
  bus.publish(10);
  bus.unsubscribe(first);
  bus.publish(20);
  EXPECT_TRUE((seen == std::vector<int>{10, 1, 2}))
      << "ordered handlers retain mutable state";
  EventBus<int> errors;
  bool delivered = false;
  const auto bad = errors.subscribe(
      [](int) { throw std::runtime_error("subscriber error"); });
  errors.subscribe([&](int) { delivered = true; });
  EXPECT_THROW(([&] { errors.publish(0); })(), std::runtime_error);
  EXPECT_TRUE((delivered)) << "other subscribers still receive event";
  errors.unsubscribe(bad);
  errors.publish(1);
  EventBus<int> nested;
  nested.subscribe([&](int value) { nested.publish(value + 1); });
  EXPECT_THROW(([&] { nested.publish(0); })(), std::logic_error);
  EventBus<int> snapshot;
  int calls = 0;
  EventBus<int>::Subscription second = 0;
  snapshot.subscribe([&](int) { snapshot.unsubscribe(second); });
  second = snapshot.subscribe([&](int) { ++calls; });
  snapshot.publish(0);
  snapshot.publish(0);
  EXPECT_TRUE((calls == 1))
      << "unsubscribe during publish takes effect next event";
}
} // namespace

#include <asterion/domain/trading_schedule.hpp>
TEST(TradingSchedule, ExplicitLabelsAndHalfOpenOrderedSessions) {
  asterion::TradingDaySchedule schedule("2026-09-28",
                                        {{10, 20}, {30, 40}, {40, 50}});
  EXPECT_EQ(schedule.trading_day(), "2026-09-28");
  EXPECT_FALSE(schedule.session_index(9));
  EXPECT_EQ(schedule.session_index(10), 0U);
  EXPECT_EQ(schedule.session_index(19), 0U);
  EXPECT_FALSE(schedule.session_index(20));
  EXPECT_FALSE(schedule.session_index(29));
  EXPECT_EQ(schedule.session_index(40), 2U);
  EXPECT_FALSE(schedule.session_index(50));
  EXPECT_THROW((asterion::TradingDaySchedule("2026-02-29", {{10, 20}})),
               std::invalid_argument);
  EXPECT_THROW(
      (asterion::TradingDaySchedule("2026-09-28", {{10, 20}, {19, 30}})),
      std::invalid_argument);
  EXPECT_THROW(
      (asterion::TradingDaySchedule("2026-09-28", {{20, 30}, {10, 20}})),
      std::invalid_argument);
  EXPECT_THROW((asterion::TradingDaySchedule("2026-09-28", {{10, 10}})),
               std::invalid_argument);
  EXPECT_THROW((asterion::TradingDaySchedule("2026-09-28", {})),
               std::invalid_argument);
}
