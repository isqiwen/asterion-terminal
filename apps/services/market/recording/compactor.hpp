#pragma once
#include <asterion/system/thread_pool.hpp>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <shared_mutex>

namespace asterion::recording {

// The market owner only schedules and observes maintenance. Directory scans,
// archive replacement and teardown all belong to this bounded worker.
class Compactor
{
public:
    using Clock = std::chrono::steady_clock;

    struct Status
    {
        std::string day, error;
    };

    // Keeps the ticks of the latest `tick_days` recorded trading days.
    Compactor(
        std::filesystem::path root,
        std::shared_ptr<std::shared_mutex> archive,
        unsigned tick_days);
    // Returns a trading day (YYYYMMDD) whose merge has just completed.
    std::optional<std::string>
    poll(bool quiet, Clock::time_point now, std::chrono::system_clock::time_point wall);
    Status status() const;

    bool running() const
    {
        return finished_.valid();
    }

private:
    const std::filesystem::path root_;
    const std::shared_ptr<std::shared_mutex> archive_;
    const unsigned tick_days_;
    mutable std::mutex mutex_;
    Status status_;
    Clock::time_point next_{};
    std::future<void> finished_;
    // The day the finished job merged, if it merged one.
    std::string merged_;
    // Destroyed first: stop and join before the state the callback uses.
    ThreadPool worker_{1, 1};
};

} // namespace asterion::recording
