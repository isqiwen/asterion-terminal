#pragma once
#include <asterion/v1/node.pb.h>
#include <asterion/v1/task.pb.h>
#include <asterion/kernel/polled_task.hpp>
#include <optional>
#include <string>
#include <vector>

namespace asterion::agent {
// The owner polls these bounded RPCs and keeps their inputs alive while suspended.
// Revalidate process incarnation and configuration before applying observations.
struct HealthObservation {
  std::string status;
  std::optional<runtime::v1::ExecutionHealth> execution;
};
PolledTask<HealthObservation> probe_service_health(node::v1::ServiceKind kind,
                                                   const std::string& name,
                                                   const std::string& endpoint,
                                                   const std::string& peer);
PolledTask<task::v1::TaskResponse> request_task_dispatch(const std::string& name,
                                                         const std::string& endpoint,
                                                         const std::vector<std::string>& running,
                                                         unsigned launch_slots);
} // namespace asterion::agent
