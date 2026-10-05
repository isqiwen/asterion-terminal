#pragma once
#include <cassert>
#include <coroutine>
#include <exception>
#include <functional>
#include <future>
#include <optional>
#include <utility>
namespace asterion {
namespace polled_detail {
template <class T> struct Result {
  std::optional<T> value;
  void return_value(T result) { value.emplace(std::move(result)); }
  T take() { return std::move(*value); }
};
template <> struct Result<void> {
  void return_void() {}
  void take() {}
};
} // namespace polled_detail
// A suspended operation owned and advanced by one event-loop thread. No scheduler,
// worker or retry is created. Keep it alive until external work using its locals
// has completed; poll only on its owner and take the result once after completion.
template <class T = void> class PolledTask {
public:
  struct promise_type : polled_detail::Result<T> {
    std::function<bool()> ready;
    std::exception_ptr error;
    PolledTask get_return_object() {
      return PolledTask(std::coroutine_handle<promise_type>::from_promise(*this));
    }
    std::suspend_always initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }
    void unhandled_exception() { error = std::current_exception(); }
  };
  PolledTask(PolledTask&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
  PolledTask(const PolledTask&) = delete;
  ~PolledTask() {
    if (handle_)
      handle_.destroy();
  }
  bool poll() {
    auto& promise = handle_.promise();
    if (!handle_.done() && (!promise.ready || promise.ready())) {
      promise.ready = {};
      handle_.resume();
    }
    return handle_.done();
  }
  T take() {
    assert(handle_.done());
    auto& promise = handle_.promise();
    if (promise.error)
      std::rethrow_exception(promise.error);
    return promise.take();
  }
  bool await_ready() { return poll(); }
  template <class P> void await_suspend(std::coroutine_handle<P> parent) {
    parent.promise().ready = [this] { return poll(); };
  }
  T await_resume() { return take(); }

private:
  explicit PolledTask(std::coroutine_handle<promise_type> handle) : handle_(handle) {}
  std::coroutine_handle<promise_type> handle_;
};
// Readiness predicates must only observe completion. Report operation failures
// after resumption, inside the coroutine's normal exception boundary.
template <class Predicate> struct PollUntil {
  Predicate ready;
  bool await_ready() const { return ready(); }
  template <class P> void await_suspend(std::coroutine_handle<P> parent) {
    parent.promise().ready = [this] { return ready(); };
  }
  void await_resume() const noexcept {}
};
template <class T> struct PollFuture {
  std::future<T> future;
  bool await_ready() const {
    return future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
  }
  template <class P> void await_suspend(std::coroutine_handle<P> parent) {
    parent.promise().ready = [this] { return await_ready(); };
  }
  T await_resume() { return future.get(); }
};
} // namespace asterion
