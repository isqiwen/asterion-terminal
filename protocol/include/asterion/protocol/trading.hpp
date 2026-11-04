#pragma once
#include <asterion/base/serialization.hpp>
#include <asterion/futures/account.hpp>
#include <asterion/system/progress.hpp>
#include <asterion/trading.pb.h>

namespace asterion::protocol {

// The trading terms of a policy contract; every increment and the multiplier are required.
Instrument terms(const ::trading::ContractTerms& contract);
::common::Costs encode_costs(const FuturesCosts& costs);
// Commands and policies in the JSON the account journal stores.
::trading::Command encode_command(const Json& command);
Json decode_command(const ::trading::Command& command);
::trading::Policy encode_policy(const Json& policy);
void validate_policy(const ::trading::Policy& policy);
Json decode_policy(const ::trading::Policy& policy);
// The configured account without its session.
::trading::Account encode_account(const Json& account);
// A live account manifest ("live_ctp"), checked and in the form the journal stores.
Json live_input(const Json& manifest);
::trading::Progress encode_progress(Progress::Observation value);
// Unknown fields are refused: a peer of another protocol version.
void validate_message(const google::protobuf::Message& message);

} // namespace asterion::protocol
