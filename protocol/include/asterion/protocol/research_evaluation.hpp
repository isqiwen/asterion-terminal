#pragma once
#include <asterion/research_evaluation.pb.h>
#include <asterion/foundation/serialization.hpp>
#include <utility>

namespace asterion::protocol {

void validate_evaluation_request(const research::proto::EvaluationRequest&);
void validate_evaluation_input(const research::proto::EvaluationInput&);
std::pair<unsigned, unsigned> evaluation_range(
    const research::proto::EvaluationRequest&,
    const backtest::proto::BacktestInput&,
    const backtest::proto::BacktestResult&);
void validate_evaluation_source(
    const research::proto::EvaluationInput&,
    const backtest::proto::BacktestInput&,
    const backtest::proto::BacktestResult&);
void validate_evaluation_result(
    const research::proto::EvaluationInput&,
    const research::proto::EvaluationResult&);
research::proto::EvaluationRequest encode_evaluation_request(const Json&);
Json decode_evaluation_input(const research::proto::EvaluationInput&);
Json decode_evaluation_result(
    const research::proto::EvaluationInput&,
    const research::proto::EvaluationResult&);

} // namespace asterion::protocol
