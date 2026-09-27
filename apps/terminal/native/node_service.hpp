#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
namespace asterion::terminal {
void install_node_service(const std::filesystem::path &executable,
                          const std::filesystem::path &root,
                          const std::string &endpoint,
                          const std::string &name = {});
// Requires the exact generated definition and the currently observed Agent PID.
// Stops only the OS registration; preserves definition, binary and all data.
void stop_node_service(const std::filesystem::path &executable,
                       const std::filesystem::path &root,
                       const std::string &endpoint, std::uint64_t expected_pid,
                       const std::string &name = {});
// Read-only recovery proof: exact definition, OS stopped state, free Agent
// lock.
void verify_node_service_stopped(const std::filesystem::path &executable,
                                 const std::filesystem::path &root,
                                 const std::string &endpoint,
                                 const std::string &name = {});
} // namespace asterion::terminal
