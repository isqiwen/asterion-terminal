#pragma once
#include <asterion/base/error.hpp>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>

namespace asterion {

using Nanoseconds = std::int64_t;

inline Nanoseconds checked_time_add(Nanoseconds value, Nanoseconds delta)
{
    if (delta < 0 || value > std::numeric_limits<Nanoseconds>::max() - delta)
        throw Error(ErrorCode::invalid_request, "invalid or overflowing time advance");
    return value + delta;
}

class Clock
{
public:
    virtual ~Clock() = default;
    virtual Nanoseconds utc_now() const = 0;
    virtual Nanoseconds monotonic_now() const = 0;
};

class SystemClock final : public Clock
{
public:
    Nanoseconds utc_now() const override
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    Nanoseconds monotonic_now() const override
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
};

class ManualClock final : public Clock
{
public:
    explicit ManualClock(Nanoseconds utc = 0)
        : utc_(utc)
    {}

    Nanoseconds utc_now() const override
    {
        std::lock_guard lock(mutex_);
        return utc_;
    }

    Nanoseconds monotonic_now() const override
    {
        std::lock_guard lock(mutex_);
        return monotonic_;
    }

    void advance(Nanoseconds delta)
    {
        std::lock_guard lock(mutex_);
        const auto utc = checked_time_add(utc_, delta);
        const auto monotonic = checked_time_add(monotonic_, delta);
        utc_ = utc;
        monotonic_ = monotonic;
    }

    template <class Rep, class Period>
    void advance(std::chrono::duration<Rep, Period> delta)
    {
        advance(std::chrono::duration_cast<std::chrono::nanoseconds>(delta).count());
    }

private:
    mutable std::mutex mutex_;
    Nanoseconds utc_;
    Nanoseconds monotonic_ = 0;
};

inline std::chrono::steady_clock::time_point steady_now(const Clock& clock)
{
    return std::chrono::steady_clock::time_point{std::chrono::nanoseconds{clock.monotonic_now()}};
}

inline std::int64_t utc_now_ms(const Clock& clock)
{
    return clock.utc_now() / 1'000'000;
}

inline std::shared_ptr<const Clock> default_clock()
{
    static const auto instance = std::make_shared<SystemClock>();
    return instance;
}

} // namespace asterion
