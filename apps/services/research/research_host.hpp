#pragma once
#include "task_store.hpp"
#include "research_data.hpp"
#include "verification_slots.hpp"
#include <asterion/system/ipc/rpc_client.hpp>
#include <asterion/system/polled_task.hpp>
#include <asterion/system/rpc_host.hpp>
#include <asterion/system/thread_pool.hpp>
#include <asterion/data_service.pb.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <optional>

namespace asterion::tasks {

// The research service's state and I/O owner. It decides task state through Store,
// hands each durable change to one journal thread and file work to a fixed
// pool, and resolves each task's Data owner. Every member runs on the RpcHost I/O
// thread; a request suspends there while its file, journal or RPC work is pending.
class ResearchHost
{
public:
    struct Configuration
    {
        std::filesystem::path directory;
        std::string service, node_id, discovery_endpoint;
        unsigned file_workers = 2;
        unsigned task_concurrency = 1;
    };
    // The endpoint a request arrived on; upgrade control requires the private health endpoint.
    enum class Lane
    {
        client,
        health
    };
    explicit ResearchHost(Configuration configuration);
    ResearchHost(const ResearchHost&) = delete;
    ResearchHost& operator=(const ResearchHost&) = delete;
    // Starts opening the store on the file pool. Call once the service has
    // claimed its endpoints; requests report "initializing" until it completes.
    void open();
    // RpcHost retains a pending reply through disconnects. The frame and every
    // suspended local stay alive until accepted file and journal work has finished.
    service::RpcHost::Reply accept(std::string frame, Lane lane);
    // Housekeeping on every I/O turn. True once owned work has stopped.
    bool advance(service::RpcHost::Stage stage);

private:
    using Clock = std::chrono::steady_clock;
    struct Call;

    struct Writing
    {
        Store::Change change;
        std::future<void> result;
        std::exception_ptr error;
        bool done = false;
    };

    // Durable changes: at most one unconfirmed change per task.
    void collect_writes();
    void collect_metadata();
    PolledTask<> variable_operation(Call&);
    PolledTask<> logic_operation(Call&);
    PolledTask<> strategy_operation(Call&);
    PolledTask<> strategy_holders(Call&, task::proto::TaskRetirement&);

    struct MetadataWork
    {
        std::future<void> result;
        research::proto::VariableReply variable;
        research::proto::LogicReply logic;
        research::proto::StrategyReply strategy;
        bool replay = false;
        std::vector<std::string> consumers;
        resource::proto::Catalogue catalogue;
        std::exception_ptr error;
        bool done = false;
    };

    std::shared_ptr<MetadataWork> metadata_work_;
    std::shared_ptr<Writing> write(Store::Change change);
    PolledTask<Store::Change> commit(Store::Change change);
    // Active controls wait only for their own journal change. Cold database
    // access waits for the owned connection without blocking the I/O thread.
    // A deadline bounds admission only. Accepted work must retain its owned state.
    PolledTask<>
    wait_for_store(std::string id = {}, Clock::time_point deadline = Clock::time_point::max());

    template <class F>
    auto file_work(F work)
    {
        return PollFuture{files_.submit([work = std::move(work)](std::stop_token) {
            work();
        })};
    }

    PolledTask<data::proto::DataResponse> data_call(
        const ResearchDataRoute& route,
        data::proto::DataRequest request,
        std::chrono::milliseconds timeout,
        std::string parent = {});

    PolledTask<std::string> serve(std::string frame, Lane lane);
    PolledTask<task::proto::TaskResponse>
    respond(std::string frame, Lane lane, Clock::time_point deadline);
    PolledTask<> handle(Call& call, std::string frame);
    PolledTask<> resources_operation(Call& call);
    PolledTask<> submit_operation(Call& call);
    // Wait for the task's own pending durable change.
    PolledTask<Clock::time_point>
    ready(const Call& call, Clock::time_point deadline = Clock::time_point::max());
    PolledTask<data::proto::DataResponse>
    data_input(const Call& call, data::proto::DataRequest query);
    template <class F>
    PolledTask<Store::Change> submit(const Call& call, F make_input, bool prepared_files = false);
    void quiesce(Call& call);
    PolledTask<> history_usage(Call& call);
    PolledTask<> run_task(std::string id);
    PolledTask<> schedule_tasks();
    PolledTask<> recover_submission(Call& call);
    PolledTask<> abandon_preparation(Call& call);
    PolledTask<> retirement(Call& call);
    void release_decision(Call& call);
    PolledTask<> submit_backtest(Call& call);
    PolledTask<> reuse_backtest(Call& call);
    PolledTask<> submit_factor(Call& call);
    PolledTask<> submit_statistics(Call& call);
    PolledTask<> submit_evaluation(Call& call);
    PolledTask<> submit_sample(Call& call);
    PolledTask<> submit_logic_trial(Call& call);
    PolledTask<research::proto::VariableDefinition>
    resolve_variable(Call&, resource::proto::Object);
    PolledTask<ResearchProgram> program(const Call& call);
    PolledTask<> claim(Call& call);
    PolledTask<> finish(Call& call, const task::proto::TaskFinish& result);

    const Configuration configuration_;
    const Clock::time_point started_ = Clock::now();
    const std::string process_;
    std::unique_ptr<Store> storage_;
    std::future<void> opening_;
    VerificationSlots verification_slots_, input_slots_;
    bool quiescing_ = false, degraded_ = false, initialized_ = false;
    ResearchData data_;
    std::optional<ResearchProgram> loaded_program_;
    std::shared_future<void> program_preservation_;
    std::atomic<bool> journal_failed_{false};
    std::map<std::string, std::shared_ptr<Writing>> writing_;
    static constexpr std::size_t journal_capacity = 16;
    // Requests wait on these pools; finished work prompts the I/O owner.
    ThreadPool journal_{1, journal_capacity, service::wake_io_owner};
    ThreadPool files_;
    std::map<std::string, PolledTask<>> executions_;
    ThreadPool computation_;
    std::optional<PolledTask<>> scheduling_;
    Clock::time_point next_schedule_ = Clock::now();
};

} // namespace asterion::tasks
