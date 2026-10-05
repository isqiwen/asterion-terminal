#include <asterion/kernel/process/capacity.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <sched.h>
#endif
namespace asterion {
HostCapacity host_capacity() {
  const auto processors = ::sysconf(_SC_NPROCESSORS_ONLN);
  if (processors < 1)
    throw std::runtime_error("cannot determine node CPU capacity");
  HostCapacity result{static_cast<unsigned>(processors), 0};
#ifdef __APPLE__
  auto size = sizeof(result.memory_bytes);
  if (::sysctlbyname("hw.memsize", &result.memory_bytes, &size, nullptr, 0) != 0)
    throw std::runtime_error("cannot determine node memory capacity");
#elif defined(__linux__)
  const auto pages = ::sysconf(_SC_PHYS_PAGES), page_size = ::sysconf(_SC_PAGESIZE);
  if (pages < 1 || page_size < 1)
    throw std::runtime_error("cannot determine node memory capacity");
  result.memory_bytes = static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(page_size);
  cpu_set_t affinity;
  CPU_ZERO(&affinity);
  if (::sched_getaffinity(0, sizeof(affinity), &affinity) != 0)
    throw std::runtime_error("cannot determine node CPU affinity");
  result.cpu_threads = std::min(result.cpu_threads, static_cast<unsigned>(CPU_COUNT(&affinity)));
  std::ifstream groups("/proc/self/cgroup");
  if (!groups)
    throw std::runtime_error("cannot inspect node cgroup capacity");
  std::string line;
  const std::filesystem::path root("/sys/fs/cgroup");
  while (std::getline(groups, line)) {
    if (!line.starts_with("0::"))
      throw std::runtime_error("node resource detection requires unified cgroups");
    auto directory =
        (root / std::filesystem::path(line.substr(3)).relative_path()).lexically_normal();
    const auto relative = directory.lexically_relative(root);
    if (relative.empty() || *relative.begin() == ".." || !std::filesystem::is_directory(directory))
      throw std::runtime_error("cannot inspect node cgroup capacity");
    // A parent slice can impose a tighter limit than the process's own group.
    for (;;) {
      std::string quota;
      if (std::filesystem::exists(directory / "memory.max")) {
        std::ifstream memory(directory / "memory.max");
        if (!(memory >> quota))
          throw std::runtime_error("cannot inspect node cgroup capacity");
        if (quota != "max")
          result.memory_bytes = std::min(result.memory_bytes, std::uint64_t(std::stoull(quota)));
      }
      if (std::filesystem::exists(directory / "cpu.max")) {
        std::ifstream cpu(directory / "cpu.max");
        std::uint64_t period = 0;
        if (!(cpu >> quota >> period) || !period)
          throw std::runtime_error("cannot inspect node cgroup capacity");
        if (quota != "max")
          result.cpu_threads = static_cast<unsigned>(
              std::min<std::uint64_t>(result.cpu_threads, std::stoull(quota) / period));
      }
      if (directory == root)
        break;
      directory = directory.parent_path();
    }
  }
#else
#error Node resource detection requires macOS or Linux
#endif
  return result;
}
} // namespace asterion
