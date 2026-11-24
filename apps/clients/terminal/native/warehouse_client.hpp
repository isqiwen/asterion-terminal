#pragma once
#include "service_endpoint.hpp"
#include "service_io.hpp"
#include <asterion/base/serialization.hpp>

namespace asterion::terminal {

// One generated warehouse.proto Request, as ProtoJSON, over pinned mutual TLS;
// returns the Response as ProtoJSON. Errors are thrown.
[[nodiscard]] std::future<Json> call_warehouse(ServiceIo&, ServiceEndpoint, Json);
[[nodiscard]] std::future<std::string> call_warehouse_json(ServiceIo&, ServiceEndpoint, Json);

} // namespace asterion::terminal
