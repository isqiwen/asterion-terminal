#pragma once
#include <asterion/research_logic.pb.h>
#include <asterion/foundation/serialization.hpp>
#include <string_view>

namespace asterion::protocol {

std::string logic_digest(const research::proto::LogicDefinition&);
std::string logic_example_digest(research::proto::LogicExample);
void validate_logic_definition(const research::proto::LogicDefinition&);
void validate_logic_example(const research::proto::LogicExample&);
void validate_logic_trial_input(const research::proto::LogicTrialInput&);
void validate_logic_trial_result(
    const research::proto::LogicTrialInput&,
    const research::proto::LogicTrialResult&);
void validate_logic_object(const resource::proto::Object&, std::string_view kind);
resource::proto::Object logic_object(const research::proto::LogicDefinition&);
resource::proto::Object logic_object(const research::proto::LogicExample&);
resource::proto::Object encode_logic_object(const Json&, std::string_view kind);
Json decode_logic_object(const resource::proto::Object&);
Json decode_logic_reply(
    const research::proto::LogicReply&,
    const research::proto::LogicRequest&,
    const resource::proto::Owner&);
Json decode_logic_definition(const research::proto::LogicDefinition&);
Json decode_logic_example(const research::proto::LogicExample&);
Json decode_logic_trial_input(const research::proto::LogicTrialInput&);
Json decode_logic_trial_result(
    const research::proto::LogicTrialInput&,
    const research::proto::LogicTrialResult&);

} // namespace asterion::protocol
