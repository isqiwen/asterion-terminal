// Test-only Native fixture: suspended I/O without services or user state.
#include "terminal_application.hpp"
#include "service_io.hpp"
#include <asterion/foundation/serialization.hpp>
#include <asterion/foundation/error.hpp>
#include <chrono>
#include <cstdlib>
namespace asterion::terminal {
struct Application::Impl {
  ServiceIo io;
  PayloadBudget replies{1024 * 1024};
  unsigned running = 0;
  ~Impl() {
    if (running != 0)
      std::terminate();
  }
};
Application::Application() {
  if (std::getenv("ASTERION_TEST_BRIDGE_INIT_FAILURE"))
    throw Error(ErrorCode::recovery_required, "Fixture initialization requires recovery");
  impl_ = std::make_unique<Impl>();
}
Application::~Application() = default;
std::future<void> Application::request(std::string wire, Completion complete) {
  return impl_->io.submit<void>(
      [this, wire = std::move(wire),
       complete = std::move(complete)](std::stop_token) -> PolledTask<void> {
        const auto method = parse_json(wire).at("method");
        if (method == "fixture.slow") {
          ++impl_->running;
          const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
          co_await PollUntil{[&] { return std::chrono::steady_clock::now() >= end; }};
          --impl_->running;
        }
        if (method == "fixture.fail")
          complete({}, std::make_exception_ptr(
                           Error(ErrorCode::resource_exhausted, "Fixture response failure")));
        else
          complete(impl_->replies.retain(Json{{"running", impl_->running}}.dump()), {});
      },
      ServiceIo::Lane::application);
}
} // namespace asterion::terminal
