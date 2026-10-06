#include "supervision_rpc.hpp"
#include <asterion/kernel/trace.hpp>
#include <asterion/kernel/ipc/rpc_client.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/protocol/health.hpp>
#include <asterion/v1/market.pb.h>
#include <asterion/v1/data_service.pb.h>

namespace asterion::agent {
using namespace std::chrono_literals;
PolledTask<HealthObservation> probe_service_health(node::v1::ServiceKind kind,
                                                   const std::string& name,
                                                   const std::string& endpoint,
                                                   const std::string& peer) {
  ipc::RpcClient channel(endpoint, 1, PayloadBudget{256 * 1024}, 64 * 1024);
  const auto correlation = "health." + unique_process_id();
  if (kind == node::v1::MARKET_DATA) {
    market::v1::Request ping;
    ping.set_version(1);
    ping.set_service_id(name);
    ping.set_correlation_id(correlation);
    ping.mutable_heartbeat();
    auto pending = channel.request(ping.SerializeAsString(), 1500ms);
    co_await PollUntil{[&] {
      channel.poll();
      return pending.wait_for(0ms) == std::future_status::ready;
    }};
    market::v1::Response reply;
    if (!reply.ParseFromString(*pending.get()))
      throw std::runtime_error("invalid market health");
    protocol::validate_message(reply);
    if (reply.version() != 1 || reply.service_id() != name ||
        reply.correlation_id() != correlation || !reply.has_health() ||
        reply.health().instance_id().empty())
      throw std::runtime_error("market health identity mismatch");
    const auto phase = reply.health().phase();
    using protocol::ServiceHealth;
    co_return HealthObservation{!reply.health().initialized() ? ServiceHealth::starting
                                : phase == "connected"        ? ServiceHealth::ready
                                : phase == "error" || phase == "sdk_unavailable"
                                    ? ServiceHealth::degraded
                                    : ServiceHealth::awaiting_input,
                                {}};
  }
  if (kind == node::v1::DATA_SERVICE) {
    data::v1::DataRequest ping;
    ping.set_version(1);
    ping.set_service_id(name);
    ping.set_correlation_id(correlation);
    ping.mutable_heartbeat();
    auto pending = channel.request(ping.SerializeAsString(), 1500ms);
    co_await PollUntil{[&] {
      channel.poll();
      return pending.wait_for(0ms) == std::future_status::ready;
    }};
    data::v1::DataResponse reply;
    if (!reply.ParseFromString(*pending.get()))
      throw std::runtime_error("invalid data health");
    protocol::validate_message(reply);
    if (reply.version() != 1 || reply.service_id() != name ||
        reply.correlation_id() != correlation || !reply.has_health() ||
        reply.health().instance_id() != name || reply.health().task_instance() != peer)
      throw std::runtime_error("data health identity mismatch");
    using protocol::ServiceHealth;
    co_return HealthObservation{reply.health().recovery_required() ? ServiceHealth::degraded
                                : reply.health().initialized()     ? ServiceHealth::ready
                                                                   : ServiceHealth::starting,
                                {}};
  }
  if (kind == node::v1::TASK_SERVICE) {
    task::v1::TaskRequest ping;
    ping.set_version(1);
    ping.set_service_id(name);
    ping.set_correlation_id(correlation);
    ping.mutable_heartbeat();
    auto pending = channel.request(ping.SerializeAsString(), 1500ms);
    co_await PollUntil{[&] {
      channel.poll();
      return pending.wait_for(0ms) == std::future_status::ready;
    }};
    task::v1::TaskResponse reply;
    if (!reply.ParseFromString(*pending.get()))
      throw std::runtime_error("invalid task health");
    protocol::validate_message(reply);
    if (reply.version() != 1 || reply.service_id() != name ||
        reply.correlation_id() != correlation || !reply.has_health() ||
        reply.health().instance_id() != name || reply.health().data_instance() != peer)
      throw std::runtime_error("task health identity mismatch");
    using protocol::ServiceHealth;
    co_return HealthObservation{reply.health().recovery_required() ? ServiceHealth::degraded
                                : reply.health().initialized()     ? ServiceHealth::ready
                                                                   : ServiceHealth::starting,
                                {}};
  }
  protocol::v1::Request ping;
  ping.set_version(1);
  ping.set_session_id(name);
  ping.set_correlation_id(correlation);
  ping.mutable_heartbeat();
  auto pending = channel.request(ping.SerializeAsString(), 1500ms);
  co_await PollUntil{[&] {
    channel.poll();
    return pending.wait_for(0ms) == std::future_status::ready;
  }};
  protocol::v1::Response reply;
  if (!reply.ParseFromString(*pending.get()))
    throw std::runtime_error("invalid health response");
  protocol::validate_message(reply);
  if (reply.version() != 1 || reply.session_id() != name || reply.correlation_id() != correlation ||
      !reply.has_health())
    throw std::runtime_error("health identity mismatch");
  co_return HealthObservation{protocol::trading_health_phase(reply.health()),
                              reply.health().execution()};
}

PolledTask<task::v1::TaskResponse> request_task_dispatch(const std::string& name,
                                                         const std::string& endpoint,
                                                         const std::vector<std::string>& running,
                                                         unsigned launch_slots) {
  task::v1::TaskRequest request;
  request.set_version(1);
  request.set_service_id(name);
  request.set_correlation_id(next_correlation_id());
  auto* dispatch = request.mutable_dispatch();
  dispatch->set_launch_slots(launch_slots);
  for (const auto& id : running)
    dispatch->add_running(id);
  ipc::RpcClient channel(endpoint, 1, PayloadBudget{256 * 1024}, 64 * 1024);
  auto pending = channel.request(request.SerializeAsString(), 1500ms);
  co_await PollUntil{[&] {
    channel.poll();
    return pending.wait_for(0ms) == std::future_status::ready;
  }};
  task::v1::TaskResponse response;
  if (!response.ParseFromString(*pending.get()))
    throw std::runtime_error("invalid task dispatch response");
  protocol::validate_message(response);
  if (response.version() != 1 || response.service_id() != name ||
      response.correlation_id() != request.correlation_id() || !response.has_launches())
    throw std::runtime_error("task queue unavailable");
  co_return response;
}
} // namespace asterion::agent
