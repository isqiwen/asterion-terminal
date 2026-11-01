#include <asterion/system/process/resources.hpp>
#include <asterion/system/process/child.hpp>
#include <gtest/gtest.h>
#include <chrono>
#include <system_error>
#include <thread>

using namespace asterion;

TEST(HostResources, ObservesHostAndDirectoryWithoutInventingFirstCpuInterval)
{
    HostResourceMonitor monitor;
    const auto directory = std::filesystem::temp_directory_path();
    const auto first = monitor.sample(directory);
    EXPECT_FALSE(first.cpu_usage_percent);
    EXPECT_GT(first.memory_available_bytes, 0);
    EXPECT_LE(first.disk_available_bytes, std::filesystem::space(directory).capacity);
    // Host CPU counters need not advance within a fixed 50 ms sleep (macOS can
    // return the same tick sample). Wait for an observed interval, with a bound.
    auto second = first;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!second.cpu_usage_percent && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        second = monitor.sample(directory);
    }
    ASSERT_TRUE(second.cpu_usage_percent);
    EXPECT_GE(*second.cpu_usage_percent, 0);
    EXPECT_LE(*second.cpu_usage_percent, 100);
}

TEST(HostResources, MissingDirectoryFailsExplicitlyAndDoesNotPreventNextObservation)
{
    HostResourceMonitor monitor;
    const auto directory = std::filesystem::temp_directory_path();
    EXPECT_THROW(monitor.sample(directory / unique_process_id()), std::system_error);
    const auto sample = monitor.sample(directory);
    EXPECT_FALSE(sample.cpu_usage_percent);
    EXPECT_GT(sample.memory_available_bytes, 0);
}
