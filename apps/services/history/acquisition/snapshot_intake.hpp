#pragma once
#include "manager.hpp"
#include <chrono>
#include <map>
#include <thread>

namespace asterion::history::acquisition {

// Takes each finished trading day's contract snapshot into the warehouse, on
// its own thread. It learns of them from the disk alone; a day that fails is
// tried again an hour later without holding back the others.
class SnapshotIntake
{
public:
    SnapshotIntake(std::filesystem::path snapshots, std::filesystem::path data, Manager& manager);

    void stop()
    {
        thread_.request_stop();
        if (thread_.joinable())
            thread_.join();
    }

    // One step of the thread's loop.
    void
    poll(std::chrono::steady_clock::time_point now, std::chrono::system_clock::time_point wall);

private:
    const std::filesystem::path snapshots_;
    warehouse::Store store_;
    Manager& manager_;
    std::map<std::string, std::chrono::steady_clock::time_point> retry_;
    std::string task_, day_;
    std::jthread thread_;
};

} // namespace asterion::history::acquisition
