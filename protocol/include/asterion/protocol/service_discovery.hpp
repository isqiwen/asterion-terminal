#pragma once
#include <asterion/node.pb.h>
#include <chrono>
#include <stop_token>
#include <string>
#include <string_view>

namespace asterion::protocol {

node::proto::Request service_discovery_request(std::string_view parent = {});
node::proto::Status decode_service_discovery(const node::proto::Request&, const std::string&);
// File-worker use only. Metadata remains on the owning Agent, with one bounded request.
node::proto::Status service_discovery(
    const std::string& endpoint,
    std::chrono::milliseconds timeout,
    std::stop_token stop = {});

} // namespace asterion::protocol
