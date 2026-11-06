#include "composer.hpp"
#include "research.hpp"
#include <asterion/system/log.hpp>

namespace asterion::history::warehouse {

using namespace std::chrono_literals;

Composer::Composer(std::filesystem::path data)
    : data_(std::move(data)),
      research_(source_directory(data_, "research"), "research"),
      thread_([this](std::stop_token stop) {
          run(stop);
      })
{}

void Composer::request()
{
    {
        std::lock_guard lock(mutex_);
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
    for (;;) {
        {
            std::unique_lock lock(mutex_);
            if (!changed_.wait(lock, stop, [this] {
                    return requested_;
                }))
                return;
            requested_ = false;
        }
        try {
            compose_research(data_, research_);
        } catch (const std::exception& error) {
            // The sources are intact; a damaged or full disk may recover.
            log::error("market research dataset not composed: {}", error.what());
            std::mutex pause;
            std::unique_lock lock(pause);
            changed_.wait_for(lock, stop, 60s, [] {
                return false;
            });
            request();
        }
    }
}

} // namespace asterion::history::warehouse
