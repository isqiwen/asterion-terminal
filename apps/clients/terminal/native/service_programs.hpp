#pragma once
#include <asterion/node.pb.h>
#include <filesystem>
#include <string>

namespace asterion::terminal {

struct ServicePrograms
{
    std::filesystem::path executable, provider, worker, factor, data, catalog;
};

ServicePrograms local_service_programs(node::proto::ServiceKind kind);
ServicePrograms bundled_service_programs(const std::string& arch, node::proto::ServiceKind kind);

} // namespace asterion::terminal
