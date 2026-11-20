#include "composer.hpp"
#include <algorithm>
#include <asterion/system/log.hpp>

namespace asterion::warehouse {

using namespace std::chrono_literals;

Composer::Composer(std::filesystem::path data)
    : data_(std::move(data)),
      research_(source_directory(data_, "research"), "research"),
      versions_(source_versions(data_))
{
    const bool any_source = std::any_of(versions_.begin(), versions_.end(), [](const auto& entry) {
        return !entry.second.empty();
    });
    if (any_source)
        try {
            compose(data_, research_, versions_);
            requested_ = false;
        } catch (const std::exception& error) {
            log::error("warehouse research dataset not composed: {}", error.what());
        }
    else
        requested_ = false;
    thread_ = std::jthread([this](std::stop_token stop) {
        run(stop);
    });
}

void Composer::request(const SourceVersions& committed)
{
    {
        std::lock_guard lock(mutex_);
        for (const auto& [provider, version] : committed)
            versions_[provider] = version;
        requested_ = true;
    }
    changed_.notify_all();
}

void Composer::stop()
{
    thread_.request_stop();
    if (thread_.joinable())
        thread_.join();
}

void Composer::run(std::stop_token stop)
{
    while (!stop.stop_requested()) {
        SourceVersions versions;
        {
            std::unique_lock lock(mutex_);
            if (!changed_.wait(lock, stop, [this] {
                    return requested_;
                }))
                return;
            requested_ = false;
            versions = versions_;
        }
        try {
            compose(data_, research_, versions);
        } catch (const std::exception& error) {
            // The sources are intact; a damaged or full disk may recover.
            log::error("warehouse research dataset not composed: {}", error.what());
            std::mutex pause;
            std::unique_lock lock(pause);
            changed_.wait_for(lock, stop, 60s, [] {
                return false;
            });
            request({});
        }
    }
}

} // namespace asterion::warehouse
