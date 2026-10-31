#include "research_logic.hpp"
#include "strategy.hpp"
#include <asterion/protocol/research_logic.hpp>
#include <asterion/base/error.hpp>

namespace asterion::tasks {

research::proto::LogicTrialResult compute_logic_trial(
    const research::proto::LogicTrialInput& input,
    std::stop_token stop,
    const std::function<void(unsigned, unsigned)>& progress)
{
    protocol::validate_logic_trial_input(input);
    MovingAverageSignal calculation(input.definition().fast(), input.definition().slow());
    research::proto::LogicTrialResult result;
    const auto total = static_cast<unsigned>(input.example().observations_size());
    unsigned matched = 0, decisions = 0;
    for (unsigned i = 0; i < total; ++i) {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "logic trial cancelled");
        const auto& sample = input.example().observations(static_cast<int>(i));
        calculation.push(Decimal::from_raw(sample.close().units()));
        const auto observation = calculation.observation();
        auto& row = *result.add_rows();
        row.set_index(i);
        if (observation.direction)
            row.set_target_sign(*observation.direction);
        else
            row.set_warmup(true);
        if (observation.fast_mean)
            row.set_fast_mean(*observation.fast_mean);
        if (observation.slow_mean)
            row.set_slow_mean(*observation.slow_mean);
        if (observation.difference)
            row.set_difference(*observation.difference);
        const bool match = observation.direction
            ? sample.has_target_sign() && sample.target_sign() == *observation.direction
            : sample.has_warmup();
        row.set_matches(match);
        matched += match;
        decisions += observation.direction.has_value();
        if (progress)
            progress(i + 1, total);
    }
    if (stop.stop_requested())
        throw Error(ErrorCode::cancelled, "logic trial cancelled");
    result.set_matched(matched);
    result.set_decisions(decisions);
    result.set_all_match(matched == total);
    return result;
}

} // namespace asterion::tasks
