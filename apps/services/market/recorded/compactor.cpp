#include "compactor.hpp"
#include "compaction.hpp"
#include <asterion/system/log.hpp>
#include <algorithm>

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

std::optional<std::string>
Compactor::poll(Clock::time_point now, std::chrono::system_clock::time_point wall)
{
    if (finished_.valid()) {
        if (finished_.wait_for(0s) != std::future_status::ready)
            return {};
        std::lock_guard lock(mutex_);
        std::optional<std::string> merged;
        try {
            finished_.get();
            if (!merged_.empty()) {
                retry_.erase(merged_);
                merged = std::exchange(merged_, {});
            }
            if (retry_.empty())
                status_.error.clear();
        } catch (const std::exception& error) {
            status_.error = error.what();
            // A day that fails waits alone; a scan that fails has no day to blame.
            if (status_.day.empty())
                next_ = now + 1h;
            else
                retry_[status_.day] = now + 1h;
            log::error("market recording merge failed: {}", status_.error);
        }
        status_.day.clear();
        return merged;
    }
    if (now < next_)
        return {};
    next_ = now + 1min;
    finished_ = worker_.submit([this, now, wall](std::stop_token stop) {
        const auto days = compactable_days(root_, wall, stop);
        if (stop.stop_requested())
            return;
        std::string day;
        {
            std::lock_guard lock(mutex_);
            const auto due = std::find_if(days.begin(), days.end(), [&](const std::string& item) {
                const auto retry = retry_.find(item);
                return retry == retry_.end() || retry->second <= now;
            });
            if (due != days.end())
                day = *due;
            status_.day = day;
        }
        std::unique_lock lock(*archive_, std::defer_lock);
        while (!lock.try_lock()) {
            if (stop.stop_requested())
                return;
            std::this_thread::sleep_for(10ms);
        }
        if (stop.stop_requested())
            return;
        if (!day.empty()) {
            compact_day(root_, day, stop);
            std::lock_guard state(mutex_);
            merged_ = day;
            return;
        }
        // Old ticks go once every finished day had its merge; a day still failing keeps its own.
        for (const auto& removed : prune_ticks(root_, tick_days_, days))
            log::info("market recording ticks removed: trading_day={}", removed);
    });
    return {};
}

} // namespace asterion::recording
