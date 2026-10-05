#pragma once
#include <asterion/foundation/serialization.hpp>
#include <filesystem>
#include <asterion/kernel/progress.hpp>
#include <future>
#include <memory>
#include <string>
#include <string_view>
namespace asterion::trading {
// Thread-safe entry point for one account. Accepted operations run in order on
// its account thread; shutdown drains them before destroying account resources.
// Callers authenticate and validate the request envelope before entry. Admission
// fixes each command's control revision; revoke/disconnect close send permission
// before the account processes their state changes.
class LiveSession final {
public:
  LiveSession(std::filesystem::path directory, const std::filesystem::path& ctp_library,
              const std::filesystem::path& ownership_directory,
              const Json& create_manifest = nullptr);
  ~LiveSession();
  LiveSession(const LiveSession&) = delete;
  LiveSession& operator=(const LiveSession&) = delete;
  // Initialization runs on the account owner. Await it before submitting operations.
  std::shared_future<void> initialized() const;
  // Quiesce mutation admission and invalidate sends; accepted replies may still read.
  void close_admission();
  // After reply drain, stop all resources on their owners. Completion includes SDK release.
  void stop();
  std::shared_future<void> stopped() const;
  std::future<void> connect(std::string password, std::string auth_code);
  std::future<void> disconnect();
  std::future<void> query_costs();
  // Admission is bounded; completion acknowledges the durable command outcome.
  [[nodiscard]] std::future<void> execute(std::string_view account_id,
                                          std::string_view policy_revision, const Json& command);
  std::future<Json> snapshot() const;
  bool recovery_required() const noexcept;
  struct Health {
    Progress::Observation state, persistence, command;
    bool business_ready;
  };
  Health health() const noexcept;

private:
  struct Loop;
  std::unique_ptr<Loop> loop_;
};
} // namespace asterion::trading
