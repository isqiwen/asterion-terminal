#pragma once
#include <asterion/market.pb.h>
#include <stop_token>

namespace asterion::data {

market::proto::ObservationCapture fetch_market_observation(
    const resource::proto::Object&,
    const std::string& node,
    const std::string& discovery,
    std::stop_token);

} // namespace asterion::data
