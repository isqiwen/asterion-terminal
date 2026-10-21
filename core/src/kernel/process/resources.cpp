#include <asterion/kernel/process/resources.hpp>
#include <fstream>
#include <sstream>
#include <system_error>
#ifdef __APPLE__
#include <mach/mach.h>
#endif

namespace asterion {
namespace {

[[noreturn]] void unavailable(const char* message)
{
    throw std::system_error(std::make_error_code(std::errc::io_error), message);
}

} // namespace

HostResources HostResourceMonitor::sample(const std::filesystem::path& directory)
{
    HostResources result;
    CpuTime cpu{};
#ifdef __linux__
    std::ifstream stat("/proc/stat");
    std::string name;
    std::uint64_t user, nice, system, idle, iowait, irq, softirq, steal;
    if (!(stat >> name >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal)
        || name != "cpu")
        unavailable("cannot read host CPU counters");
    // Guest time is already included in user/nice. I/O wait is not CPU work.
    cpu.busy = user + nice + system + irq + softirq + steal;
    cpu.total = cpu.busy + idle + iowait;
    std::ifstream memory("/proc/meminfo");
    std::string line;
    bool found = false;
    while (std::getline(memory, line)) {
        if (!line.starts_with("MemAvailable:"))
            continue;
        std::istringstream input(line);
        std::uint64_t kib;
        std::string unit;
        if (!(input >> name >> kib >> unit) || unit != "kB")
            unavailable("cannot read host available memory");
        result.memory_available_bytes = kib * 1024;
        found = true;
        break;
    }
    if (!found)
        unavailable("cannot read host available memory");
#elif defined(__APPLE__)
    const auto host = mach_host_self();
    host_cpu_load_info_data_t counters{};
    auto count = HOST_CPU_LOAD_INFO_COUNT;
    const auto cpu_result =
        host_statistics(host, HOST_CPU_LOAD_INFO, reinterpret_cast<host_info_t>(&counters), &count);
    vm_statistics64_data_t memory{};
    count = HOST_VM_INFO64_COUNT;
    const auto memory_result =
        host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&memory), &count);
    vm_size_t page_size = 0;
    const auto page_result = host_page_size(host, &page_size);
    mach_port_deallocate(mach_task_self(), host);
    if (cpu_result != KERN_SUCCESS)
        unavailable("cannot read host CPU counters");
    if (memory_result != KERN_SUCCESS || page_result != KERN_SUCCESS)
        unavailable("cannot read host available memory");
    cpu.busy = std::uint64_t(counters.cpu_ticks[CPU_STATE_USER])
        + counters.cpu_ticks[CPU_STATE_NICE] + counters.cpu_ticks[CPU_STATE_SYSTEM];
    cpu.total = cpu.busy + counters.cpu_ticks[CPU_STATE_IDLE];
    // Estimate: free (already includes speculative pages) plus inactive pages.
    result.memory_available_bytes =
        (std::uint64_t(memory.free_count) + memory.inactive_count) * page_size;
#else
#error Host resource observation requires macOS or Linux
#endif
    // space().available excludes space reserved for privileged users.
    result.disk_available_bytes = std::filesystem::space(directory).available;
    if (previous_ && cpu.total > previous_->total && cpu.busy >= previous_->busy) {
        const auto total = cpu.total - previous_->total;
        const auto busy = cpu.busy - previous_->busy;
        // OS counters can reset/wrap, and Linux iowait can decrease. Rebaseline
        // that interval rather than publishing an impossible percentage.
        if (busy <= total)
            result.cpu_usage_percent = 100.0 * static_cast<double>(busy) / total;
    }
    previous_ = cpu;
    return result;
}

} // namespace asterion
