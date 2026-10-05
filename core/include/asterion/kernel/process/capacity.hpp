#pragma once
#include <cstdint>
namespace asterion {
struct HostCapacity {
  unsigned cpu_threads = 0;
  std::uint64_t memory_bytes = 0;
};
// Startup observation, including Linux affinity and unified cgroup limits.
// This reports capacity, not current free memory or an OS resource reservation.
HostCapacity host_capacity();
} // namespace asterion
