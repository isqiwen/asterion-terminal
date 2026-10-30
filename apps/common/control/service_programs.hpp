#pragma once
#include <asterion/node.pb.h>
#include <filesystem>
#include <string>

namespace asterion::terminal {

struct ServicePrograms
{
    std::filesystem::path executable, provider, catalog;
};

// The kind's program and CTP libraries inside the application (or as named by
// the ASTERION_*_EXECUTABLE / ASTERION_CTP_* variables in development and tests).
ServicePrograms local_service_programs(node::proto::ServiceKind kind);

} // namespace asterion::terminal
