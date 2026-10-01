#include <gtest/gtest.h>

#include <asterion/domain/order.hpp>

#include <iostream>
#include <limits>
#include <vector>

using namespace asterion;
namespace {
Decimal d(const char* value) {
  return Decimal::parse(value);
}
Instrument instrument() {
  return {{"TEST", "TEST-202612"}, AssetClass::futures, "CNY", d("0.2"), d("1"), d("10")};
}
TEST(Domain, decimal_contract) {
  for (const char* value : {"0", "0.00000001", "-0.00000001", "12.3456", "-12.3456",
                            "92233720368.54775807", "-92233720368.54775808"}) {
    EXPECT_TRUE((Decimal::parse(value).str() == value)) << "decimal must roundtrip exactly";
  }
  EXPECT_TRUE((d("1.25000000").str() == "1.25")) << "decimal formatting";
  for (const char* value :
       {"", "-", "+1", " 1", "1 ", "1e3", "1.", ".1", "01", "1.000000000", "nan", "1.2.3"}) {
    EXPECT_THROW(([&] { Decimal::parse(value); })(), std::invalid_argument);
  }
  for (const char* value :
       {"92233720368.54775808", "-92233720368.54775809", "999999999999999999999999999"}) {
    EXPECT_THROW(([&] { Decimal::parse(value); })(), std::overflow_error);
  }
  EXPECT_TRUE((d("0.1") + d("0.2") == d("0.3"))) << "exact addition";
  EXPECT_TRUE((d("-0.1") - d("0.2") == d("-0.3"))) << "exact subtraction";
  EXPECT_TRUE((d("123.45") * d("10") == d("1234.5"))) << "exact notional";
  EXPECT_TRUE((d("0.0001") * d("0.0001") == d("0.00000001"))) << "small exact product";
  EXPECT_TRUE((d("-1.25") * d("-0.8") == d("1"))) << "signed multiplication";
  const auto maximum = Decimal::from_raw(std::numeric_limits<std::int64_t>::max());
  const auto minimum = Decimal::from_raw(std::numeric_limits<std::int64_t>::min());
  const auto quantum = Decimal::from_raw(1);
  EXPECT_TRUE((maximum * d("1") == maximum))
      << "large multiplication without intermediate overflow";
  EXPECT_TRUE((minimum * d("1") == minimum)) << "minimum multiplication";
  EXPECT_TRUE((minimum - minimum == Decimal{})) << "minimum subtraction without negation overflow";
  EXPECT_TRUE((minimum + maximum == Decimal::from_raw(-1))) << "extreme addition";
  EXPECT_THROW(([&] { static_cast<void>(maximum + quantum); })(), std::overflow_error);
  EXPECT_THROW(([&] { static_cast<void>(minimum - quantum); })(), std::overflow_error);
  EXPECT_THROW(([&] { static_cast<void>(maximum - minimum); })(), std::overflow_error);
  EXPECT_THROW(([&] { static_cast<void>(minimum * d("-1")); })(), std::overflow_error);
  EXPECT_THROW(([&] { static_cast<void>(maximum * d("2")); })(), std::overflow_error);
  EXPECT_THROW(([&] { static_cast<void>(quantum * quantum); })(), std::domain_error);
  EXPECT_TRUE((d("1.2").multiple_of(d("0.2")))) << "valid tick grid";
  EXPECT_TRUE((!d("1.3").multiple_of(d("0.2")))) << "invalid tick grid";
  EXPECT_THROW(([&] { static_cast<void>(d("1").multiple_of(Decimal{})); })(),
               std::invalid_argument);
  // Independent integer oracle: cents times cents yields exactly four places.
  for (std::int64_t a = -100; a <= 100; ++a) {
    for (std::int64_t b = -100; b <= 100; ++b) {
      EXPECT_TRUE((Decimal::from_raw(a * 1000000) * Decimal::from_raw(b * 1000000) ==
                   Decimal::from_raw(a * b * 10000)))
          << "decimal product integer oracle";
    }
  }
}
TEST(Domain, market_contract) {
  auto spec = instrument();
  spec.validate();
  const auto ok = [&](MarketBar bar) { bar.validate(spec); };
  const auto bad = [&](MarketBar bar) {
    EXPECT_THROW(([&] { bar.validate(spec); })(), std::invalid_argument);
  };
  // Negative prices can be valid in some derivative markets.
  ok({"2026-09-28", 0, d("-0.2"), d("0"), d("-0.4"), d("-0.2"), d("1")});
  bad({"2026-09-28", -1, d("1"), d("1"), d("1"), d("1"), d("1")});
  bad({"2026-09-28", 1, d("1.1"), d("2"), d("1"), d("1"), d("1")});
  bad({"2026-09-28", 1, d("1"), d("1"), d("1"), d("1"), d("0.5")});
  bad({"2026-09-28", 1, d("1"), d("1"), d("2"), d("1"), d("1")}); // low above high
  bad({"2026-09-28", 1, d("3"), d("2"), d("1"), d("1"), d("1")}); // open above high
  bad({"2026-02-30", 1, d("1"), d("1"), d("1"), d("1"), d("1")}); // invalid trading day
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
  EXPECT_TRUE(
      (order.state() == OrderState::partially_filled && order.remaining_quantity() == d("2")))
      << "partial fill state";
  EXPECT_THROW(([&] { order.apply({"fill-1", request.id, d("2"), d("9.8")}); })(),
               std::invalid_argument);
  EXPECT_THROW(([&] { order.apply({"fill-2", request.id, d("3"), d("9.8")}); })(),
               std::invalid_argument);
  EXPECT_THROW(([&] { order.apply({"fill-2", request.id, d("1"), d("10.2")}); })(),
               std::invalid_argument);
  EXPECT_TRUE((order.filled_quantity() == d("1"))) << "bad reports cannot mutate state";
  EXPECT_TRUE((order.apply({"fill-2", request.id, d("2"), d("10")}))) << "final fill";
  EXPECT_TRUE((order.state() == OrderState::filled && order.remaining_quantity() == Decimal{}))
      << "fully filled";
  EXPECT_TRUE((!order.apply(first))) << "duplicate still ignored after terminal state";
  EXPECT_THROW(([&] { order.cancel(); })(), std::logic_error);
  EXPECT_THROW(([&] { order.apply({"fill-3", request.id, d("1"), d("10")}); })(), std::logic_error);
  Order cancelled(request, spec);
  cancelled.accept();
  cancelled.apply(first);
  cancelled.cancel();
  EXPECT_TRUE((cancelled.remaining_quantity() == d("2"))) << "cancel retains executed quantity";
  EXPECT_THROW(([&] { cancelled.apply({"fill-3", request.id, d("1"), d("10")}); })(),
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
} // namespace
