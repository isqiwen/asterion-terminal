#pragma once
#include <asterion/v1/node.pb.h>
#include <filesystem>
#include <string>
namespace asterion::terminal {
struct ServicePrograms {
  std::filesystem::path executable, provider, worker, factor, data, catalog;
};
ServicePrograms local_service_programs(node::v1::ServiceKind kind);
ServicePrograms bundled_service_programs(const std::string& arch, node::v1::ServiceKind kind);
} // namespace asterion::terminal
