#pragma once
#include <asterion/foundation/serialization.hpp>
#include <filesystem>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
namespace asterion::terminal {
// What one data provider (a data source plugin) was given: a single
// credential used by all of its sources, and the local request-rate preference.
// Applying a shared budget to a Data instance is a separate explicit command.
struct DataCredential {
  std::string provider;
  unsigned requests_per_minute = 0;
  bool remember = false;
  std::string credential; // Only returned to native callers; never serialize into UI snapshots.
};
// What the provider's sources accept.
struct DataCredentialLimits {
  bool credential_required = false;
  std::size_t credential_max_length = 0;
  bool remember_allowed = false;
  unsigned requests_per_minute_max = 0;
};
// Where remembered credentials live. Never a settings file.
class CredentialStore {
public:
  virtual ~CredentialStore() = default;
  virtual std::optional<std::string> load(const std::string& account) = 0;
  virtual void store(const std::string& account, const std::string& secret) = 0;
  virtual void erase(const std::string& account) = 0;
};
// The login keychain through the asterion-keychain helper; null when the
// helper is unavailable, in which case credentials cannot be remembered.
// Pipe I/O and helper completion share a deadline of at most 30 seconds.
std::shared_ptr<CredentialStore>
keychain_store(const std::filesystem::path& helper,
               std::chrono::milliseconds timeout = std::chrono::seconds(30));
class DataCredentials {
public:
  DataCredentials(std::filesystem::path directory, std::shared_ptr<CredentialStore> remembered)
      : directory_(std::move(directory)), remembered_(std::move(remembered)) {}
  // Cached until this object changes an entry or the directory's modification
  // time changes. Unreadable files are listed with an error instead of
  // failing the whole snapshot.
  Json snapshot() const;
  // Empty when nothing was saved for the provider.
  std::optional<DataCredential> find(const std::string& provider) const;
  // An empty credential keeps the one already held.
  void save(DataCredential value, const DataCredentialLimits& limits);
  void clear(const std::string& provider);

private:
  std::filesystem::path directory_;
  std::shared_ptr<CredentialStore> remembered_;
  std::string account(const std::string& provider) const;
  // Credentials that are not remembered last until the Terminal exits.
  std::map<std::string, std::string> session_;
  mutable std::optional<Json> cached_;
  mutable std::filesystem::file_time_type cached_time_{};
  Json read_all() const;
  std::filesystem::path path(const std::string& provider) const;
};
} // namespace asterion::terminal
