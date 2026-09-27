#include <gtest/gtest.h>
#include <asterion/foundation/bounded_queue.hpp>
#include <asterion/foundation/decimal.hpp>
#include <asterion/foundation/serialization.hpp>
#include <future>
#include <iostream>
#include <set>
#include <thread>
using namespace asterion;
TEST(Foundation, DecimalRounding) {

  const auto one = Decimal::parse("1");
  EXPECT_TRUE((divide(one, Decimal::parse("8")).str() == "0.125")) << "exact decimal division";
  EXPECT_THROW(([&] { divide(one, Decimal::parse("3")); })(), std::domain_error);
  EXPECT_TRUE((divide(one, Decimal::parse("3"), Rounding::ceiling).str() == "0.33333334"))
      << "explicit division rounding";
  EXPECT_TRUE(
      (divide(Decimal::parse("-1"), Decimal::parse("3"), Rounding::floor).str() == "-0.33333334"))
      << "negative floor";
  EXPECT_TRUE((divide(Decimal::from_raw(1), Decimal::parse("2"), Rounding::half_even).raw() == 0))
      << "half even rounds to even zero";
  EXPECT_TRUE((divide(Decimal::from_raw(3), Decimal::parse("2"), Rounding::half_even).raw() == 2))
      << "half even rounds odd upward";
  const auto minimum = Decimal::from_raw(std::numeric_limits<std::int64_t>::min());
  EXPECT_TRUE((divide(minimum, one) == minimum)) << "minimum magnitude division without overflow";
  EXPECT_TRUE((divide(minimum, minimum) == one)) << "wide remainder division";
  EXPECT_THROW(([&] { divide(minimum, Decimal::parse("-1")); })(), std::overflow_error);
  EXPECT_THROW(([&] { divide(one, Decimal{}); })(), std::domain_error);
  EXPECT_TRUE(
      (quantize(Decimal::parse("1.25"), Decimal::parse("0.5"), Rounding::half_even).str() == "1"))
      << "grid tie to even";
  EXPECT_TRUE(
      (quantize(Decimal::parse("-1.25"), Decimal::parse("0.5"), Rounding::floor).str() == "-1.5"))
      << "negative grid floor";
  EXPECT_THROW(([&] { quantize(Decimal::parse("1.25"), Decimal::parse("0.5")); })(),
               std::domain_error);
}

TEST(Foundation, ConcurrentIdentity) {
  IdSequence ids("test.run");
  std::vector<std::future<std::vector<std::string>>> producers;
  for (int i = 0; i < 4; ++i)
    producers.push_back(std::async(std::launch::async, [&] {
      std::vector<std::string> result;
      for (int j = 0; j < 1000; ++j)
        result.push_back(ids.next());
      return result;
    }));
  std::set<std::string> unique;
  for (auto& producer : producers)
    for (auto& id : producer.get())
      EXPECT_TRUE((unique.insert(id).second)) << "concurrent identity collision";
  EXPECT_TRUE((unique.size() == 4000)) << "all IDs generated";
  EXPECT_THROW(([] { IdSequence bad("invalid scope"); })(), Error);
}

TEST(Foundation, ClockDomains) {
  ManualClock clock(100);
  clock.advance(5);
  EXPECT_TRUE((clock.utc_now() == 105 && clock.monotonic_now() == 5))
      << "manual time domains advance together";
  EXPECT_THROW(([&] { clock.advance(-1); })(), Error);
  ManualClock maximum(std::numeric_limits<Nanoseconds>::max());
  EXPECT_THROW(([&] { maximum.advance(1); })(), Error);
  EXPECT_TRUE((maximum.monotonic_now() == 0)) << "overflow cannot partially advance clock";
}

TEST(Foundation, StrictSerialization) {
  const EventEnvelope event{"test:1",
                            "test.source",
                            "tick.received",
                            1790384400000000001LL,
                            {{"price", "3510.00000001"}}};
  const auto copy = decode_event(encode_event(event));
  EXPECT_TRUE((copy.timestamp_ns == event.timestamp_ns && copy.payload == event.payload))
      << "exact nanosecond and decimal strings survive wire roundtrip";
  EXPECT_THROW(([] { static_cast<void>(parse_json(R"({"a":1,"a":2})")); })(), Error);
  EXPECT_THROW(([] { static_cast<void>(parse_json(R"({"nested":{"a":1,"a":2}})")); })(), Error);
  EXPECT_TRUE((parse_json(R"({"a":{"k":1},"b":{"k":2}})").size() == 2))
      << "keys in distinct objects allowed";
  EXPECT_THROW(
      ([] { static_cast<void>(parse_json(std::string(70, '[') + "0" + std::string(70, ']'))); })(),
      Error);
  EXPECT_THROW(([] { static_cast<void>(parse_json("{}", 1)); })(), Error);
  auto wire = Json::parse(encode_event(event));
  wire["version"] = 1.0;
  EXPECT_THROW(([&] { static_cast<void>(decode_event(wire.dump())); })(), Error);
  wire["version"] = 1;
  wire["timestamp_ns"] = "01";
  EXPECT_THROW(([&] { static_cast<void>(decode_event(wire.dump())); })(), Error);
}

TEST(Foundation, BoundedConcurrency) {
  BoundedQueue<int> queue(2);
  EXPECT_TRUE((queue.try_push(1) && queue.try_push(2) && !queue.try_push(3)))
      << "queue backpressure";
  queue.close();
  EXPECT_TRUE(
      (!queue.try_push(4) && queue.wait_pop() == 1 && queue.wait_pop() == 2 && !queue.wait_pop()))
      << "close drains accepted items then terminates";
  BoundedQueue<int> waiting(1);
  std::promise<void> entered;
  std::jthread waiter([&](std::stop_token stop) {
    entered.set_value();
    EXPECT_TRUE((!waiting.wait_pop(stop))) << "cancellation wakes idle consumer";
  });
  entered.get_future().wait();
  waiter.request_stop();
  waiter.join();
  BoundedQueue<int> concurrent(4096);
  std::vector<std::jthread> threads;
  for (int i = 0; i < 4; ++i)
    threads.emplace_back([&, i] {
      for (int j = 0; j < 1000; ++j)
        EXPECT_TRUE((concurrent.try_push(i * 1000 + j))) << "multi-producer capacity";
    });
  for (auto& thread : threads)
    thread.join();
  concurrent.close();
  std::set<int> values;
  while (auto value = concurrent.wait_pop())
    values.insert(*value);
  EXPECT_TRUE((values.size() == 4000)) << "concurrent queue loses no accepted items";
}
TEST(Foundation, ErrorCodesRoundTripAndExceptionsClassify) {
  for (const auto code : error_codes)
    EXPECT_EQ(parse_error_code(error_name(code)), code);
  EXPECT_FALSE(parse_error_code("future_code").has_value());
  EXPECT_EQ(classify(Error(ErrorCode::conflict, "x")), ErrorCode::conflict);
  EXPECT_EQ(classify(std::invalid_argument("x")), ErrorCode::invalid_request);
  EXPECT_EQ(classify(std::overflow_error("x")), ErrorCode::invalid_request);
  EXPECT_EQ(classify(std::logic_error("x")), ErrorCode::conflict);
  EXPECT_EQ(classify(std::runtime_error("x")), ErrorCode::operation_failed);
  EXPECT_EQ(classify(std::bad_alloc()), ErrorCode::resource_exhausted);
  try {
    (void)Json::parse("{");
  } catch (const std::exception& e) {
    EXPECT_EQ(classify(e), ErrorCode::invalid_request);
  }
  try {
    throw_remote_error("not_found", "missing");
  } catch (const Error& e) {
    EXPECT_EQ(e.code(), ErrorCode::not_found);
  }
  try {
    throw_remote_error("from_newer_peer", "?");
  } catch (const Error& e) {
    EXPECT_EQ(e.code(), ErrorCode::operation_failed);
  }
}
