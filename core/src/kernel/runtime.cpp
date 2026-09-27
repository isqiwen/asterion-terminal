#include <asterion/kernel/runtime.hpp>
namespace asterion {
namespace {
Clock& required_clock(const std::shared_ptr<Clock>& clock) {
  if (!clock)
    throw Error(ErrorCode::invalid_request, "runtime requires a clock");
  return *clock;
}
} // namespace
Runtime::Runtime(std::string scope, std::shared_ptr<Clock> clock, std::shared_ptr<Logger> logger)
    : clock_(std::move(clock)), ids_(std::move(scope)), logger_(std::move(logger)),
      scheduler_(required_clock(clock_)) {
  if (!logger_)
    throw Error(ErrorCode::invalid_request, "runtime requires a logger");
  config_.declare("runtime.workers", 2, [](const Json& value) {
    return value.is_number_integer() && value >= 1 && value <= 256;
  });
  config_.declare("runtime.queue_capacity", 256, [](const Json& value) {
    return value.is_number_integer() && value >= 1 && value <= 65536;
  });
}
Runtime::~Runtime() {
  stop();
}
void Runtime::add_plugin(std::unique_ptr<Plugin> plugin) {
  if (state_ != RuntimeState::created)
    throw Error(ErrorCode::conflict, "runtime composition is sealed");
  plugins_.add(std::move(plugin));
}
void Runtime::command(std::string name, std::string capability,
                      std::function<Json(const Json&)> handler) {
  if (state_ != RuntimeState::created)
    throw Error(ErrorCode::conflict, "runtime composition is sealed");
  validate_id(name);
  validate_id(capability);
  if (!handler)
    throw Error(ErrorCode::invalid_request, "empty command handler");
  if (commands_.size() >= 256)
    throw Error(ErrorCode::resource_exhausted, "command registry full");
  if (!commands_.emplace(std::move(name), Command{std::move(capability), std::move(handler)})
           .second)
    throw Error(ErrorCode::conflict, "duplicate command");
}
void Runtime::start() {
  if (state_ != RuntimeState::created)
    throw Error(ErrorCode::conflict, "runtime is single-use");
  state_ = RuntimeState::starting;
  config_.seal();
  access_.seal();
  try {
    thread_pool_ =
        std::make_unique<ThreadPool>(config_.at("runtime.workers").get<std::size_t>(),
                                     config_.at("runtime.queue_capacity").get<std::size_t>());
    plugins_.start();
    state_ = RuntimeState::running;
    logger_->write(LogLevel::info, "runtime.started");
  } catch (...) {
    if (thread_pool_)
      thread_pool_->shutdown();
    scheduler_.close();
    messages_.close();
    resources_.clear();
    state_ = RuntimeState::failed;
    logger_->write(LogLevel::error, "runtime.start_failed");
    throw;
  }
}
void Runtime::stop() {
  if (dispatching_ || state_ == RuntimeState::starting)
    throw Error(ErrorCode::conflict, "cannot stop during runtime callback");
  if (state_ == RuntimeState::stopped || state_ == RuntimeState::stopping)
    return;
  const auto failed = state_ == RuntimeState::failed;
  state_ = RuntimeState::stopping;
  if (thread_pool_)
    thread_pool_->shutdown();
  scheduler_.close();
  messages_.close();
  plugins_.stop();
  resources_.clear();
  state_ = failed ? RuntimeState::failed : RuntimeState::stopped;
  logger_->write(LogLevel::info, "runtime.stopped");
  logger_->flush();
}
ThreadPool& Runtime::thread_pool() {
  if (state_ != RuntimeState::running)
    throw Error(ErrorCode::unavailable, "runtime not running");
  return *thread_pool_;
}
void Runtime::poll(std::size_t budget) {
  if (state_ != RuntimeState::running)
    throw Error(ErrorCode::unavailable, "runtime not running");
  if (dispatching_)
    throw Error(ErrorCode::conflict, "recursive runtime pump");
  dispatching_ = true;
  std::exception_ptr first_error;
  try {
    scheduler_.run_due(budget);
  } catch (...) {
    first_error = std::current_exception();
  }
  try {
    messages_.dispatch(budget);
  } catch (...) {
    if (!first_error)
      first_error = std::current_exception();
  }
  dispatching_ = false;
  if (first_error)
    std::rethrow_exception(first_error);
}
Json Runtime::dispatch(const std::string& principal, const std::string& method,
                       const Json& params) {
  if (state_ != RuntimeState::running)
    throw Error(ErrorCode::unavailable, "runtime not running");
  if (dispatching_)
    throw Error(ErrorCode::conflict, "recursive runtime dispatch");
  validate_id(method);
  const auto found = commands_.find(method);
  const auto operation = found == commands_.end() ? "runtime.unknown" : method;
  const auto trace = ids_.next();
  const auto start = clock_->monotonic_now();
  dispatching_ = true;
  auto finish = [&](bool success) noexcept {
    dispatching_ = false;
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
    access_.require(principal, found->second.capability);
    auto result = found->second.handler(params);
    finish(true);
    return result;
  } catch (...) {
    finish(false);
    throw;
  }
}
} // namespace asterion
