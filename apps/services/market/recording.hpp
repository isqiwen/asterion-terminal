#pragma once
#include "recording/recorder.hpp"
#include "ctp_feed.hpp"
#include <asterion/market.pb.h>

namespace asterion::market_data {

// Everything the feed delivers, kept: the recorder writes each quote and
// minute bar to files, which is all the history side ever sees of it. Only the
// market thread uses it.
class Recording
{
public:
    Recording(
        const std::filesystem::path& directory,
        const std::string& instance,
        const IntradayBars& minutes);
    // Ends the current segment and writes what is buffered.
    ~Recording();

    // Called before the minute bars take the same event, so the first quote of
    // a new trading day still sees the previous day's bars.
    void observe(const ctp::FeedEvent& event);
    void interrupt(std::string_view reason);
    // Writes due files.
    void maintain();
    void status(::market::Recording& out) const;

private:
    const std::filesystem::path directory_;
    const IntradayBars& minutes_;
    recording::Recorder recorder_;
};

} // namespace asterion::market_data
