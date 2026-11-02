#pragma once
#include <asterion/base/serialization.hpp>
#include <future>
#include "trading_connections.hpp"
#include "service_io.hpp"

namespace asterion::terminal {

// One generated trading.proto Request, as ProtoJSON, on its own pinned TLS
// connection; returns the Response as ProtoJSON. Errors are thrown.
[[nodiscard]] std::future<Json> call_trading(ServiceIo&, TradingEndpoint, Json);

} // namespace asterion::terminal
