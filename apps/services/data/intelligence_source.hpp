#pragma once
#include <asterion/intelligence.pb.h>
#include <stop_token>

namespace asterion::data {

intelligence::proto::NoticePublicationIntent fetch_intelligence_preparation(
    const resource::proto::Object&,
    const std::string& node,
    const std::string& discovery,
    std::stop_token);

intelligence::proto::NoticeChangeIntent fetch_intelligence_change_preparation(
    const resource::proto::Object&,
    const std::string& node,
    const std::string& discovery,
    std::stop_token);

intelligence::proto::NoticeSetIntent fetch_intelligence_set_preparation(
    const resource::proto::Object&,
    const std::string& node,
    const std::string& discovery,
    std::stop_token);

} // namespace asterion::data
