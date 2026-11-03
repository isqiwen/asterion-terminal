#include "compactor.hpp"
#include "compaction.hpp"
#include <asterion/system/log.hpp>

namespace asterion::recording {

using namespace std::chrono_literals;

Compactor::Compactor(
    std::filesystem::path root,
    std::shared_ptr<std::shared_mutex> archive,
    unsigned tick_days)
    : root_(std::move(root)),
      archive_(std::move(archive)),
      tick_days_(tick_days)
{}

Compactor::Status Compactor::status() const
{
    std::lock_guard lock(mutex_);
    return status_;
}

void Compactor::poll(bool quiet, Clock::time_point now, std::chrono::system_clock::time_point wall)
{
    if (finished_.valid()) {
        if (finished_.wait_for(0s) != std::future_status::ready)
            return;
        std::lock_guard lock(mutex_);
        try {
            finished_.get();
            status_.error.clear();
        } catch (const std::exception& error) {
            status_.error = error.what();
            next_ = now + 1h;
            log::error("market recording merge failed: {}", status_.error);
        }
        status_.day.clear();
        return;
    }
    if (!quiet || now < next_)
        return;
    next_ = now + 1min;
    finished_ = worker_.submit([this, wall](std::stop_token stop) {
        const auto days = compactable_days(root_, wall, stop);
        if (stop.stop_requested())
            return;
        if (!days.empty()) {
            std::lock_guard lock(mutex_);
            status_.day = days.front();
        }
        std::unique_lock lock(*archive_, std::defer_lock);
        while (!lock.try_lock()) {
            if (stop.stop_requested())
                return;
            std::this_thread::sleep_for(10ms);
        }
        if (stop.stop_requested())
            return;
        // Old ticks go once every finished day is merged.
        if (!days.empty())
            compact_day(root_, days.front(), stop);
        else
            for (const auto& day : prune_ticks(root_, tick_days_))
                log::info("market recording ticks removed: trading_day={}", day);
    });
}

} // namespace asterion::recording
