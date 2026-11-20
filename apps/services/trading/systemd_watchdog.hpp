#pragma once
#include "live_session.hpp"
#include <asterion/base/time.hpp>
#include <asterion/system/progress.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <sys/socket.h>
#include <sys/un.h>

namespace asterion::trading {

// systemd restarts an instance whose startup recovery or account loop has
// stalled: initialization, state loop, or a pending command/write has not
// moved within this limit.
inline constexpr std::uint64_t watchdog_stall_limit_ms = 30000;

inline bool watchdog_healthy(
    const Progress::Observation& initialization,
    const LiveSession::Health* account,
    std::uint64_t stall_limit_ms = watchdog_stall_limit_ms)
{
    if (initialization.pending && initialization.age_ms > stall_limit_ms)
        return false;
    if (!account)
        return true;
    return account->state.age_ms <= stall_limit_ms
        && (!account->command.pending || account->command.age_ms <= stall_limit_ms)
        && (!account->persistence.pending || account->persistence.age_ms <= stall_limit_ms);
}

// The liveness signal for systemd's WatchdogSec=. Outside such a unit it does nothing.
class SystemdWatchdog final
{
public:
    explicit SystemdWatchdog(std::shared_ptr<const Clock> clock = default_clock());
    ~SystemdWatchdog();
    SystemdWatchdog(const SystemdWatchdog&) = delete;
    SystemdWatchdog& operator=(const SystemdWatchdog&) = delete;
    // Sends at most twice per watchdog interval, as systemd asks.
    void notify();
    bool notify_if_healthy(
        const Progress::Observation& initialization,
        const LiveSession::Health* account,
        std::uint64_t stall_limit_ms = watchdog_stall_limit_ms);

private:
    int socket_ = -1;
    sockaddr_un address_{};
    socklen_t length_ = 0;
    std::chrono::steady_clock::duration interval_{};
    std::chrono::steady_clock::time_point next_{};
    std::shared_ptr<const Clock> clock_;
};

} // namespace asterion::trading
