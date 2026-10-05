#pragma once
#include <coroutine>
#include <exception>
#include <utility>
namespace asterion::trading {
// One account-owned command, suspended only at an explicit asynchronous boundary.
// The account loop owns the root; nested commands transfer back to their caller.
class AccountCommand {
public:
  struct promise_type;
  using Handle = std::coroutine_handle<promise_type>;
  struct promise_type {
    std::exception_ptr error;
    std::coroutine_handle<> caller = std::noop_coroutine();
    AccountCommand get_return_object() { return AccountCommand(Handle::from_promise(*this)); }
    std::suspend_always initial_suspend() noexcept { return {}; }
    struct Finish {
      bool await_ready() noexcept { return false; }
      std::coroutine_handle<> await_suspend(Handle command) noexcept {
        return command.promise().caller;
      }
      void await_resume() noexcept {}
    };
    Finish final_suspend() noexcept { return {}; }
    void return_void() noexcept {}
    void unhandled_exception() noexcept { error = std::current_exception(); }
  };
  explicit AccountCommand(Handle handle) : handle_(handle) {}
  AccountCommand(AccountCommand&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
  AccountCommand(const AccountCommand&) = delete;
  ~AccountCommand() {
    if (handle_)
      handle_.destroy();
  }
  void start() { handle_.resume(); }
  bool done() const { return handle_.done(); }
  void result() const {
    if (handle_.promise().error)
      std::rethrow_exception(handle_.promise().error);
  }
  struct Awaiter {
    Handle handle;
    ~Awaiter() { handle.destroy(); }
    bool await_ready() const noexcept { return false; }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
      handle.promise().caller = caller;
      return handle;
    }
    void await_resume() const {
      if (handle.promise().error)
        std::rethrow_exception(handle.promise().error);
    }
  };
  Awaiter operator co_await() && noexcept { return {std::exchange(handle_, {})}; }

private:
  Handle handle_;
};
} // namespace asterion::trading
