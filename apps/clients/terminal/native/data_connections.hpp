#pragma once
#include <asterion/foundation/serialization.hpp>
#include <asterion/v1/data.pb.h>
#include <filesystem>
#include <map>
#include <optional>
namespace asterion::terminal {
struct DataConnection {
  std::string id, name, source, plugin_id, revision;
  unsigned requests_per_minute = 0;
  bool remember = false;
  std::string credential; // Only returned to native callers; never serialize into UI snapshots.
};
class DataConnections {
public:
  explicit DataConnections(std::filesystem::path directory) : directory_(std::move(directory)) {}
  // Cached until this object changes a connection or the directory's
  // modification time changes. Unreadable files are listed with an error
  // instead of failing the whole snapshot.
  Json snapshot() const;
  DataConnection get(const std::string& id) const;
  void save(DataConnection connection, const std::string& expected_revision,
            const std::string& credential_action, const data::v1::HistoryConnectionSchema& schema);
  void remove(const std::string& id, const std::string& expected_revision);

private:
  std::filesystem::path directory_;
  std::map<std::string, std::pair<std::string, std::string>> session_credentials_;
  mutable std::optional<Json> cached_;
  mutable std::filesystem::file_time_type cached_time_{};
  Json read_all() const;
  std::filesystem::path path(const std::string& id) const;
};
} // namespace asterion::terminal
