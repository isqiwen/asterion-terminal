#include "systemd_watchdog.hpp"
#include <asterion/system/log.hpp>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>

namespace asterion::trading {

SystemdWatchdog::SystemdWatchdog(std::shared_ptr<const Clock> clock)
    : clock_(clock ? std::move(clock) : default_clock())
{
    const char* interval = std::getenv("WATCHDOG_USEC");
    const char* path = std::getenv("NOTIFY_SOCKET");
    const char* owner = std::getenv("WATCHDOG_PID");
    if (!interval || !path || (owner && std::to_string(::getpid()) != owner))
        return;
    const std::string_view usec(interval), name(path);
    std::uint64_t microseconds = 0;
    const auto [end, error] = std::from_chars(usec.data(), usec.data() + usec.size(), microseconds);
    if (error != std::errc{} || end != usec.data() + usec.size() || !microseconds)
        throw std::runtime_error("invalid WATCHDOG_USEC from systemd");
    if (name.empty() || name.size() >= sizeof address_.sun_path
        || (name.front() != '/' && name.front() != '@'))
        throw std::runtime_error("invalid NOTIFY_SOCKET from systemd");
    address_.sun_family = AF_UNIX;
    std::memcpy(address_.sun_path, name.data(), name.size());
    // A leading @ names a socket in the abstract namespace.
    if (name.front() == '@')
        address_.sun_path[0] = '\0';
    length_ = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + name.size());
    socket_ = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    if (socket_ < 0)
        throw std::runtime_error("cannot open the systemd notification socket");
    interval_ = std::chrono::microseconds(microseconds) / 2;
}

SystemdWatchdog::~SystemdWatchdog()
{
    if (socket_ >= 0)
        ::close(socket_);
}

void SystemdWatchdog::notify()
{
    const auto now = steady_now(*clock_);
    if (socket_ < 0 || now < next_)
        return;
    next_ = now + interval_;
    constexpr std::string_view message = "WATCHDOG=1";
    if (::sendto(
            socket_,
            message.data(),
            message.size(),
            0,
            reinterpret_cast<const sockaddr*>(&address_),
            length_)
        < 0)
        log::warn("systemd watchdog notification failed: {}", std::strerror(errno));
}

bool SystemdWatchdog::notify_if_healthy(
    const Progress::Observation& initialization,
    const LiveSession::Health* account,
    std::uint64_t stall_limit_ms)
{
    if (!watchdog_healthy(initialization, account, stall_limit_ms))
        return false;
    notify();
    return true;
}

} // namespace asterion::trading
