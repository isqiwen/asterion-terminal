#pragma once
#include <asio.hpp>
#include <asterion/foundation/error.hpp>
namespace asterion::ipc::detail {
enum class DeadlinePolicy { strict, preserve_accepted };
// Drain both operation and cancellation completion before destroying their
// state.
template <class Start, class Cancel>
void run(asio::io_context& io, std::chrono::milliseconds timeout, Start start, Cancel cancel,
         DeadlinePolicy policy = DeadlinePolicy::strict) {
  io.restart();
  asio::steady_timer timer(io);
  asio::error_code result;
  bool expired = false, completed = false;
  if (timeout.count() >= 0) {
    timer.expires_after(timeout);
    timer.async_wait([&](asio::error_code ec) {
      if (!ec) {
        expired = true;
        cancel();
      }
    });
  }
  try {
    start([&](asio::error_code ec, auto...) {
      completed = true;
      result = ec;
      timer.cancel();
    });
  } catch (...) {
    timer.cancel();
    cancel();
    io.run();
    throw;
  }
  io.run();
  // An accept cancellation may race with an already committed successful
  // accept. Its idle deadline is not a deadline on the accepted connection.
  // Other operations close their socket on cancellation and remain strict.
  if (expired && (policy == DeadlinePolicy::strict || !completed || result))
    throw Error(ErrorCode::unavailable, "TCP/TLS operation timed out");
  if (result)
    throw Error(ErrorCode::unavailable, "TCP/TLS connection failed: " + result.message());
}
} // namespace asterion::ipc::detail
