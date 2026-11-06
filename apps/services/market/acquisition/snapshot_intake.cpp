#include "snapshot_intake.hpp"
#include <asterion/protocol/common.hpp>
#include <asterion/system/log.hpp>
#include <asterion/system/process/child.hpp>
#include <condition_variable>
#include <ctime>

namespace asterion::market_data::acquisition {

using namespace std::chrono_literals;

namespace {

// Trading days before today in Beijing, and today from 15:30 on: a day's
// catalog no longer changes. A night session belongs to the next trading day.
bool ended(const std::string& trading_day, std::chrono::system_clock::time_point now)
{
    const auto time = std::chrono::system_clock::to_time_t(now) + 8 * 3600;
    std::tm tm{};
    gmtime_r(&time, &tm);
    char today[9], clock[5];
    std::strftime(today, sizeof(today), "%Y%m%d", &tm);
    std::strftime(clock, sizeof(clock), "%H%M", &tm);
    return trading_day < today || (trading_day == today && std::string(clock) >= "1530");
}

} // namespace

SnapshotIntake::SnapshotIntake(
    std::filesystem::path snapshots,
    std::filesystem::path data,
    Manager& manager)
    : snapshots_(std::move(snapshots)),
      store_(warehouse::source_directory(data, "ctp"), "ctp"),
      manager_(manager),
      thread_([this](std::stop_token stop) {
          std::mutex mutex;
          std::condition_variable_any woken;
          while (!stop.stop_requested()) {
              try {
                  poll(std::chrono::steady_clock::now(), std::chrono::system_clock::now());
              } catch (const std::exception& error) {
                  // The snapshots are intact: this work can be done later.
                  log::error("market contract snapshot intake failed: {}", error.what());
              }
              std::unique_lock lock(mutex);
              woken.wait_for(lock, stop, 5s, [] {
                  return false;
              });
          }
      })
{}

void SnapshotIntake::poll(
    std::chrono::steady_clock::time_point now,
    std::chrono::system_clock::time_point wall)
{
    if (!task_.empty()) {
        const auto task = manager_.get(task_);
        if (task.state() == wire::TASK_STATE_QUEUED || task.state() == wire::TASK_STATE_RUNNING)
            return;
        // Once in a process's life when it worked: the next day has its own file.
        retry_[day_] = task.state() == wire::TASK_STATE_SUCCEEDED
            ? std::chrono::steady_clock::time_point::max()
            : now + 1h;
        if (task.state() != wire::TASK_STATE_SUCCEEDED)
            log::error("contract snapshot of {} not taken in: {}", day_, task.error().message());
        task_.clear();
    }
    if (!std::filesystem::is_directory(snapshots_))
        return;
    const auto stored = store_.current();
    for (const auto& entry : std::filesystem::directory_iterator(snapshots_)) {
        const auto day = entry.path().stem().string();
        if (entry.path().extension() != ".pb" || !ended(day, wall))
            continue;
        const auto retry = retry_.find(day);
        if (retry != retry_.end() && now < retry->second)
            continue;
        const auto iso = protocol::iso_day(day);
        if (std::any_of(stored.partitions.begin(), stored.partitions.end(), [&](const auto& part) {
                return part.first.ends_with("/" + iso);
            }))
            continue;
        wire::StartAcquisition request;
        request.set_id("snapshot-" + day + "-" + unique_process_id().substr(0, 8));
        request.set_provider("ctp");
        request.set_table(wire::TABLE_PRICE_TICKS);
        request.set_first_day(iso);
        request.set_last_day(iso);
        manager_.submit(request);
        task_ = request.id();
        day_ = day;
        return;
    }
}

} // namespace asterion::market_data::acquisition
