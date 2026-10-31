#pragma once
#include <asterion/research_variable.pb.h>
#include <asterion/base/serialization.hpp>

namespace asterion::protocol {

std::string variable_digest(research::proto::VariableDefinition);
void validate_variable_definition(const research::proto::VariableDefinition&);
void validate_variable_reference(const resource::proto::Object&);
resource::proto::Object variable_object(const research::proto::VariableDefinition&);
void verify_variable_reference(
    const resource::proto::Object&,
    const research::proto::VariableDefinition&);
Json decode_variable_definition(const research::proto::VariableDefinition&);
Json decode_variable_reply(
    const research::proto::VariableReply&,
    const research::proto::VariableRequest&,
    const resource::proto::Owner&);
resource::proto::Object encode_variable_reference(const Json&);
Json decode_variable_reference(const resource::proto::Object&);

} // namespace asterion::protocol
