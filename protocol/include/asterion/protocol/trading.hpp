#pragma once
#include <asterion/domain/account.hpp>
#include <asterion/foundation/serialization.hpp>
#include <asterion/v1/trading.pb.h>
namespace asterion::protocol {
v1::Contract encode_contract(const Json& input);
Json decode_contract(const v1::Contract& input);
v1::RiskLimits encode_risk(const Json& value);
Json decode_risk(const v1::RiskLimits& value);
v1::Costs encode_costs(const Json& costs);
Json decode_costs(const v1::Costs& costs);
FuturesCosts futures_costs(const v1::Costs& costs);
v1::PaperInput encode_input(const Json& manifest);
Json decode_input(const v1::PaperInput& input);
v1::Command encode_command(const Json& command);
Json decode_command(const v1::Command& command);
v1::Snapshot encode_snapshot(const Json& snapshot);
Json decode_snapshot(const v1::Snapshot& snapshot);
void validate_message(const google::protobuf::Message& message);
} // namespace asterion::protocol
