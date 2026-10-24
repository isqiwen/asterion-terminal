#include "intelligence_source.hpp"
#include <asterion/protocol/intelligence.hpp>
#include <asterion/protocol/service_discovery.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/kernel/ipc/rpc_client.hpp>
#include <asterion/kernel/trace.hpp>

namespace asterion::data {

static intelligence::proto::IntelligenceResponse fetch_preparation(
    const resource::proto::Object& source,
    const std::string& node,
    const std::string& discovery,
    std::stop_token stop,
    const std::string& kind)
{
    using namespace std::chrono_literals;
    protocol::validate_message(source);
    const bool change = kind == "intelligence_change_preparation";
    const bool set = kind == "intelligence_set_preparation";
    if ((!change && !set && kind != "intelligence_preparation")
        || source.owner().domain() != "intelligence" || source.kind() != kind)
        throw std::invalid_argument("invalid intelligence preparation source");
    if (source.owner().node_id() != node)
        throw Error(ErrorCode::conflict, "cross-node intelligence content saving is not available");
    if (discovery.empty())
        throw Error(
            ErrorCode::unavailable,
            "intelligence content saving requires the owning Agent");
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    const auto remaining = [&] {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "intelligence content saving cancelled");
        const auto left = std::chrono::ceil<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left <= 0ms)
            throw Error(ErrorCode::unavailable, "intelligence preparation request timed out");
        return left;
    };
    const auto status = protocol::service_discovery(discovery, remaining(), stop);
    if (status.node_id() != node || status.phase() != node::proto::Status::READY
        || status.has_failure())
        throw Error(ErrorCode::unavailable, "intelligence source node discovery is not ready");
    const node::proto::Service* peer = nullptr;
    for (const auto& service : status.services())
        if (service.id() == source.owner().service_id()) {
            if (peer || service.kind() != node::proto::INTELLIGENCE)
                throw Error(ErrorCode::conflict, "intelligence preparation owner mismatch");
            peer = &service;
        }
    if (!peer || peer->state() != "running" || peer->endpoint().empty())
        throw Error(ErrorCode::unavailable, "intelligence preparation source is not reachable");
    intelligence::proto::IntelligenceRequest request;
    request.set_version(1);
    request.set_service_id(source.owner().service_id());
    request.set_correlation_id(next_correlation_id());
    if (set)
        *request.mutable_event_set()->mutable_preparation() = source;
    else if (change)
        *request.mutable_change_preparation() = source;
    else
        *request.mutable_preparation() = source;
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
    intelligence::proto::IntelligenceResponse response;
    if (!response.ParseFromString(*pending.get()))
        throw std::invalid_argument("invalid intelligence preparation response");
    protocol::validate_message(response);
    if (response.version() != request.version() || response.service_id() != request.service_id()
        || response.correlation_id() != request.correlation_id())
        throw std::invalid_argument("intelligence preparation response identity mismatch");
    if (response.has_error())
        throw_remote_error(response.error().code(), response.error().message());
    const bool exact = set ? response.has_set_preparation()
            && response.set_preparation().reference().SerializeAsString()
                == source.SerializeAsString()
        : change ? response.has_change_preparation()
            && response.change_preparation().reference().SerializeAsString()
                == source.SerializeAsString()
                 : response.has_preparation()
            && response.preparation().reference().SerializeAsString() == source.SerializeAsString();
    if (!exact)
        throw Error(ErrorCode::conflict, "intelligence payload identity mismatch");
    return response;
}

intelligence::proto::NoticePublicationIntent fetch_intelligence_preparation(
    const resource::proto::Object& source,
    const std::string& node,
    const std::string& discovery,
    std::stop_token stop)
{
    return fetch_preparation(source, node, discovery, stop, "intelligence_preparation")
        .preparation();
}

intelligence::proto::NoticeChangeIntent fetch_intelligence_change_preparation(
    const resource::proto::Object& source,
    const std::string& node,
    const std::string& discovery,
    std::stop_token stop)
{
    return fetch_preparation(source, node, discovery, stop, "intelligence_change_preparation")
        .change_preparation();
}

intelligence::proto::NoticeSetIntent fetch_intelligence_set_preparation(
    const resource::proto::Object& source,
    const std::string& node,
    const std::string& discovery,
    std::stop_token stop)
{
    return fetch_preparation(source, node, discovery, stop, "intelligence_set_preparation")
        .set_preparation();
}

} // namespace asterion::data
