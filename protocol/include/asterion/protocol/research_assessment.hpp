#pragma once
#include <asterion/research_assessment.pb.h>
#include <asterion/foundation/serialization.hpp>

namespace asterion::protocol {

void validate_batch_assessment_snapshot(const research::proto::BatchAssessmentSnapshot&);
void validate_batch_assessment_declaration(const research::proto::BatchAssessmentDeclaration&);
void validate_batch_assessment(const research::proto::BatchAssessment&);
std::string batch_assessment_snapshot_digest(const research::proto::BatchAssessmentSnapshot&);
std::string batch_assessment_digest(research::proto::BatchAssessment);
resource::proto::Object batch_assessment_object(const research::proto::BatchAssessment&);

research::proto::BatchAssessmentDeclaration encode_batch_assessment_declaration(const Json&);
Json decode_batch_assessment(const research::proto::BatchAssessment&);
Json decode_batch_assessment_reply(
    const research::proto::BatchAssessmentReply&,
    const research::proto::BatchAssessmentRequest&,
    const resource::proto::Owner&);

} // namespace asterion::protocol
