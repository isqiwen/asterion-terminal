#pragma once
#include "service_endpoint.hpp"
#include "service_io.hpp"
#include <asterion/base/serialization.hpp>

namespace asterion::terminal {

// One generated warehouse.proto Request, as ProtoJSON, on its own pinned TLS
// connection; returns the Response as ProtoJSON. Errors are thrown.
[[nodiscard]] std::future<Json> call_warehouse(ServiceIo&, ServiceEndpoint, Json);

} // namespace asterion::terminal
