#pragma once
#include <asterion/domain/account.hpp>
#include <asterion/domain/position_target.hpp>
#include <asterion/foundation/serialization.hpp>
#include <asterion/trading.pb.h>

namespace asterion::protocol {

proto::Contract encode_contract(const Json& input);
Json decode_contract(const proto::Contract& input);
// Which sides a strategy may hold: "both", "long" or "short".
PositionSides position_sides(proto::PositionSides value);
proto::PositionSides encode_position_sides(const Json& value);
// What a factor is called: "momentum", "term_structure" or "volatility".
proto::Factor encode_factor(const Json& name);
const char* factor_name(proto::Factor factor);
// {"quantity", "sides", "rule": {"kind": "moving_average" | "breakout" |
// "momentum" | "reversion", ...its windows}}, or without a quantity
// {"sides", "rule": {"kind": "cross_" and the name of the factor ranked by,
// "reverse", ...its windows, "notional", "volatility"}}. Shapes only: whether
// the windows make a strategy is the strategy's to say.
proto::Strategy encode_strategy(const Json& value);
Json decode_strategy(const proto::Strategy& strategy);
// Whether the definition makes a strategy for a contract traded in lots of
// `quantity_increment`: a positive lot-aligned quantity, sides, and windows
// its rule can work with. A rule over several contracts has a positive
// notional instead of a quantity.
void validate_strategy(const proto::Strategy& strategy, Decimal quantity_increment);
// The completed bars after which the rule first takes a side or none.
std::size_t strategy_warmup(const proto::Strategy& strategy);
proto::RiskLimits encode_risk(const Json& value);
Json decode_risk(const proto::RiskLimits& value);
proto::Costs encode_costs(const Json& costs);
Json decode_costs(const proto::Costs& costs);
FuturesCosts futures_costs(const proto::Costs& costs);
proto::CostSchedule encode_cost_schedule(const Json& schedule);
Json decode_cost_schedule(const proto::CostSchedule& schedule);
std::vector<FuturesCostVersion> cost_schedule(const proto::CostSchedule& schedule);
proto::Costs encode_costs(const FuturesCosts& costs);
ContractTerms contract_terms(const proto::PaperContract& contract);
// Identity of a portfolio's data; a single dataset's own revision.
std::string dataset_revision(const proto::PaperInput& input);
enum class DatasetView
{
    full,
    metadata
};
// More would say the model no longer fits the contract.
inline constexpr unsigned max_slippage_ticks = 100;
Json decode_input(const proto::PaperInput& input, DatasetView view = DatasetView::full);
proto::Command encode_command(const Json& command);
Json decode_command(const proto::Command& command);
// Detached original facts, shared by full account and fixed decision observations.
proto::Position encode_position(const PositionLot& position);
proto::Order encode_order(const AccountSnapshot::OrderView& order);
Json decode_position(const proto::Position& position);
Json decode_order(const proto::Order& order);
// Account observation encoded directly; the worker supplies replay progress.
proto::Snapshot encode_snapshot(const FuturesAccount& account, const proto::PaperInput& input);
Json decode_snapshot(const proto::Snapshot& snapshot);
// Live manifest ("live_ctp") and the broker-reported session state.
proto::LivePolicy encode_live_policy(const Json& policy);
void validate_live_policy(const proto::LivePolicy& policy);
Json decode_live_policy(const proto::LivePolicy& policy);
proto::LiveInput encode_live_input(const Json& manifest);
Json decode_live_input(const proto::LiveInput& input);
Json decode_live_snapshot(const proto::LiveSnapshot& snapshot);
void validate_message(const google::protobuf::Message& message);

} // namespace asterion::protocol
