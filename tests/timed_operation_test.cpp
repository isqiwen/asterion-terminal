#include "kernel/ipc/timed_operation.hpp"
#include <functional>
#include <gtest/gtest.h>
using namespace asterion;
using namespace asterion::ipc::detail;
using namespace std::chrono_literals;

// A deadline that fires fails the operation even if its completion then arrives.
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
      io, 10s, [&](auto done) { asio::post(io, [done] { done(asio::error_code{}); }); },
      [&] { cancelled = true; }));
  EXPECT_FALSE(cancelled);
}
