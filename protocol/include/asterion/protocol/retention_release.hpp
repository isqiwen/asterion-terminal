#pragma once
#include <asterion/resource.pb.h>
#include <chrono>
#include <stop_token>
#include <string>

namespace asterion::protocol {

resource::proto::ReleaseDecision verify_retention_release(
    const resource::proto::Object&,
    const std::string& node_id,
    const std::string& discovery_endpoint,
    std::stop_token,
    std::chrono::milliseconds timeout = std::chrono::seconds(5));

} // namespace asterion::protocol
