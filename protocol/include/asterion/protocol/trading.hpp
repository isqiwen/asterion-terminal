#pragma once
#include <asterion/futures/account.hpp>
#include <asterion/futures/position_target.hpp>
#include <asterion/base/serialization.hpp>
#include <asterion/trading.pb.h>

namespace asterion::protocol {

proto::Contract encode_contract(const Json& input);
// The contract's trading terms; every increment and the multiplier are required.
Instrument instrument(const proto::Contract& contract);
Json decode_contract(const proto::Contract& input);
// Which sides a strategy may hold: "both", "long" or "short".
PositionSides position_sides(proto::PositionSides value);
proto::PositionSides encode_position_sides(const Json& value);
proto::RiskLimits encode_risk(const Json& value);
Json decode_risk(const proto::RiskLimits& value);
proto::Costs encode_costs(const Json& costs);
Json decode_costs(const proto::Costs& costs);
FuturesCosts futures_costs(const proto::Costs& costs);
proto::CostSchedule encode_cost_schedule(const Json& schedule);
Json decode_cost_schedule(const proto::CostSchedule& schedule);
std::vector<FuturesCostVersion> cost_schedule(const proto::CostSchedule& schedule);
proto::Costs encode_costs(const FuturesCosts& costs);
// More would say the model no longer fits the contract.
inline constexpr unsigned max_slippage_ticks = 100;
proto::Command encode_command(const Json& command);
Json decode_command(const proto::Command& command);
// Live manifest ("live_ctp") and the broker-reported session state.
proto::LivePolicy encode_live_policy(const Json& policy);
void validate_live_policy(const proto::LivePolicy& policy);
Json decode_live_policy(const proto::LivePolicy& policy);
proto::TradingAccount encode_trading_account(const Json& account);
Json decode_trading_account(const proto::TradingAccount& account);
proto::LiveInput encode_live_input(const Json& manifest);
Json decode_live_input(const proto::LiveInput& input);
Json decode_live_snapshot(const proto::LiveSnapshot& snapshot);
void validate_message(const google::protobuf::Message& message);

} // namespace asterion::protocol
