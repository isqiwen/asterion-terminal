#pragma once
#include "service_endpoint.hpp"
#include <asterion/node.pb.h>

namespace asterion::terminal {

struct HistoryService
{
    node::proto::ServiceKind kind;
    std::string directory, state, data_service;
    ServiceEndpoint address;
};

struct DataTaskEndpoints
{
    ServiceEndpoint task, data;
};

} // namespace asterion::terminal
