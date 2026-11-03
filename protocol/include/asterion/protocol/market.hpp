#pragma once
#include <asterion/futures/intraday_bars.hpp>
#include <asterion/futures/live_market.hpp>
#include <asterion/base/serialization.hpp>
#include <asterion/market.pb.h>

namespace asterion::protocol {

market::proto::Snapshot encode_market(const LiveMarketSnapshot& state, const std::string& instance);
market::proto::EventBatch encode_market_events(const MarketEventBatch& batch);
market::proto::MinuteSeries encode_minutes(const IntradaySeries& series);

} // namespace asterion::protocol
