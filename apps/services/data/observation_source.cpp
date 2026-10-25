#include "observation_source.hpp"
#include <asterion/protocol/observation.hpp>
#include <asterion/protocol/service_discovery.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/kernel/ipc/rpc_client.hpp>
#include <asterion/kernel/trace.hpp>

namespace asterion::data {

market::proto::ObservationCapture fetch_market_observation(
    const resource::proto::Object& source,
    const std::string& node,
    const std::string& discovery,
    std::stop_token stop)
{
    using namespace std::chrono_literals;
    protocol::validate_observation_reference(source);
    if (source.owner().node_id() != node)
        throw Error(ErrorCode::conflict, "cross-node observation archiving is not available");
    if (discovery.empty())
        throw Error(ErrorCode::unavailable, "observation archiving requires the owning Agent");
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    const auto remaining = [&] {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "observation archiving cancelled");
        const auto left = std::chrono::ceil<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left <= 0ms)
            throw Error(ErrorCode::unavailable, "market observation request timed out");
        return left;
    };
    const auto status = protocol::service_discovery(discovery, remaining(), stop);
    if (status.node_id() != node || status.phase() != node::proto::Status::READY
        || status.has_failure())
        throw Error(ErrorCode::unavailable, "observation source node discovery is not ready");
    const node::proto::Service* peer = nullptr;
    for (const auto& service : status.services())
        if (service.id() == source.owner().service_id()) {
            if (peer || service.kind() != node::proto::MARKET)
                throw Error(ErrorCode::conflict, "market observation owner mismatch");
            peer = &service;
        }
    if (!peer || peer->state() != "running" || peer->endpoint().empty())
        throw Error(ErrorCode::unavailable, "market observation source is not reachable");
    market::proto::Request request;
    request.set_version(1);
    request.set_service_id(source.owner().service_id());
    request.set_correlation_id(next_correlation_id());
    *request.mutable_observation_read() = source;
    ipc::Reactor reactor;
    ipc::RpcClient
        client(reactor, peer->endpoint(), 1, PayloadBudget{8 * 1024 * 1024}, 8 * 1024 * 1024);
    auto pending = client.request(request.SerializeAsString(), remaining(), deadline, remaining());
    while (pending.wait_for(0ms) != std::future_status::ready) {
        const auto left = remaining();
        client.poll();
        if (pending.wait_for(0ms) == std::future_status::ready)
            break;
        reactor.wait(std::min(left, 100ms));
    }
    market::proto::Response response;
    if (!response.ParseFromString(*pending.get()))
        throw std::invalid_argument("invalid market observation response");
    protocol::validate_message(response);
    if (response.version() != request.version() || response.service_id() != request.service_id()
        || response.correlation_id() != request.correlation_id())
        throw std::invalid_argument("market observation response identity mismatch");
    if (response.has_error())
        throw_remote_error(response.error().code(), response.error().message());
    if (!response.has_observation()
        || response.observation().reference().SerializeAsString() != source.SerializeAsString())
        throw Error(ErrorCode::conflict, "observation payload identity mismatch");
    protocol::validate_observation(response.observation());
    return response.observation();
}

} // namespace asterion::data
