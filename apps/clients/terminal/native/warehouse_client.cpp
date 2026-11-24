#include "warehouse_client.hpp"
#include "service_relay.hpp"
#include <asterion/warehouse.pb.h>
#include <asterion/version.hpp>

namespace asterion::terminal {
namespace {

bool read_only(const ::warehouse::Request& request)
{
    switch (request.call_case()) {
        case ::warehouse::Request::kStartAcquisition:
        case ::warehouse::Request::kCancelAcquisition:
        case ::warehouse::Request::kStartUpdate:
            return false;
        default:
            return true;
    }
}

} // namespace

std::future<std::string> call_warehouse_json(ServiceIo& io, ServiceEndpoint endpoint, Json payload)
{
    return relay_call_json<::warehouse::Request, ::warehouse::Response>(
        io,
        std::move(endpoint),
        std::move(payload),
        [](const ::warehouse::Request&) {},
        read_only,
        build::warehouse_protocol);
}

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
