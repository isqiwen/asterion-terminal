#pragma once
#include <asterion/node.pb.h>
#include <asterion/task.pb.h>
#include <asterion/protocol/health.hpp>
#include <asterion/kernel/polled_task.hpp>
#include <optional>
#include <string>
#include <vector>

namespace asterion::terminal {

// One heartbeat of a local service on its private health endpoint, bounded to
// 1.5 s. The caller revalidates the process before applying the observation.
struct HealthObservation
{
    protocol::ServiceHealth status = protocol::ServiceHealth::starting;
    std::optional<runtime::proto::ExecutionHealth> execution;
};

PolledTask<HealthObservation> probe_service_health(
    node::proto::ServiceKind kind,
    // By value: the coroutine runs after the caller's arguments are gone.
    std::string name,
    std::string endpoint);

} // namespace asterion::terminal
