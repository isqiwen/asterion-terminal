#pragma once
#include <asterion/research_sample.pb.h>
#include <asterion/foundation/serialization.hpp>

namespace asterion::protocol {

void validate_sample_request(const research::proto::SampleRequest&);
void validate_sample_input(const research::proto::SampleInput&);
research::proto::SampleExecution sample_execution(const research::proto::SampleInput&);
void validate_sample_result(
    const research::proto::SampleInput&,
    const research::proto::SampleResult&);
research::proto::SampleRequest encode_sample_request(const Json&);
Json decode_sample_input(const research::proto::SampleInput&);
Json decode_sample_result(
    const research::proto::SampleInput&,
    const research::proto::SampleResult&);

} // namespace asterion::protocol
