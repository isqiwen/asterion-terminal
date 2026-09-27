#pragma once
#include <asterion/domain/live_market.hpp>
#include <asterion/foundation/serialization.hpp>
#include <asterion/v1/market.pb.h>
namespace asterion::protocol {
market::v1::Snapshot encode_market(const LiveMarketSnapshot& state, const std::string& instance);
market::v1::EventBatch encode_market_events(const MarketEventBatch& batch);
Json decode_market(const market::v1::Snapshot& state);
} // namespace asterion::protocol
