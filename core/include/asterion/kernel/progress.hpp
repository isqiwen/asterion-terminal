#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
namespace asterion {
// The executor alone marks progress; observers only measure its age. One atomic
// publishes the phase and timestamp together, including a blocked operation.
class Progress {
public:
  struct Observation {
    bool observed = false;
    bool pending = false;
    std::uint64_t age_ms = 0;
  };
  void begin() noexcept { stamp_ = -now(); }
  void finish() noexcept { stamp_ = now(); }
  Observation observe() const noexcept {
    const auto stamp = stamp_.load();
    return {stamp != 0, stamp < 0,
            stamp ? static_cast<std::uint64_t>(now() - (stamp < 0 ? -stamp : stamp)) : 0};
  }

private:
  static std::int64_t now() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }
  std::atomic<std::int64_t> stamp_{0};
};
} // namespace asterion
