#pragma once
#include <asterion/foundation/serialization.hpp>
#include <filesystem>
#include <optional>
namespace asterion::terminal {
// One counter account as the broker issues it; passwords and authorization
// codes are never part of it. Several may be stored, one is current: the
// market login, the contract catalog query and new trading accounts use it.
struct CtpConnection {
  std::string id, name, broker_id, user_id, app_id, trade_front, market_front, revision;
};
class CtpConnections {
public:
  explicit CtpConnections(std::filesystem::path directory) : directory_(std::move(directory)) {}
  // Cached until this object changes a connection or the directory's
  // modification time changes. Unreadable files are listed with an error.
  Json snapshot() const;
  CtpConnection get(const std::string& id) const;
  void save(CtpConnection connection, const std::string& expected_revision);
  void remove(const std::string& id, const std::string& expected_revision);
  // The current account, or nothing when none is selected or it was removed.
  std::optional<CtpConnection> current() const;
  void select(const std::string& id);

private:
  std::filesystem::path directory_;
  mutable std::optional<Json> cached_;
  mutable std::filesystem::file_time_type cached_time_{};
  Json read_all() const;
  std::filesystem::path path(const std::string& id) const;
};
} // namespace asterion::terminal
