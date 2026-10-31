#pragma once
#include <asterion/research_statistics.pb.h>
#include <asterion/factor.pb.h>
#include <asterion/research_sample.pb.h>
#include <asterion/base/serialization.hpp>

namespace asterion::protocol {

void validate_statistics_request(const research::proto::StatisticsRequest&);
void validate_statistics_input(const research::proto::StatisticsInput&);
void validate_statistics_source(
    const research::proto::StatisticsInput&,
    const factor::proto::FactorInput&,
    const factor::proto::FactorResult&);
void validate_statistics_source(
    const research::proto::StatisticsInput&,
    const research::proto::SampleInput&,
    const research::proto::SampleResult&);
void validate_statistics_result(
    const research::proto::StatisticsInput&,
    const research::proto::StatisticsResult&);
research::proto::StatisticsRequest encode_statistics_request(const Json&);
Json decode_statistics_input(const research::proto::StatisticsInput&);
Json decode_statistics_result(
    const research::proto::StatisticsInput&,
    const research::proto::StatisticsResult&);

} // namespace asterion::protocol
