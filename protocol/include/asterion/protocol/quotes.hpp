#pragma once
#include <asterion/futures/intraday_bars.hpp>
#include <asterion/futures/live_market.hpp>
#include <asterion/quotes.pb.h>

namespace asterion::protocol {

::quotes::Feed encode_feed(MarketPhase phase, int ctp_error);
// The feed and its rows; the caller adds the catalog.
::quotes::Board encode_board(const LiveMarketSnapshot& state);
::quotes::Minutes encode_minutes(const IntradaySeries& series);
::common::Instrument encode_instrument(const InstrumentId& id);
InstrumentId instrument(const ::common::Instrument& id);

} // namespace asterion::protocol
