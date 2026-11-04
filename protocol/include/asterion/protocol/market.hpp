#pragma once
#include <asterion/futures/intraday_bars.hpp>
#include <asterion/futures/live_market.hpp>
#include <asterion/market.pb.h>

namespace asterion::protocol {

::market::Feed encode_feed(MarketPhase phase, int ctp_error);
// The feed and its rows; the caller adds the catalog.
::market::Board encode_board(const LiveMarketSnapshot& state);
::market::Minutes encode_minutes(const IntradaySeries& series);
::common::Instrument encode_instrument(const InstrumentId& id);
InstrumentId instrument(const ::common::Instrument& id);
// The warehouse's names: calendar, contracts, daily, minutes, settlement, observations.
std::string table_name(::market::Table table);
::market::Table table_of(std::string_view name);

} // namespace asterion::protocol
