#pragma once
#include "acquisition/manager.hpp"
#include "acquisition/recorded_publication.hpp"
#include "recording/compactor.hpp"
#include "recording/recorder.hpp"
#include "ctp_feed.hpp"
#include <asterion/market.pb.h>

namespace asterion::market_data {

// Everything the feed delivers, kept: the recorder writes each quote and
// minute bar, a quiet market's finished trading days are merged, and each
// merged day is published to research. Only the market thread uses it.
class Recording
{
public:
    using Clock = std::chrono::steady_clock;

    Recording(
        const std::filesystem::path& directory,
        const std::string& instance,
        unsigned tick_days,
        std::shared_ptr<std::shared_mutex> archive,
        acquisition::Manager& acquisitions,
        const IntradayBars& minutes);
    // Ends the current segment and writes what is buffered.
    ~Recording();

    // Called before the minute bars take the same event, so the first quote of
    // a new trading day still sees the previous day's bars.
    void observe(const ctp::FeedEvent& event);
    void interrupt(std::string_view reason);
    // Writes due files, merges a finished day once quotes stopped for 30
    // minutes, and publishes merged days.
    void maintain(Clock::time_point now);
    void status(::market::Recording& out) const;

private:
    const std::filesystem::path directory_;
    const IntradayBars& minutes_;
    recording::Recorder recorder_;
    recording::Compactor compactor_;
    // A merge after late quotes publishes the day again.
    acquisition::RecordedPublication publication_;
    Clock::time_point quiet_since_ = Clock::now();
};

} // namespace asterion::market_data
