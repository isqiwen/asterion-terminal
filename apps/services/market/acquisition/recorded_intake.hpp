#pragma once
#include "manager.hpp"
#include "recorded/compactor.hpp"
#include <shared_mutex>
#include <thread>

namespace asterion::market_data::acquisition {

// Takes finished trading days from the recording files on its own thread:
// merges them and publishes them through `manager`. It learns of them from the
// disk alone, so nothing here can reach the live market, and no failure here
// stops anything but itself.
class RecordedIntake
{
public:
    RecordedIntake(
        std::filesystem::path recordings,
        std::shared_ptr<std::shared_mutex> archive,
        unsigned tick_days,
        Manager& manager);

    recording::Compactor::Status status() const
    {
        return compactor_.status();
    }

    void stop()
    {
        thread_.request_stop();
        if (thread_.joinable())
            thread_.join();
    }

private:
    void run(std::stop_token stop);

    const std::filesystem::path recordings_;
    Manager& manager_;
    recording::Compactor compactor_;
    std::jthread thread_;
};

} // namespace asterion::market_data::acquisition
