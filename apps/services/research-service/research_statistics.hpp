#pragma once
#include "research_execution.hpp"
#include <cstdint>
#include <asterion/research_statistics.pb.h>

namespace asterion::tasks {

// The response has been verified and reconfirmed by the owning Store. The byte
// identity is captured with the same ResultRead, never supplied by a client.
research::proto::StatisticsInput capture_statistics_input(
    const resource::proto::Owner&,
    const research::proto::StatisticsRequest&,
    const task::proto::TaskResponse&,
    const std::string& input_digest,
    std::uint64_t input_bytes);
research::proto::StatisticsResult compute_statistics(
    const research::proto::StatisticsInput&,
    const std::filesystem::path& root,
    std::stop_token = {},
    const TaskProgress& = {});

} // namespace asterion::tasks
