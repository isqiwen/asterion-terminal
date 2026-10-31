#pragma once
// Internal to the Research host implementation files.
#include <asterion/research.pb.h>
#include "research_host.hpp"
#include "research_submission.hpp"
#include "variable_store.hpp"
#include "logic_store.hpp"
#include "strategy_store.hpp"
#include <asterion/protocol/research_strategy.hpp>
#include <asterion/protocol/research_variable.hpp>
#include "research_execution.hpp"
#include "research_statistics.hpp"
#include "research_evaluation.hpp"
#include <asterion/protocol/research_statistics.hpp>
#include <asterion/protocol/research_evaluation.hpp>
#include <asterion/protocol/research_sample.hpp>
#include <asterion/protocol/research_logic.hpp>
#include "task_payload.hpp"
#include "program_files.hpp"
#include <asterion/protocol/resource.hpp>
#include <unistd.h>
#include <limits>
#include <asterion/futures/order_limits.hpp>
#include <asterion/system/log.hpp>
#include <asterion/system/process/child.hpp>
#include <asterion/system/process/owner.hpp>
#include <asterion/system/trace.hpp>
#include <asterion/protocol/data.hpp>
#include <asterion/protocol/data_client.hpp>
#include <asterion/protocol/factor.hpp>
#include <asterion/protocol/task_execution.hpp>
#include <asterion/system/process/artifact.hpp>
#include <set>
#include <asterion/protocol/rpc_diagnostics.hpp>
#include <algorithm>
#include <stdexcept>

namespace asterion::tasks {

using namespace std::chrono_literals;
namespace wire = task::proto;

// The existing task an operation names. Submissions and retries are admitted
// by the store itself and wait for no earlier change of their own.
inline std::string named_task(const research::proto::ResearchRequest& request)
{
    if (request.has_recover_submission())
        return request.recover_submission().id();
    if (request.has_abandon_preparation())
        return request.abandon_preparation().id();
    if (request.has_release_decision())
        return request.release_decision().id();
    if (request.has_retirement())
        return request.retirement().id();
    if (request.has_metadata_get())
        return request.metadata_get().id();
    if (request.has_metadata_edit())
        return request.metadata_edit().id();
    if (request.has_get())
        return request.get().id();
    if (request.has_cancel())
        return request.cancel().id();
    if (request.has_result())
        return request.result().id();
    return {};
}

// New work is not admitted once an upgrade has asked the service to quiesce.
inline void
require_fixed_owner(const data::proto::DataOwner& selected, const data::proto::DataOwner& fixed)
{
    validate_id(fixed.node_id());
    validate_id(fixed.service_id());
    if ((!selected.node_id().empty() && selected.node_id() != fixed.node_id())
        || (!selected.service_id().empty() && selected.service_id() != fixed.service_id()))
        throw Error(ErrorCode::conflict, "fixed experiment cannot change its data owner");
}

inline void refuse_during_upgrade(bool quiescing)
{
    if (quiescing)
        throw Error(ErrorCode::unavailable, "research service is preparing for upgrade");
}

inline void check_submission_shape(const research::proto::ResearchSubmit& submit)
{
    if (submit.has_strategy_use() && !submit.has_backtest_reuse() && !submit.has_backtest())
        throw std::invalid_argument("strategy delivery requires explicit backtest admission");
}

inline void check_retry(const wire::Task& task)
{
    switch (task.kind()) {
        case wire::BACKTEST:
        case wire::FACTOR:
        case wire::STATISTICS:
        case wire::PERFORMANCE_EVALUATION:
        case wire::SAMPLE:
        case wire::LOGIC_TRIAL:
            return;
        default:
            throw Error(ErrorCode::conflict, "acquisition belongs to the data service");
    }
}

// Requests that change durable state are refused once an upgrade asked to quiesce.
inline bool changes_state(const research::proto::ResearchRequest& request)
{
    return request.has_submit() || request.has_metadata_edit() || request.has_retry()
        || request.has_recover_submission() || request.has_abandon_preparation()
        || (request.has_retirement() && request.retirement().execute());
}

// Requests that prepare or recover fixed input hold one bounded preparation slot.
inline bool prepares_input(const research::proto::ResearchRequest& request)
{
    return request.has_submit() || request.has_recover_submission()
        || request.has_abandon_preparation() || request.has_retirement()
        || (request.has_strategy() && request.strategy().has_mutation()
            && (request.strategy().mutation().has_freeze()
                || request.strategy().mutation().has_logic()));
}

struct ResearchHost::Call
{
    research::proto::ResearchRequest request;
    wire::TaskResponse response;
    wire::TaskAttempt attempt;
    Lane lane = Lane::client;
    Clock::time_point deadline, verification_deadline, input_deadline;
    std::string task_id;
    mutable std::optional<ResearchDataRoute> data_route;

    void admitted() const
    {
        if ((lane != Lane::health && service::stop_requested()) || Clock::now() >= deadline)
            throw Error(ErrorCode::unavailable, "task request admission timed out");
    }

    void input_in_time() const
    {
        if (service::stop_requested() || Clock::now() >= input_deadline)
            throw Error(ErrorCode::unavailable, "task input preparation timed out");
    }

    void verified_in_time() const
    {
        if (service::stop_requested() || Clock::now() >= verification_deadline)
            throw Error(ErrorCode::unavailable, "task result verification timed out");
    }
};

template <class F>
PolledTask<Store::Change> ResearchHost::submit(const Call& call, F make_input, bool prepared_files)
{
    const auto id = call.request.has_submit() ? call.request.submit().id()
                                              : call.request.recover_submission().id();
    std::optional<Store::Submission> input;
    co_await file_work([&] {
        input.emplace(make_input());
    });
    co_await ready(call);
    call.input_in_time();
    refuse_during_upgrade(quiescing_);
    storage_->admit_submission(*input);
    try {
        data::proto::DataRetention retention;
        co_await file_work([&] {
            if (!prepared_files)
                input->prepare_files();
            if (input->execution().has_statistics() || input->execution().has_evaluation()
                || input->execution().has_logic_trial())
                return; // Owned fixed evidence; source domains retain their formal publications.
            if (!call.data_route)
                throw std::logic_error("research submission has no resolved data owner");
            retention = research_retention(
                input->execution(),
                *call.data_route,
                configuration_.service,
                id);
            save_research_retention(configuration_.directory / id, retention);
        });
        if (input->execution().has_statistics() || input->execution().has_evaluation()
            || input->execution().has_logic_trial()) {
            co_await ready(call);
            if (input->execution().has_logic_trial()) {
                co_await wait_for_store({}, call.deadline);
                const auto fixed = input->execution().logic_trial();
                research::Logic(*storage_, fixed.owner()).verify_input(fixed, true);
            }
            co_return co_await commit(storage_->register_submission(*input));
        }
        data::proto::DataRequest retain;
        *retain.mutable_retention()->mutable_prepare() = retention;
        const auto retained = co_await data_call(
            *call.data_route,
            std::move(retain),
            5s,
            call.request.correlation_id());
        if (retained.retention().retention().SerializeAsString() != retention.SerializeAsString()
            || (retained.retention().phase() != data::proto::DataRetentionRecord::PREPARED
                && retained.retention().phase() != data::proto::DataRetentionRecord::CONFIRMED))
            throw std::invalid_argument("data did not confirm retention of the fixed inputs");
        // Admission has reserved the task's capacity and order. Complete
        // that accepted submission even if its client deadline has passed.
        co_await ready(call);
        if (input->execution().has_strategy_use()) {
            co_await wait_for_store({}, call.deadline);
            research::Strategies(*storage_, input->execution().strategy_use().package().owner())
                .verify_use(input->execution().strategy_use(), id, true);
        }
        co_return co_await commit(storage_->register_submission(*input));
    } catch (...) {
        if (storage_->abandon_submission(*input))
            degraded_ = true;
        throw;
    }
}

} // namespace asterion::tasks
