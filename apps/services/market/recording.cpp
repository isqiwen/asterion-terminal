#include "recording.hpp"
#include <asterion/protocol/common.hpp>

namespace asterion::market_data {

namespace {

std::int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

Recording::Recording(
    const std::filesystem::path& directory,
    const std::string& instance,
    const IntradayBars& minutes)
    : directory_(directory),
      minutes_(minutes),
      recorder_(directory / "recordings", instance)
{}

Recording::~Recording()
{
    try {
        recorder_.close("stopped", now_ms(), minutes_);
    } catch (...) {
        // The writer reports the failure; provider teardown must finish.
    }
}

void Recording::observe(const ctp::FeedEvent& event)
{
    if (const auto* status = std::get_if<ctp::FeedStatus>(&event)) {
        if (status->phase != MarketPhase::connected)
            recorder_.interrupt(market_phase_name(status->phase));
    } else if (const auto* quote = std::get_if<MarketQuoteObservation>(&event)) {
        recorder_.observe(quote->quote, quote->out_of_order, minutes_);
    }
}

void Recording::interrupt(std::string_view reason)
{
    recorder_.interrupt(reason);
}

void Recording::maintain()
{
    recorder_.poll(now_ms(), minutes_);
}

void Recording::status(::market::Recording& out) const
{
    const auto& written = recorder_.status();
    out.set_trading_day(protocol::iso_day(written.trading_day));
    out.set_rows(written.rows);
    out.set_files(written.files);
    out.set_bytes(written.bytes);
    out.set_lost_rows(written.lost);
    if (written.written_ms)
        out.set_written_ms(written.written_ms);
    out.set_disk_free_bytes(std::filesystem::space(directory_).available);
    out.set_write_error(written.error);
}

} // namespace asterion::market_data
