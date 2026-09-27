#include "kernel/ipc/timed_operation.hpp"
#include <functional>
#include <gtest/gtest.h>
using namespace asterion;
using namespace asterion::ipc::detail;
using namespace std::chrono_literals;

// Cancellation cannot withdraw an accept completion already committed by the
// OS. Deliver that successful completion after the idle timer fires, without
// relying on scheduler timing or thousands of probabilistic connections.
TEST(TransportDeadline, AcceptedSocketSurvivesIdleCancellationRace) {
  asio::io_context io;
  std::function<void(asio::error_code)> complete;
  bool cancelled = false;
  EXPECT_NO_THROW(run(
      io, 0ms, [&](auto done) { complete = done; },
      [&] {
        cancelled = true;
        asio::post(io, [&] { complete({}); });
      },
      DeadlinePolicy::preserve_accepted));
  EXPECT_TRUE(cancelled);
}
TEST(TransportDeadline, IdleCancellationWithoutAcceptRemainsTimeout) {
  asio::io_context io;
  std::function<void(asio::error_code)> complete;
  EXPECT_THROW(run(
                   io, 0ms, [&](auto done) { complete = done; },
                   [&] {
                     asio::post(
                         io, [&] { complete(asio::error::operation_aborted); });
                   },
                   DeadlinePolicy::preserve_accepted),
               Error);
}
TEST(TransportDeadline, RequestDeadlineRemainsStrict) {
  asio::io_context io;
  std::function<void(asio::error_code)> complete;
  EXPECT_THROW(run(
                   io, 0ms, [&](auto done) { complete = done; },
                   [&] { asio::post(io, [&] { complete({}); }); }),
               Error);
}
TEST(TransportDeadline, CompletedOperationDoesNotWaitForDeadline) {
  asio::io_context io;
  bool cancelled = false;
  EXPECT_NO_THROW(run(
      io, 10s,
      [&](auto done) { asio::post(io, [done] { done(asio::error_code{}); }); },
      [&] { cancelled = true; }, DeadlinePolicy::preserve_accepted));
  EXPECT_FALSE(cancelled);
}
