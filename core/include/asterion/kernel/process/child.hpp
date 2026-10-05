#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
namespace asterion {
// Owned child; no shell parsing. Close IPC before destruction for graceful exit.
class ChildProcess {
public:
  // stdout_file captures standard output; merge_stderr sends standard error
  // to the same file (for diagnostics), otherwise it is discarded.
  ChildProcess(const std::filesystem::path& executable, const std::vector<std::string>& arguments,
               bool independent = false, const std::filesystem::path& stdout_file = {},
               bool merge_stderr = false);
  ~ChildProcess();
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  bool exited();
  int exit_code();
  std::uint64_t id() const noexcept;
  bool wait(std::chrono::milliseconds timeout);
  // Ask a running child to stop without waiting; destruction still owns reaping.
  void request_stop() noexcept;
  // Relinquish ownership of an independently managed process.
  void release();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
std::filesystem::path current_executable();
std::string unique_process_id();
} // namespace asterion
