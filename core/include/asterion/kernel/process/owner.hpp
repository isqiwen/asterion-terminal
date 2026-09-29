#pragma once
#include <cstdint>
#include <memory>
namespace asterion {
std::uint64_t current_process_id() noexcept;
// Read-only liveness probe. Reused PIDs conservatively remain live; never signals a process.
bool process_running(std::uint64_t pid);
// A managed child must stop if its supervising process disappears.
class ProcessOwner {
public:
  explicit ProcessOwner(std::uint64_t pid);
  ~ProcessOwner();
  bool alive() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace asterion
