#include "recorded_intake.hpp"
#include "recorded_publication.hpp"
#include "recorded/compaction.hpp"
#include <asterion/system/log.hpp>
#include <condition_variable>

namespace asterion::market_data::acquisition {

using namespace std::chrono_literals;

RecordedIntake::RecordedIntake(
    std::filesystem::path recordings,
    std::shared_ptr<std::shared_mutex> archive,
    unsigned tick_days,
    Manager& manager)
    : recordings_(std::move(recordings)),
      manager_(manager),
      compactor_(recordings_, std::move(archive), tick_days)
{
    // Recovery finishes before a client can read the archive. A damaged journal
    // is reported again when its day is merged.
    try {
        recording::finish_compactions(recordings_);
    } catch (const std::exception& error) {
        log::error("market recording merge recovery failed: {}", error.what());
    }
    thread_ = std::jthread([this](std::stop_token stop) {
        run(stop);
    });
}

void RecordedIntake::run(std::stop_token stop)
{
    std::optional<RecordedPublication> publication;
    std::mutex mutex;
    std::condition_variable_any woken;
    while (!stop.stop_requested()) {
        auto wait = 1s;
        try {
            if (!publication)
                publication.emplace(recordings_ / "unpublished.json", manager_);
            const auto now = RecordedPublication::Clock::now();
            if (const auto day = compactor_.poll(now, std::chrono::system_clock::now()))
                publication->add(*day);
            publication->poll(now);
        } catch (const std::exception& error) {
            // The recording files are intact: this work can be done later.
            log::error("market recorded intake failed: {}", error.what());
            wait = 60s;
        }
        std::unique_lock lock(mutex);
        woken.wait_for(lock, stop, wait, [] {
            return false;
        });
    }
}

} // namespace asterion::market_data::acquisition
