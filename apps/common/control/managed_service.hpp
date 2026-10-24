#pragma once
#include "service_endpoint.hpp"
#include <asterion/node.pb.h>

namespace asterion::terminal {

struct HistoryService
{
    node::proto::ServiceKind kind;
    std::string directory, state;
    ServiceEndpoint address;
};

} // namespace asterion::terminal
