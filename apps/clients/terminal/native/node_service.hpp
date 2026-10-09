#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
namespace asterion::terminal {
// The definition this user's service manager reads: a launchd agent on macOS,
// a systemd user unit on Linux. An empty name is the installed Terminal's.
std::filesystem::path node_service_definition(const std::string& name = {});
void install_node_service(const std::filesystem::path& executable,
                          const std::filesystem::path& root, const std::string& endpoint,
                          const std::string& name = {});
// Requires the exact generated definition and the currently observed Agent PID.
// Stops only the OS registration; preserves definition, binary and all data.
void stop_node_service(const std::filesystem::path& executable, const std::filesystem::path& root,
                       const std::string& endpoint, std::uint64_t expected_pid,
                       const std::string& name = {});
// Read-only recovery proof: exact definition, OS stopped state, free Agent
// lock.
void verify_node_service_stopped(const std::filesystem::path& executable,
                                 const std::filesystem::path& root, const std::string& endpoint,
                                 const std::string& name = {});
// Ends the login registration of a service that stop or verify proved stopped.
void remove_node_service(const std::string& name = {});
} // namespace asterion::terminal
