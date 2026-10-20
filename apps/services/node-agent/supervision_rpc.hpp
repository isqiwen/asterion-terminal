#pragma once
#include <asterion/node.pb.h>
#include <asterion/task.pb.h>
#include <asterion/protocol/health.hpp>
#include <asterion/kernel/polled_task.hpp>
#include <optional>
#include <string>
#include <vector>

namespace asterion::agent {

// The owner polls these bounded RPCs and keeps their inputs alive while suspended.
// Revalidate process incarnation and configuration before applying observations.
struct HealthObservation
{
    protocol::ServiceHealth status = protocol::ServiceHealth::starting;
    std::optional<runtime::proto::ExecutionHealth> execution;
};

PolledTask<HealthObservation> probe_service_health(
    node::proto::ServiceKind kind,
    const std::string& name,
    const std::string& endpoint,
    const std::string& peer);
PolledTask<task::proto::TaskResponse> request_task_dispatch(
    const std::string& name,
    const std::string& endpoint,
    const std::vector<std::string>& running,
    unsigned launch_slots);

} // namespace asterion::agent
