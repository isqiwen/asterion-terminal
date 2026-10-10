#pragma once
#include <asterion/domain/account.hpp>
#include <asterion/domain/position_target.hpp>
#include <asterion/foundation/serialization.hpp>
#include <asterion/v1/trading.pb.h>
namespace asterion::protocol {
v1::Contract encode_contract(const Json& input);
Json decode_contract(const v1::Contract& input);
// Which sides a strategy may hold: "both", "long" or "short".
PositionSides position_sides(v1::PositionSides value);
v1::PositionSides encode_position_sides(const Json& value);
// {"quantity", "sides", "rule": {"kind": "moving_average" | "breakout" |
// "momentum" | "reversion", ...its windows}}, or without a quantity
// {"sides", "rule": {"kind": "cross_momentum" | "cross_term_structure", ...its
// windows, "notional"}}. Shapes only: whether
// the windows make a strategy is the strategy's to say.
v1::Strategy encode_strategy(const Json& value);
Json decode_strategy(const v1::Strategy& strategy);
// Whether the definition makes a strategy for a contract traded in lots of
// `quantity_increment`: a positive lot-aligned quantity, sides, and windows
// its rule can work with. A rule over several contracts has a positive
// notional instead of a quantity.
void validate_strategy(const v1::Strategy& strategy, Decimal quantity_increment);
// The completed bars after which the rule first takes a side or none.
std::size_t strategy_warmup(const v1::Strategy& strategy);
v1::RiskLimits encode_risk(const Json& value);
Json decode_risk(const v1::RiskLimits& value);
v1::Costs encode_costs(const Json& costs);
Json decode_costs(const v1::Costs& costs);
FuturesCosts futures_costs(const v1::Costs& costs);
v1::CostSchedule encode_cost_schedule(const Json& schedule);
Json decode_cost_schedule(const v1::CostSchedule& schedule);
std::vector<FuturesCostVersion> cost_schedule(const v1::CostSchedule& schedule);
v1::Costs encode_costs(const FuturesCosts& costs);
ContractTerms contract_terms(const v1::PaperContract& contract);
// Identity of a portfolio's data; a single dataset's own revision.
std::string dataset_revision(const v1::PaperInput& input);
enum class DatasetView { full, metadata };
Json decode_input(const v1::PaperInput& input, DatasetView view = DatasetView::full);
v1::Command encode_command(const Json& command);
Json decode_command(const v1::Command& command);
v1::Snapshot encode_snapshot(const Json& snapshot);
Json decode_snapshot(const v1::Snapshot& snapshot);
// Live manifest version 5 ("live_ctp") and the broker-reported session state.
v1::LivePolicy encode_live_policy(const Json& policy);
Json decode_live_policy(const v1::LivePolicy& policy);
v1::LiveInput encode_live_input(const Json& manifest);
Json decode_live_input(const v1::LiveInput& input);
v1::LiveSnapshot encode_live_snapshot(const Json& snapshot);
Json decode_live_snapshot(const v1::LiveSnapshot& snapshot);
void validate_message(const google::protobuf::Message& message);
} // namespace asterion::protocol
