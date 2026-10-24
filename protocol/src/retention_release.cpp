#include <asterion/protocol/retention_release.hpp>
#include <asterion/research.pb.h>
#include <asterion/intelligence.pb.h>
#include <asterion/foundation/error.hpp>
#include <asterion/foundation/id.hpp>
#include <asterion/kernel/ipc/rpc_client.hpp>
#include <asterion/kernel/trace.hpp>
#include <asterion/protocol/service_discovery.hpp>
#include <asterion/protocol/trading.hpp>
#include <algorithm>

namespace asterion::protocol {

resource::proto::ReleaseDecision verify_retention_release(
    const resource::proto::Object& consumer,
    const std::string& node_id,
    const std::string& discovery_endpoint,
    std::stop_token stop,
    std::chrono::milliseconds timeout)
{
    using namespace std::chrono_literals;
    validate_message(consumer);
    validate_id(node_id);
    validate_id(consumer.owner().service_id());
    const bool intelligence_owner = consumer.owner().domain() == "intelligence";
    if (!intelligence_owner && consumer.owner().domain() != "research")
        throw Error(ErrorCode::conflict, "consumer domain has no implemented release verifier");
    if (consumer.owner().node_id() != node_id)
        throw Error(ErrorCode::conflict, "cross-node release verification is not available");
    if (discovery_endpoint.empty())
        throw Error(ErrorCode::unavailable, "release verification requires the owning Agent");
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const auto remaining = [&] {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "release verification cancelled");
        const auto left = std::chrono::ceil<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left <= 0ms)
            throw Error(ErrorCode::unavailable, "release verification timed out");
        return left;
    };
    const auto status = service_discovery(discovery_endpoint, remaining(), stop);
    if (status.node_id() != node_id || status.phase() != node::proto::Status::READY
        || status.has_failure())
        throw Error(ErrorCode::unavailable, "consumer node discovery is not ready");
    const node::proto::Service* peer = nullptr;
    for (const auto& service : status.services()) {
        if (service.id() != consumer.owner().service_id())
            continue;
        if (peer
            || service.kind()
                != (intelligence_owner ? node::proto::INTELLIGENCE : node::proto::RESEARCH))
            throw Error(ErrorCode::conflict, "consumer discovery identity mismatch");
        peer = &service;
    }
    if (!peer || peer->state() != "running" || peer->endpoint().empty())
        throw Error(
            ErrorCode::unavailable,
            "consumer owner is not reachable; retention stays held");
    research::proto::ResearchRequest request;
    request.set_version(1);
    request.set_service_id(consumer.owner().service_id());
    request.set_correlation_id(next_correlation_id());
    *request.mutable_release_decision() = consumer;
    intelligence::proto::IntelligenceRequest intelligence_request;
    if (intelligence_owner) {
        intelligence_request.set_version(request.version());
        intelligence_request.set_service_id(request.service_id());
        intelligence_request.set_correlation_id(request.correlation_id());
        *intelligence_request.mutable_release_decision() = consumer;
    }
    ipc::Reactor reactor;
    ipc::RpcClient client(reactor, peer->endpoint(), 1, PayloadBudget{65536}, 65536);
    auto pending = client.request(
        intelligence_owner ? intelligence_request.SerializeAsString() : request.SerializeAsString(),
        remaining(),
        deadline,
        remaining());
    while (pending.wait_for(0ms) != std::future_status::ready) {
        const auto left = remaining();
        client.poll();
        if (pending.wait_for(0ms) == std::future_status::ready)
            break;
        reactor.wait(std::min(left, 100ms));
    }
    const auto frame = pending.get();
    if (intelligence_owner) {
        intelligence::proto::IntelligenceResponse response;
        if (!response.ParseFromString(*frame))
            throw std::invalid_argument("invalid intelligence release response");
        validate_message(response);
        if (response.version() != request.version() || response.service_id() != request.service_id()
            || response.correlation_id() != request.correlation_id())
            throw std::invalid_argument("intelligence release response identity mismatch");
        if (response.has_error())
            throw_remote_error(response.error().code(), response.error().message());
        if (!response.has_release_decision()
            || response.release_decision().object().SerializeAsString()
                != consumer.SerializeAsString()
            || response.release_decision().state() != resource::proto::ReleaseDecision::RELEASED)
            throw Error(ErrorCode::conflict, "intelligence has not durably released this version");
        return response.release_decision();
    }
    task::proto::TaskResponse response;
    if (!response.ParseFromString(*frame))
        throw std::invalid_argument("invalid consumer release response");
    validate_message(response);
    if (response.version() != request.version() || response.service_id() != request.service_id()
        || response.correlation_id() != request.correlation_id() || response.has_result_task())
        throw std::invalid_argument("consumer release response identity mismatch");
    if (response.has_error())
        throw_remote_error(response.error().code(), response.error().message());
    if (!response.has_release_decision()
        || response.release_decision().object().SerializeAsString() != consumer.SerializeAsString()
        || response.release_decision().state() != resource::proto::ReleaseDecision::RELEASED)
        throw Error(ErrorCode::conflict, "consumer has not durably released this version");
    return response.release_decision();
}

} // namespace asterion::protocol
