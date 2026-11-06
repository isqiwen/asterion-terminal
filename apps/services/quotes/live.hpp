#pragma once
#include "catalog.hpp"
#include "settings.hpp"
#include <asterion/futures/intraday_bars.hpp>
#include <asterion/quotes.pb.h>
#include <chrono>

namespace asterion::quotes {

// What one quote stream has already been sent.
struct Sent
{
    std::optional<std::uint64_t> catalog, feed;
    std::uint64_t minutes = 0;
};

// The live market: the CTP session, the contract catalog, the subscription to
// every listed contract, the latest quote of each and today's minute bars.
// Only the market thread uses it.
class Live
{
public:
    using Clock = std::chrono::steady_clock;

    explicit Live(const Settings& settings);
    ~Live();

    // Logs in again a minute after a failed login, refreshes the catalog after
    // each login, on each new trading day and five minutes after a failed
    // refresh, subscribes once the catalog lists contracts, and retries refused
    // subscriptions every five minutes.
    void maintain(Clock::time_point now);
    // The feed's changes since the last call, in order.
    ctp::FeedEvents poll();
    void apply(const ctp::FeedEvent& event);

    const IntradayBars& minutes() const
    {
        return minutes_;
    }

    void status(::quotes::Status& out) const;
    ::quotes::Minutes minutes(const ::common::Instrument& instrument) const;
    // The selected rows (all when empty): a full board on a stream's first
    // frame or after the subscriptions changed, otherwise only what changed
    // since `sent`.
    ::quotes::QuoteFrame frame(const ::quotes::StreamQuotes& selection, Sent& sent) const;

private:
    void login();
    void subscribe();
    ::quotes::Feed feed_state() const;

    const Settings& settings_;
    const std::int64_t started_ms_;
    // Every SDK call and release runs here; declared first so it outlives both users.
    ThreadPool sdk_{1, 2};
    ctp::Feed feed_;
    Catalog catalog_;
    IntradayBars minutes_;
    MarketPhase phase_ = MarketPhase::disconnected;
    int feed_error_ = 0;
    std::uint32_t subscribed_ = 0;
    std::uint64_t subscribed_catalog_ = 0;
    std::int64_t last_quote_ms_ = 0;
    std::uint64_t out_of_order_ = 0;
    bool refresh_wanted_ = false;
    std::string trading_day_;
    Clock::time_point next_login_{}, next_refresh_{}, next_retry_{};
};

} // namespace asterion::quotes
