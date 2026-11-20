#pragma once
#include <asterion/base/serialization.hpp>
#include <future>
#include "trading_connections.hpp"
#include "service_io.hpp"

namespace asterion::terminal {

inline bool urgent_trading_request(const Json& payload)
{
    return payload.contains("sendCommand") || payload.contains("pauseStrategy")
        || payload.contains("stopStrategy") || payload.contains("flattenStrategy")
        || payload.contains("getEmergency") || payload.contains("emergencyControl");
}

// One generated trading.proto Request, as ProtoJSON, on its own pinned TLS
// connection; returns the Response as ProtoJSON. Errors are thrown.
[[nodiscard]] std::future<Json> call_trading(ServiceIo&, TradingEndpoint, Json);

} // namespace asterion::terminal
