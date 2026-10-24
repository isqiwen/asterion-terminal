#pragma once
#include <asterion/research_project.pb.h>
#include <asterion/foundation/serialization.hpp>

namespace asterion::protocol {

void validate_project_binding(const research::proto::ProjectBinding&);
Json decode_project_binding(const research::proto::ProjectBinding&);
research::proto::ProjectBinding encode_project_binding(const Json&);
research::proto::ProjectDefinition encode_project_definition(const Json&);
Json decode_project_definition(const research::proto::ProjectDefinition&);
research::proto::ConclusionWrite encode_project_conclusion(const Json&);
Json decode_project_reply(
    const research::proto::ProjectReply&,
    const research::proto::ProjectRequest&,
    const resource::proto::Owner&);

} // namespace asterion::protocol
