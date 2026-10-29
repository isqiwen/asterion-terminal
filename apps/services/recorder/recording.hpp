#pragma once
#include "settings.hpp"
#include "recorder.hpp"
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>

namespace asterion::recording {

// The recording loop owns the CTP feed, the catalog and the recorder on its
// own thread. Clients never reach it: they read the status it publishes and
// the files it wrote.
class Recording
{
public:
    struct Configuration
    {
        Settings settings;
        std::string instance;
    };

    struct Status
    {
        std::string phase = "disconnected";
        int error_code = 0;
        std::string catalog_phase, catalog_trading_day, catalog_error;
        std::size_t subscribed = 0;
        std::int64_t last_quote_ms = 0;
        Recorder::Status written;
        // The trading day being merged after the close, and the last failure.
        std::string merging, merge_error;
    };

    explicit Recording(Configuration config);
    ~Recording();
    Status status() const;
    void stop();
    std::shared_future<void> stopped() const;

private:
    struct Loop;
    std::unique_ptr<Loop> loop_;
};

} // namespace asterion::recording
