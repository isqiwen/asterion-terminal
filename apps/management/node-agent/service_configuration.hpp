#pragma once
#include <asterion/node.pb.h>
#include <filesystem>
#include <string>
#include <vector>

namespace asterion::agent {

struct ServiceConfiguration
{
    node::proto::ServiceKind kind = node::proto::UNSPECIFIED_SERVICE;
    std::string artifact, provider_artifact, catalog_artifact;
    // The lab service's interpreter. It runs in place inside the bundled Python
    // runtime, which it needs beside it; `artifact` is the interpreter's digest.
    std::string python;
    std::vector<std::string> plugin_artifacts;
    std::string directory;
    unsigned short port = 0;
    bool desired = true;

    bool operator==(const ServiceConfiguration&) const = default;
};

// Each kind of service is one program, installed in <root>/<program>:
// bin/<executable>, lib/ (CTP libraries, plugins/<service>/), config/,
// logs/ and data/<service>/.
std::string program_name(node::proto::ServiceKind kind);
std::string executable_name(node::proto::ServiceKind kind);

std::string service_revision(const ServiceConfiguration& configuration);
void save_service_configuration(
    const std::filesystem::path& folder,
    const ServiceConfiguration& configuration);
ServiceConfiguration load_service_configuration(
    const std::filesystem::path& folder,
    bool local,
    unsigned short control_port);

} // namespace asterion::agent
