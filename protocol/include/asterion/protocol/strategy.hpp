#pragma once
#include <asterion/protocol/data.hpp>
#include <asterion/v1/strategy.pb.h>
namespace asterion::protocol {
Json decode_replay_plan(const strategy::v1::ReplayPlan& plan);
strategy::v1::ReplayPlan encode_replay_plan(const Json& plan);
} // namespace asterion::protocol
