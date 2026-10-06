#pragma once
#include <asterion/kernel/environment.hpp>
#include <asterion/kernel/process/capacity.hpp>
#include <asterion/v1/node.pb.h>
#include <algorithm>
#include <sstream>
#include <stdexcept>
namespace asterion::agent {
// The capacity admission is computed from. A test node states its own as
// "<cpu threads>,<memory MiB>", so that what a test may start does not depend
// on the machine running it; every other node reads the host.
inline HostCapacity node_capacity() {
  const auto stated = environment_variable("ASTERION_TEST_HOST_CAPACITY");
  if (!stated)
    return host_capacity();
  std::istringstream input(*stated);
  unsigned cpu = 0;
  std::uint64_t memory_mib = 0;
  char separator = 0;
  if (!(input >> cpu >> separator >> memory_mib) || separator != ',' || !cpu || !memory_mib ||
      input.peek() != std::char_traits<char>::eof())
    throw std::invalid_argument("invalid test host capacity");
  return {cpu, memory_mib * 1024 * 1024};
}
// Admission reservations, not CPU affinity, an RSS ceiling or disk bandwidth.
struct Resources {
  unsigned cpu = 0;
  std::uint64_t memory_mib = 0;
  unsigned io = 0;
  Resources& operator+=(const Resources& other) {
    cpu += other.cpu;
    memory_mib += other.memory_mib;
    io += other.io;
    return *this;
  }
};
class ResourceBudget {
public:
  ResourceBudget(HostCapacity host, bool terminal_node) {
    const unsigned reserved_cpu = terminal_node ? 2 : 1;
    const auto memory = host.memory_bytes / (1024 * 1024);
    const auto reserved_memory = std::max<std::uint64_t>(2048, memory / 4);
    limit = {host.cpu_threads > reserved_cpu ? host.cpu_threads - reserved_cpu : 0,
             memory > reserved_memory ? memory - reserved_memory : 0, 10};
    file_workers = host.cpu_threads >= 10 ? 2 : 1;
  }
  Resources service(node::v1::ServiceKind kind) const {
    switch (kind) {
    case node::v1::DATA_SERVICE:
      return {file_workers, 1024, file_workers + 1};
    case node::v1::TASK_SERVICE:
      return {file_workers, 2048, file_workers + 1};
    case node::v1::MARKET_DATA:
    case node::v1::LIVE_TRADING:
      return {1, 512, 1};
    default:
      throw std::invalid_argument("unsupported service resource profile");
    }
  }
  static Resources workers(unsigned count) { return {count, 1024ULL * count, count}; }
  bool fits(Resources used, const Resources& extra = {}) const {
    used += extra;
    return used.cpu <= limit.cpu && used.memory_mib <= limit.memory_mib && used.io <= limit.io;
  }
  unsigned worker_slots(const Resources& used, unsigned maximum) const {
    if (!fits(used))
      return 0;
    return static_cast<unsigned>(
        std::min<std::uint64_t>({maximum, limit.cpu - used.cpu,
                                 (limit.memory_mib - used.memory_mib) / 1024, limit.io - used.io}));
  }
  Resources limit;
  unsigned file_workers;
};
} // namespace asterion::agent
