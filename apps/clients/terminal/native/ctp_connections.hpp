#pragma once
#include <asterion/foundation/serialization.hpp>
#include <filesystem>
#include <optional>
#include "data_credentials.hpp"
namespace asterion::terminal {
// One counter account as the broker issues it; passwords and authorization
// codes are never part of it. Several may be stored and traded at the same
// time; exactly one of them is the source of market data.
struct CtpConnection {
  std::string id, name, broker_id, user_id, app_id, trade_front, market_front, revision;
};
struct MarketCredentials {
  std::string password, auth_code;
};
class CtpConnections {
public:
  explicit CtpConnections(std::filesystem::path directory,
                          std::shared_ptr<CredentialStore> remembered = {})
      : directory_(std::move(directory)), remembered_(std::move(remembered)) {}
  // Market login only. Secrets never enter account files or UI snapshots.
  MarketCredentials market_credentials(const CtpConnection& connection) const;
  void remember_market_credentials(const std::string& id, const MarketCredentials& credentials);
  void forget_market_credentials(const std::string& id);
  // Cached, once the directory has settled, until this object changes a
  // connection or the directory's modification time changes. Unreadable files
  // are listed with an error.
  Json snapshot() const;
  CtpConnection get(const std::string& id) const;
  void save(CtpConnection connection, const std::string& expected_revision);
  void remove(const std::string& id, const std::string& expected_revision);
  // The market data account, or nothing when none is selected or it was removed.
  std::optional<CtpConnection> market() const;
  void select_market(const std::string& id);

private:
  std::filesystem::path directory_;
  std::shared_ptr<CredentialStore> remembered_;
  std::string credential_account(const CtpConnection& connection) const;
  mutable std::optional<Json> cached_;
  mutable std::filesystem::file_time_type cached_time_{};
  Json read_all() const;
  std::filesystem::path path(const std::string& id) const;
};
} // namespace asterion::terminal
