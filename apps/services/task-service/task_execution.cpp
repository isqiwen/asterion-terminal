#include "task_execution.hpp"
#include "backtest/engine.hpp"
#include "factor/factor_engine.hpp"
#include "risk_module.hpp"

namespace asterion::tasks {

task::proto::TaskFinish execute(
    task::proto::TaskAttempt attempt,
    const std::filesystem::path& directory,
    const std::string& service,
    std::stop_token stop,
    const TaskProgress& progress)
{
    resolve_task_input(attempt, stop);
    task::proto::TaskFinish result;
    switch (attempt.task().kind()) {
        case task::proto::BACKTEST: {
            const auto module = risk_providers::Module::pinned(
                directory / attempt.task().id(),
                attempt.task().risk_artifact());
            *result.mutable_result() =
                backtest::run(attempt.task().input(), stop, progress, &module);
            break;
        }
        case task::proto::FACTOR:
            *result.mutable_factor() = factor::run(attempt.task().factor(), stop, progress);
            break;
        case task::proto::MINUTE_DOWNLOAD:
        case task::proto::TABLE_UPDATE:
            result = download(attempt, service, stop, progress);
            break;
        default:
            throw std::invalid_argument("unsupported task kind");
    }
    result.set_id(attempt.task().id());
    result.set_token(attempt.token());
    return result;
}

} // namespace asterion::tasks
