#pragma once
#include <asterion/foundation/serialization.hpp>
#include <filesystem>
#include <vector>
#include "service_endpoint.hpp"
namespace asterion::terminal {
struct ReplayOwner {
  std::filesystem::path directory;
  std::string session, endpoint;
};
struct RemoteReplayOwner {
  std::string directory, state;
  ServiceEndpoint address;
};
// Directory is empty only for a direct connection whose server owns its path.
Json remote_replay_usage(const std::vector<RemoteReplayOwner>& owners,
                         const std::string& dataset_id);
// Only named local accounts under this environment's accounts/paper directory.
// Running owners use a bounded read-only RPC; other ledgers use SQLite.
// Failures remain explicitly unchecked, never an empty result.
Json local_replay_usage(const std::filesystem::path& node, const std::string& dataset_id,
                        const std::vector<ReplayOwner>& owners = {});
} // namespace asterion::terminal
