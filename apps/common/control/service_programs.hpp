#pragma once
#include <asterion/node.pb.h>
#include <filesystem>
#include <string>

namespace asterion::terminal {

struct ServicePrograms
{
    std::filesystem::path executable, provider, catalog;
};

ServicePrograms local_service_programs(node::proto::ServiceKind kind);
ServicePrograms bundled_service_programs(const std::string& arch, node::proto::ServiceKind kind);

// Runs the installed Agent (<root>/agent/bin/asterion_node_agent) offline,
// after its service has stopped, to prepare the local programs; used by
// installation and development.
void prepare_local_programs(const std::filesystem::path& agent);

} // namespace asterion::terminal
