#include "research_execution.hpp"
#include "research_statistics.hpp"
#include "research_evaluation.hpp"
#include "research_sample.hpp"
#include "research_logic.hpp"
#include "backtest/engine.hpp"
#include "factor/factor_engine.hpp"
#include <asterion/domain/order_limits.hpp>
#include <asterion/protocol/task_execution.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/foundation/error.hpp>

namespace asterion::tasks {

task::proto::TaskFinish execute(
    task::proto::TaskAttempt attempt,
    const std::filesystem::path& directory,
    std::stop_token stop,
    const TaskProgress& progress,
    const std::string& loaded_program)
{
    if ((attempt.task().kind() == task::proto::BACKTEST
            || attempt.task().kind() == task::proto::FACTOR)
        && !attempt.task().program_artifact().empty()
        && attempt.task().program_artifact() != loaded_program)
        throw Error(ErrorCode::conflict, "calculation requires its fixed execution program");
    resolve_task_input(attempt, stop);
    task::proto::TaskFinish result;
    switch (attempt.task().kind()) {
        case task::proto::BACKTEST: {
            *result.mutable_result() = backtest::run(attempt.task().input(), stop, progress);
            break;
        }
        case task::proto::FACTOR:
            *result.mutable_factor() = factor::run(attempt.task().factor(), stop, progress);
            break;
        case task::proto::LOGIC_TRIAL:
            if (loaded_program.empty() || loaded_program != attempt.task().program_artifact())
                throw Error(
                    ErrorCode::conflict,
                    "calculation requires its fixed execution program");
            *result.mutable_logic_trial() =
                compute_logic_trial(attempt.task().logic_trial(), stop, progress);
            break;
        case task::proto::SAMPLE:
            if (loaded_program.empty() || loaded_program != attempt.task().program_artifact())
                throw Error(
                    ErrorCode::conflict,
                    "calculation requires its fixed execution program");
            *result.mutable_sample() = capture_sample(attempt.task().sample(), stop, progress);
            break;
        case task::proto::PERFORMANCE_EVALUATION:
            if (loaded_program.empty() || loaded_program != attempt.task().program_artifact())
                throw Error(
                    ErrorCode::conflict,
                    "calculation requires its fixed execution program");
            *result.mutable_evaluation() =
                compute_evaluation(attempt.task().evaluation(), directory, stop, progress);
            break;
        case task::proto::STATISTICS:
            if (loaded_program.empty() || loaded_program != attempt.task().program_artifact())
                throw Error(
                    ErrorCode::conflict,
                    "calculation requires its fixed execution program");
            *result.mutable_statistics() =
                compute_statistics(attempt.task().statistics(), directory, stop, progress);
            break;
        default:
            throw std::invalid_argument("unsupported task kind");
    }
    result.set_id(attempt.task().id());
    result.set_token(attempt.token());
    return result;
}

} // namespace asterion::tasks
