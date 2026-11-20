#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>

namespace asterion {

struct HostResources
{
    std::optional<double> cpu_usage_percent;
    std::uint64_t memory_available_bytes = 0;
    std::uint64_t disk_available_bytes = 0;
};

// One serialized sampler per host observer. CPU needs two observations; memory
// describes the host, not cgroup headroom. Disk describes the supplied directory.
class HostResourceMonitor
{
public:
    HostResources sample(const std::filesystem::path& directory);

private:
    struct CpuTime
    {
        std::uint64_t busy, total;
    };

    std::optional<CpuTime> previous_;
};

} // namespace asterion
