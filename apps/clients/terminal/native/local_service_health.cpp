#include "local_service_health.hpp"
#include <asterion/system/trace.hpp>
#include <asterion/system/ipc/rpc_client.hpp>
#include <asterion/system/process/child.hpp>
#include <asterion/protocol/health.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/lab.pb.h>

namespace asterion::terminal {

using namespace std::chrono_literals;

PolledTask<HealthObservation>
probe_service_health(node::proto::ServiceKind kind, std::string name, std::string endpoint)
{
    ipc::RpcClient channel(endpoint, 1, PayloadBudget{256 * 1024}, 64 * 1024);
    const auto correlation = "health." + unique_process_id();
    if (kind == node::proto::LAB) {
        lab::proto::Request ping;
        ping.set_version(1);
        ping.set_service_id(name);
        ping.set_correlation_id(correlation);
        ping.mutable_health();
        auto pending = channel.request(ping.SerializeAsString(), 1500ms);
        co_await PollUntil{[&] {
            channel.poll();
            return pending.wait_for(0ms) == std::future_status::ready;
        }};
        lab::proto::Response reply;
        if (!reply.ParseFromString(*pending.get()))
            throw std::runtime_error("invalid lab health");
        protocol::validate_message(reply);
        if (reply.version() != 1 || reply.service_id() != name
            || reply.correlation_id() != correlation || !reply.has_health()
            || reply.health().instance_id() != name)
            throw std::runtime_error("lab health identity mismatch");
        using protocol::ServiceHealth;
        co_return HealthObservation{
            reply.health().initialized() ? ServiceHealth::ready : ServiceHealth::starting,
            {}
        };
    }
    throw std::invalid_argument("the Terminal supervises no such service kind");
}

} // namespace asterion::terminal
