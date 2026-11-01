#pragma once
#include <asterion/system/thread_pool.hpp>
#include <chrono>
#include <filesystem>
#include <mutex>
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

    Compactor(std::filesystem::path root, std::shared_ptr<std::shared_mutex> archive);
    void poll(bool quiet, Clock::time_point now, std::chrono::system_clock::time_point wall);
    Status status() const;

    bool running() const
    {
        return finished_.valid();
    }

private:
    const std::filesystem::path root_;
    const std::shared_ptr<std::shared_mutex> archive_;
    mutable std::mutex mutex_;
    Status status_;
    Clock::time_point next_{};
    std::future<void> finished_;
    // Destroyed first: stop and join before the state the callback uses.
    ThreadPool worker_{1, 1};
};

} // namespace asterion::recording
