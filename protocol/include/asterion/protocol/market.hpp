#pragma once
#include <asterion/futures/intraday_bars.hpp>
#include <asterion/futures/live_market.hpp>
#include <asterion/base/serialization.hpp>
#include <asterion/market.pb.h>

namespace asterion::protocol {

market::proto::Snapshot encode_market(const LiveMarketSnapshot& state, const std::string& instance);
market::proto::EventBatch encode_market_events(const MarketEventBatch& batch);
Json decode_market(const market::proto::Snapshot& state);
market::proto::MinuteSeries encode_minutes(const IntradaySeries& series);
Json decode_minutes(const market::proto::MinuteSeries& series);

} // namespace asterion::protocol
