#include "history_client.hpp"
#include "service_relay.hpp"
#include <asterion/history.pb.h>
#include <asterion/version.hpp>

namespace asterion::terminal {

std::future<Json> call_history(ServiceIo& io, ServiceEndpoint endpoint, Json payload)
{
    return relay_call<::history::Request, ::history::Response>(
        io,
        std::move(endpoint),
        std::move(payload),
        [](const ::history::Request&) {},
        build::history_protocol);
}

} // namespace asterion::terminal
