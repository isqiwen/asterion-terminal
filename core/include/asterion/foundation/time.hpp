#pragma once
#include <asterion/foundation/error.hpp>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
namespace asterion {
using Nanoseconds = std::int64_t;
inline Nanoseconds checked_time_add(Nanoseconds value, Nanoseconds delta) {
    if (delta < 0 || value > std::numeric_limits<Nanoseconds>::max() - delta)
        throw Error(ErrorCode::invalid_request, "invalid or overflowing time advance");
    return value + delta;
}
class Clock {
public:
    virtual ~Clock() = default;
    virtual Nanoseconds utc_now() const = 0;
    virtual Nanoseconds monotonic_now() const = 0;
};
class SystemClock final : public Clock {
public:
    Nanoseconds utc_now() const override {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
    Nanoseconds monotonic_now() const override {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
};
class ManualClock final : public Clock {
public:
    explicit ManualClock(Nanoseconds utc = 0) : utc_(utc) {}
    Nanoseconds utc_now() const override { std::lock_guard lock(mutex_); return utc_; }
    Nanoseconds monotonic_now() const override { std::lock_guard lock(mutex_); return monotonic_; }
    void advance(Nanoseconds delta) {
        std::lock_guard lock(mutex_);
        const auto utc = checked_time_add(utc_, delta);
        const auto monotonic = checked_time_add(monotonic_, delta);
        utc_ = utc;
        monotonic_ = monotonic;
    }
private:
    mutable std::mutex mutex_;
    Nanoseconds utc_;
    Nanoseconds monotonic_ = 0;
};
}
