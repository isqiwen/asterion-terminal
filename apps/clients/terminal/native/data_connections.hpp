#pragma once
#include <asterion/foundation/serialization.hpp>
#include <asterion/v1/data.pb.h>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
namespace asterion::terminal {
struct DataConnection {
  std::string id, name, source, plugin_id, revision;
  unsigned requests_per_minute = 0;
  bool remember = false;
  std::string credential; // Only returned to native callers; never serialize into UI snapshots.
};
// Where remembered credentials live. Never a connection file.
class CredentialStore {
public:
  virtual ~CredentialStore() = default;
  virtual std::optional<std::string> load(const std::string& account) = 0;
  virtual void store(const std::string& account, const std::string& secret) = 0;
  virtual void erase(const std::string& account) = 0;
};
// The login keychain through the asterion-keychain helper; null when the
// helper is unavailable, in which case credentials cannot be remembered.
std::shared_ptr<CredentialStore> keychain_store(const std::filesystem::path& helper);
class DataConnections {
public:
  DataConnections(std::filesystem::path directory, std::shared_ptr<CredentialStore> remembered)
      : directory_(std::move(directory)), remembered_(std::move(remembered)) {}
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
  std::shared_ptr<CredentialStore> remembered_;
  std::string account(const std::string& id) const;
  std::map<std::string, std::pair<std::string, std::string>> session_credentials_;
  mutable std::optional<Json> cached_;
  mutable std::filesystem::file_time_type cached_time_{};
  Json read_all() const;
  std::filesystem::path path(const std::string& id) const;
};
} // namespace asterion::terminal
