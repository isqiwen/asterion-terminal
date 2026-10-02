#include <asterion/kernel/runtime.hpp>
namespace asterion {
namespace {
// Commands may run concurrently on different threads; only a handler that
// dispatches again on its own thread is recursive.
thread_local const Runtime* dispatching = nullptr;
Clock& required_clock(const std::shared_ptr<Clock>& clock) {
  if (!clock)
    throw Error(ErrorCode::invalid_request, "runtime requires a clock");
  return *clock;
}
} // namespace
Runtime::Runtime(std::string scope, std::shared_ptr<Clock> clock, std::shared_ptr<Logger> logger)
    : clock_(std::move(clock)), ids_(std::move(scope)), logger_(std::move(logger)) {
  (void)required_clock(clock_);
  if (!logger_)
    throw Error(ErrorCode::invalid_request, "runtime requires a logger");
}
Runtime::~Runtime() {
  stop();
}
void Runtime::add_plugin(std::unique_ptr<Plugin> plugin) {
  if (state_ != RuntimeState::created)
    throw Error(ErrorCode::conflict, "runtime composition is sealed");
  plugins_.add(std::move(plugin));
}
void Runtime::command(std::string name, std::function<Json(const Json&)> handler) {
  if (state_ != RuntimeState::created)
    throw Error(ErrorCode::conflict, "runtime composition is sealed");
  validate_id(name);
  if (!handler)
    throw Error(ErrorCode::invalid_request, "empty command handler");
  if (commands_.size() >= 256)
    throw Error(ErrorCode::resource_exhausted, "command registry full");
  if (!commands_.emplace(std::move(name), std::move(handler)).second)
    throw Error(ErrorCode::conflict, "duplicate command");
}
void Runtime::start() {
  if (state_ != RuntimeState::created)
    throw Error(ErrorCode::conflict, "runtime is single-use");
  state_ = RuntimeState::starting;
  try {
    plugins_.start();
    state_ = RuntimeState::running;
    logger_->write(LogLevel::info, "runtime.started");
  } catch (...) {
    state_ = RuntimeState::failed;
    logger_->write(LogLevel::error, "runtime.start_failed");
    throw;
  }
}
void Runtime::stop() {
  if (dispatching == this || state_ == RuntimeState::starting)
    throw Error(ErrorCode::conflict, "cannot stop during runtime callback");
  if (state_ == RuntimeState::stopped || state_ == RuntimeState::stopping)
    return;
  const auto failed = state_ == RuntimeState::failed;
  state_ = RuntimeState::stopping;
  plugins_.stop();
  state_ = failed ? RuntimeState::failed : RuntimeState::stopped;
  logger_->write(LogLevel::info, "runtime.stopped");
  logger_->flush();
}
Json Runtime::dispatch(const std::string& method, const Json& params) {
  if (state_ != RuntimeState::running)
    throw Error(ErrorCode::unavailable, "runtime not running");
  if (dispatching == this)
    throw Error(ErrorCode::conflict, "recursive runtime dispatch");
  validate_id(method);
  const auto found = commands_.find(method);
  const auto operation = found == commands_.end() ? "runtime.unknown" : method;
  const auto trace = ids_.next();
  const auto start = clock_->monotonic_now();
  const auto* previous = dispatching;
  dispatching = this;
  auto finish = [&](bool success) noexcept {
    dispatching = previous;
    // Telemetry failure must never change a completed business result.
    try {
      const auto end = clock_->monotonic_now();
      if (end < start || (start < 0 && end > std::numeric_limits<Nanoseconds>::max() + start))
        return;
      observations_.record({trace, operation, clock_->utc_now(), end - start, success});
      logger_->write(success ? LogLevel::debug : LogLevel::warning, operation,
                     {{"trace_id", trace}, {"duration_ns", end - start}, {"success", success}});
    } catch (...) {
    }
  };
  try {
    if (found == commands_.end())
      throw Error(ErrorCode::invalid_request, "unsupported command");
    auto result = found->second(params);
    finish(true);
    return result;
  } catch (...) {
    finish(false);
    throw;
  }
}
} // namespace asterion
