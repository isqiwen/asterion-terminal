#include <asterion/version.hpp>
#include "live.hpp"
#include <asterion/protocol/common.hpp>
#include <asterion/protocol/quotes.hpp>
#include <algorithm>
#include <set>

namespace asterion::quotes {

using namespace std::chrono_literals;
namespace wire = ::quotes;

namespace {

std::int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

Live::Live(const Settings& settings)
    : settings_(settings),
      started_ms_(now_ms()),
      feed_(sdk_, settings.ctp_library, settings.directory / "ctp-flow"),
      catalog_(sdk_, settings.directory / "ctp-catalog.pb", settings.directory / "catalog")
{
    login();
}

Live::~Live()
{
    feed_.stop();
}

void Live::login()
{
    feed_.connect(settings_.market, {});
    subscribed_ = 0;
    subscribed_catalog_ = 0;
    next_login_ = Clock::now() + 60s;
}

void Live::maintain(Clock::time_point now)
{
    catalog_.poll();
    if (feed_.phase() == MarketPhase::connected && now >= next_retry_) {
        feed_.retry_failed_subscriptions();
        next_retry_ = now + 5min;
    }
    if (feed_.phase() == MarketPhase::error && now >= next_login_)
        login();
    const auto catalog = catalog_.state().status().state();
    if (catalog == wire::CATALOG_STATE_FAILED && next_refresh_ == Clock::time_point{})
        next_refresh_ = now + 5min;
    if ((refresh_wanted_ || (next_refresh_ != Clock::time_point{} && now >= next_refresh_))
        && !catalog_.running()) {
        catalog_.refresh(
            settings_.ctp_catalog_library,
            settings_.directory / "ctp-catalog-flow",
            settings_.catalog);
        refresh_wanted_ = false;
        next_refresh_ = {};
    }
    // The cached catalog is used at once; after that only a finished refresh.
    if (feed_.phase() == MarketPhase::connected && subscribed_catalog_ != catalog_.revision()
        && (catalog_.state().status().state() == wire::CATALOG_STATE_READY
            || (!subscribed_catalog_ && catalog_.state().contracts_size())))
        subscribe();
}

void Live::subscribe()
{
    std::map<InstrumentId, int> multipliers;
    std::vector<InstrumentId> ids;
    std::set<std::string> symbols;
    for (const auto& row : catalog_.state().contracts()) {
        const auto id = protocol::instrument(row.instrument());
        multipliers[id] = row.multiplier();
        // CTP subscribes by symbol alone.
        if (symbols.insert(id.symbol).second)
            ids.push_back(id);
    }
    feed_.set_multipliers(std::move(multipliers));
    feed_.subscribe(ids);
    subscribed_ = static_cast<std::uint32_t>(ids.size());
    subscribed_catalog_ = catalog_.revision();
}

ctp::FeedEvents Live::poll()
{
    return feed_.poll();
}

void Live::apply(const ctp::FeedEvent& event)
{
    if (const auto* status = std::get_if<ctp::FeedStatus>(&event)) {
        feed_error_ = status->error_code;
        // Each new login refreshes the catalog; a subscription change keeps the phase.
        if (status->phase == MarketPhase::connected && phase_ != MarketPhase::connected)
            refresh_wanted_ = true;
        if (status->phase != MarketPhase::connected)
            minutes_.interrupt();
        phase_ = status->phase;
    } else if (const auto* quote = std::get_if<MarketQuoteObservation>(&event)) {
        last_quote_ms_ = quote->quote.received_ms;
        // A connection can outlast a trading day: its contracts and their
        // parameters are read for the new day too.
        if (quote->quote.trading_day > trading_day_) {
            refresh_wanted_ = refresh_wanted_ || !trading_day_.empty();
            trading_day_ = quote->quote.trading_day;
        }
        out_of_order_ += quote->out_of_order;
        if (!quote->out_of_order)
            minutes_.observe(quote->quote);
    }
}

wire::Feed Live::feed_state() const
{
    auto out = protocol::encode_feed(feed_.phase(), feed_error_);
    if (settings_.ctp_library.empty())
        out.set_state(wire::FEED_STATE_NO_SDK);
    return out;
}

void Live::status(wire::Status& out) const
{
    out.set_version(asterion::build::description);
    out.set_started_ms(started_ms_);
    *out.mutable_feed() = feed_state();
    *out.mutable_catalog() = catalog_.state().status();
    out.set_subscribed(subscribed_);
    out.set_failed_subscriptions(feed_.failed_subscriptions());
    if (last_quote_ms_)
        out.set_last_quote_ms(last_quote_ms_);
    out.set_out_of_order_quotes(out_of_order_);
}

wire::Minutes Live::minutes(const ::common::Instrument& instrument) const
{
    const auto id = protocol::instrument(instrument);
    id.validate();
    if (const auto series = minutes_.series(id))
        return protocol::encode_minutes(*series);
    wire::Minutes empty;
    *empty.mutable_instrument() = instrument;
    return empty;
}

wire::QuoteFrame Live::frame(const wire::StreamQuotes& selection, Sent& sent) const
{
    // Rows whose one-minute change moved are sent even if their quote did not.
    const auto moved =
        sent.catalog ? minutes_.changed_after(sent.minutes) : std::vector<InstrumentId>{};
    const auto state = feed_.snapshot(sent.feed, moved);
    auto board = protocol::encode_board(state);
    *board.mutable_feed() = feed_state();
    if (!selection.instruments().empty()) {
        std::set<InstrumentId> selected;
        for (const auto& id : selection.instruments())
            selected.insert(protocol::instrument(id));
        auto& rows = *board.mutable_rows();
        rows.erase(
            std::remove_if(
                rows.begin(),
                rows.end(),
                [&](const auto& row) {
                    return !selected.contains(protocol::instrument(row.instrument()));
                }),
            rows.end());
    }
    for (auto& row : *board.mutable_rows())
        if (const auto change = minutes_.change_1m_percent(protocol::instrument(row.instrument())))
            *row.mutable_change_1m_percent() = protocol::decimal(*change);
    if (!state.subscriptions_delta || sent.catalog != catalog_.revision())
        *board.mutable_catalog() = catalog_.state();
    wire::QuoteFrame frame;
    *(state.subscriptions_delta ? frame.mutable_changed() : frame.mutable_full()) =
        std::move(board);
    sent.catalog = catalog_.revision();
    sent.feed = state.sequence;
    sent.minutes = minutes_.revision();
    return frame;
}

} // namespace asterion::quotes
