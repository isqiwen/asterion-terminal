#include "systemd_watchdog.hpp"
#include <asterion/system/process/child.hpp>
#include <gtest/gtest.h>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace asterion;
using namespace asterion::trading;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

class ScopedEnv
{
public:
    ScopedEnv(const char* name, std::optional<std::string> value)
        : name_(name)
    {
        if (const char* current = std::getenv(name))
            previous_ = current;
        if (value)
            ::setenv(name, value->c_str(), 1);
        else
            ::unsetenv(name);
    }

    ~ScopedEnv()
    {
        if (previous_)
            ::setenv(name_.c_str(), previous_->c_str(), 1);
        else
            ::unsetenv(name_.c_str());
    }

private:
    std::string name_;
    std::optional<std::string> previous_;
};

class NotifySocket
{
public:
    NotifySocket()
    {
        path_ = fs::temp_directory_path() / ("ast-wd-" + unique_process_id() + ".sock");
        ::unlink(path_.c_str());
        fd_ = ::socket(AF_UNIX, SOCK_DGRAM, 0);
        if (fd_ < 0)
            throw std::runtime_error("cannot create notify socket");
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        const auto text = path_.string();
        std::memcpy(addr.sun_path, text.data(), text.size());
        const auto len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + text.size() + 1);
        if (::bind(fd_, reinterpret_cast<const sockaddr*>(&addr), len) < 0) {
            ::close(fd_);
            throw std::runtime_error("cannot bind notify socket");
        }
    }

    ~NotifySocket()
    {
        if (fd_ >= 0)
            ::close(fd_);
        std::error_code ec;
        fs::remove(path_, ec);
    }

    std::string path() const
    {
        return path_.string();
    }

    std::optional<std::string> try_read() const
    {
        char buffer[64];
        const auto n = ::recv(fd_, buffer, sizeof buffer, MSG_DONTWAIT);
        if (n <= 0)
            return std::nullopt;
        return std::string(buffer, static_cast<std::size_t>(n));
    }

private:
    fs::path path_;
    int fd_ = -1;
};

LiveSession::Health healthy_account()
{
    return LiveSession::Health{
        .state = {.observed = true, .pending = false, .age_ms = 100},
        .persistence = {.observed = true, .pending = false, .age_ms = 50},
        .command = {.observed = true, .pending = false, .age_ms = 10},
        .business_ready = true
    };
}

} // namespace

TEST(SystemdWatchdog, SuppressesHeartbeatsWhenInitializationOrAccountLoopStalls)
{
    NotifySocket receiver;
    ScopedEnv notify("NOTIFY_SOCKET", receiver.path());
    ScopedEnv usec("WATCHDOG_USEC", "1000000"); // 1 s -> notify interval 500 ms
    ScopedEnv pid("WATCHDOG_PID", std::to_string(::getpid()));

    auto clock = std::make_shared<ManualClock>();
    SystemdWatchdog watchdog(clock);

    const Progress::Observation idle_init{.observed = true, .pending = false, .age_ms = 0};
    auto account = healthy_account();

    // Healthy state sends WATCHDOG=1 immediately and rate-limits until half WATCHDOG_USEC.
    EXPECT_TRUE(watchdog.notify_if_healthy(idle_init, &account));
    EXPECT_EQ(receiver.try_read(), "WATCHDOG=1");
    EXPECT_TRUE(watchdog.notify_if_healthy(idle_init, &account));
    EXPECT_FALSE(receiver.try_read().has_value());

    // Stalled startup initialization stops watchdog heartbeats completely.
    clock->advance(600ms);
    const Progress::Observation stalled_init{
        .observed = true,
        .pending = true,
        .age_ms = watchdog_stall_limit_ms + 1
    };
    EXPECT_FALSE(watchdog.notify_if_healthy(stalled_init, nullptr));
    EXPECT_FALSE(receiver.try_read().has_value());

    // Stalled state loop, pending command, or pending SQLite persistence stops heartbeats.
    {
        auto stalled_state = healthy_account();
        stalled_state.state.age_ms = watchdog_stall_limit_ms + 1;
        EXPECT_FALSE(watchdog.notify_if_healthy(idle_init, &stalled_state));
        EXPECT_FALSE(receiver.try_read().has_value());
    }
    {
        auto stalled_command = healthy_account();
        stalled_command
            .command = {.observed = true, .pending = true, .age_ms = watchdog_stall_limit_ms + 1};
        EXPECT_FALSE(watchdog.notify_if_healthy(idle_init, &stalled_command));
        EXPECT_FALSE(receiver.try_read().has_value());
    }
    {
        auto stalled_write = healthy_account();
        stalled_write.persistence =
            {.observed = true, .pending = true, .age_ms = watchdog_stall_limit_ms + 1};
        EXPECT_FALSE(watchdog.notify_if_healthy(idle_init, &stalled_write));
        EXPECT_FALSE(receiver.try_read().has_value());
    }

    // Idle (non-pending) command/persistence ages do not look stalled; recovery resumes heartbeats.
    auto idle_account = healthy_account();
    idle_account.command = {.observed = true, .pending = false, .age_ms = 90000};
    idle_account.persistence = {.observed = true, .pending = false, .age_ms = 90000};
    EXPECT_TRUE(watchdog.notify_if_healthy(idle_init, &idle_account));
    EXPECT_EQ(receiver.try_read(), "WATCHDOG=1");
}

TEST(SystemdWatchdog, ValidatesSystemdEnvironmentAndIgnoresOtherPids)
{
    NotifySocket receiver;
    {
        ScopedEnv notify("NOTIFY_SOCKET", receiver.path());
        ScopedEnv usec("WATCHDOG_USEC", "0");
        ScopedEnv pid("WATCHDOG_PID", std::nullopt);
        EXPECT_THROW((void)SystemdWatchdog{}, std::runtime_error);
    }
    {
        ScopedEnv notify("NOTIFY_SOCKET", "relative.sock");
        ScopedEnv usec("WATCHDOG_USEC", "1000000");
        ScopedEnv pid("WATCHDOG_PID", std::nullopt);
        EXPECT_THROW((void)SystemdWatchdog{}, std::runtime_error);
    }
    {
        ScopedEnv notify("NOTIFY_SOCKET", receiver.path());
        ScopedEnv usec("WATCHDOG_USEC", "1000000");
        ScopedEnv pid("WATCHDOG_PID", std::to_string(::getpid() + 1));
        SystemdWatchdog inactive;
        inactive.notify();
        EXPECT_FALSE(receiver.try_read().has_value());
    }
}
