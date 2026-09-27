#pragma once
#include <asterion/foundation/id.hpp>
#include <asterion/foundation/time.hpp>
#include <deque>
#include <map>
#include <mutex>
#include <vector>
namespace asterion {
struct Observation {
  std::string trace_id;
  std::string operation;
  Nanoseconds timestamp_ns;
  Nanoseconds duration_ns;
  bool success;
};
struct Metrics {
  std::uint64_t succeeded = 0, failed = 0;
};
// Deliberately no arbitrary log messages, request bodies, paths or credentials.
class Observability final {
public:
  explicit Observability(std::size_t capacity = 256) : capacity_(capacity) {
    if (!capacity)
      throw Error(ErrorCode::invalid_request, "invalid observation capacity");
  }
  void record(Observation observation) {
    validate_id(observation.trace_id);
    validate_id(observation.operation);
    if (observation.duration_ns < 0)
      throw Error(ErrorCode::invalid_request, "negative observation duration");
    std::lock_guard lock(mutex_);
    if (observation.success)
      ++metrics_.succeeded;
    else
      ++metrics_.failed;
    recent_.push_back(std::move(observation));
    if (recent_.size() > capacity_) {
      recent_.pop_front();
      ++dropped_;
    }
  }
  Metrics metrics() const {
    std::lock_guard lock(mutex_);
    return metrics_;
  }
  std::vector<Observation> recent() const {
    std::lock_guard lock(mutex_);
    return {recent_.begin(), recent_.end()};
  }
  std::uint64_t dropped() const {
    std::lock_guard lock(mutex_);
    return dropped_;
  }

private:
  std::size_t capacity_;
  mutable std::mutex mutex_;
  Metrics metrics_;
  std::uint64_t dropped_ = 0;
  std::deque<Observation> recent_;
};
} // namespace asterion
