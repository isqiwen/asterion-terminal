#include "warehouse_client.hpp"
#include "service_relay.hpp"
#include <asterion/warehouse.pb.h>
#include <asterion/version.hpp>

namespace asterion::terminal {

std::future<Json> call_warehouse(ServiceIo& io, ServiceEndpoint endpoint, Json payload)
{
    return relay_call<::warehouse::Request, ::warehouse::Response>(
        io,
        std::move(endpoint),
        std::move(payload),
        [](const ::warehouse::Request&) {},
        build::warehouse_protocol);
}

} // namespace asterion::terminal
