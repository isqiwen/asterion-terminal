#include "recording.hpp"
#include <asterion/protocol/common.hpp>

namespace asterion::market_data {

using namespace std::chrono_literals;

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
    unsigned tick_days,
    std::shared_ptr<std::shared_mutex> archive,
    acquisition::Manager& acquisitions,
    const IntradayBars& minutes)
    : directory_(directory),
      minutes_(minutes),
      recorder_(directory / "recordings", instance),
      compactor_(directory / "recordings", std::move(archive), tick_days),
      publication_(directory / "recordings" / "unpublished.json", acquisitions)
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
        quiet_since_ = Clock::now();
        recorder_.observe(quote->quote, quote->out_of_order, minutes_);
    }
}

void Recording::interrupt(std::string_view reason)
{
    recorder_.interrupt(reason);
}

void Recording::maintain(Clock::time_point now)
{
    recorder_.poll(now_ms(), minutes_);
    if (const auto day =
            compactor_.poll(now - quiet_since_ >= 30min, now, std::chrono::system_clock::now()))
        publication_.add(*day);
    publication_.poll(now);
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
    const auto merging = compactor_.status();
    out.set_merging_day(protocol::iso_day(merging.day));
    out.set_merge_error(merging.error);
}

} // namespace asterion::market_data
