#include <asterion/protocol/common.hpp>
#include <asterion/protocol/quotes.hpp>

namespace asterion::protocol {

namespace {

void put(::common::Decimal* field, const Decimal& value)
{
    field->set_text(value.str());
}

::quotes::Subscription subscription(SubscriptionState state)
{
    switch (state) {
        case SubscriptionState::pending:
            return ::quotes::SUBSCRIPTION_PENDING;
        case SubscriptionState::subscribed:
            return ::quotes::SUBSCRIPTION_ACTIVE;
        case SubscriptionState::error:
            return ::quotes::SUBSCRIPTION_FAILED;
    }
    return ::quotes::SUBSCRIPTION_FAILED;
}

void encode_levels(
    const std::optional<Decimal>& best,
    std::int64_t best_lots,
    const std::array<MarketDepthLevel, 4>& rest,
    google::protobuf::RepeatedPtrField<::quotes::Level>* out)
{
    auto* first = out->Add();
    if (best) {
        put(first->mutable_price(), *best);
        first->set_lots(best_lots);
    }
    for (const auto& level : rest) {
        auto* row = out->Add();
        if (level.price)
            put(row->mutable_price(), *level.price);
        if (level.quantity)
            row->set_lots(*level.quantity);
    }
}

::quotes::Quote encode_quote(const MarketQuote& q)
{
    ::quotes::Quote quote;
    if (q.last)
        put(quote.mutable_last(), *q.last);
    if (q.open)
        put(quote.mutable_open(), *q.open);
    if (q.high)
        put(quote.mutable_high(), *q.high);
    if (q.low)
        put(quote.mutable_low(), *q.low);
    if (q.average_price)
        put(quote.mutable_average_price(), *q.average_price);
    if (q.previous_close)
        put(quote.mutable_previous_close(), *q.previous_close);
    if (q.previous_settlement)
        put(quote.mutable_previous_settlement(), *q.previous_settlement);
    if (q.upper_limit)
        put(quote.mutable_upper_limit(), *q.upper_limit);
    if (q.lower_limit)
        put(quote.mutable_lower_limit(), *q.lower_limit);
    if (q.open_interest)
        put(quote.mutable_open_interest(), *q.open_interest);
    if (q.open_interest_change)
        put(quote.mutable_open_interest_change(), *q.open_interest_change);
    quote.set_volume(q.volume);
    encode_levels(q.bid, q.bid_quantity, q.bid_levels, quote.mutable_bids());
    encode_levels(q.ask, q.ask_quantity, q.ask_levels, quote.mutable_asks());
    quote.set_trading_day(iso_day(q.trading_day));
    if (q.source_ms)
        quote.set_exchange_ms(q.source_ms);
    quote.set_received_ms(q.received_ms);
    return quote;
}

} // namespace

::common::Instrument encode_instrument(const InstrumentId& id)
{
    ::common::Instrument out;
    out.set_exchange(id.venue);
    out.set_symbol(id.symbol);
    return out;
}

InstrumentId instrument(const ::common::Instrument& id)
{
    return {id.exchange(), id.symbol()};
}

::quotes::Feed encode_feed(MarketPhase phase, int ctp_error)
{
    ::quotes::Feed feed;
    switch (phase) {
        case MarketPhase::disconnected:
            feed.set_state(::quotes::FEED_STATE_DISCONNECTED);
            break;
        case MarketPhase::connecting:
            feed.set_state(::quotes::FEED_STATE_CONNECTING);
            break;
        case MarketPhase::logging_in:
            feed.set_state(::quotes::FEED_STATE_LOGGING_IN);
            break;
        case MarketPhase::reconnecting:
            feed.set_state(::quotes::FEED_STATE_RECONNECTING);
            break;
        case MarketPhase::connected:
            feed.set_state(::quotes::FEED_STATE_CONNECTED);
            break;
        case MarketPhase::error:
            feed.set_state(::quotes::FEED_STATE_FAILED);
            break;
    }
    feed.set_ctp_error(ctp_error);
    return feed;
}

::quotes::Board encode_board(const LiveMarketSnapshot& state)
{
    ::quotes::Board board;
    *board.mutable_feed() = encode_feed(state.phase, state.error_code);
    for (const auto& s : state.subscriptions) {
        auto* row = board.add_rows();
        *row->mutable_instrument() = encode_instrument(s.instrument);
        row->set_subscription(subscription(s.state));
        row->set_ctp_error(s.error_code);
        if (s.quote)
            *row->mutable_quote() = encode_quote(*s.quote);
    }
    return board;
}

::quotes::Minutes encode_minutes(const IntradaySeries& series)
{
    ::quotes::Minutes out;
    *out.mutable_instrument() = encode_instrument(series.instrument);
    out.set_trading_day(iso_day(series.trading_day));
    if (series.previous_settlement)
        put(out.mutable_previous_settlement(), *series.previous_settlement);
    if (series.first_observation_ms)
        out.set_observed_from_ms(series.first_observation_ms);
    out.set_interrupted(series.interrupted);
    for (const auto& bar : series.bars) {
        auto* row = out.add_bars();
        row->set_start_ms(bar.start_ms);
        put(row->mutable_open(), bar.open);
        put(row->mutable_high(), bar.high);
        put(row->mutable_low(), bar.low);
        put(row->mutable_close(), bar.close);
        row->set_volume(bar.volume);
        if (bar.average_price)
            put(row->mutable_average_price(), *bar.average_price);
        if (bar.open_interest)
            put(row->mutable_open_interest(), *bar.open_interest);
        row->set_partial(bar.partial);
    }
    return out;
}

} // namespace asterion::protocol
