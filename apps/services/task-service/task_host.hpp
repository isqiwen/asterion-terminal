#pragma once
#include "task_store.hpp"
#include "verification_slots.hpp"
#include <asterion/kernel/ipc/rpc_client.hpp>
#include <asterion/kernel/polled_task.hpp>
#include <asterion/kernel/rpc_host.hpp>
#include <asterion/kernel/thread_pool.hpp>
#include <asterion/v1/data_service.pb.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
namespace asterion::tasks {
// The task service's state and I/O owner. It decides task state through Store,
// hands each durable change to one journal thread and file work to a fixed
// pool, and calls its bound Data service. Every member runs on the RpcHost I/O
// thread; a request suspends there while its file, journal or RPC work is pending.
class TaskHost {
public:
  struct Configuration {
    std::filesystem::path directory;
    std::string service, data_instance, data_endpoint;
    std::chrono::seconds worker_timeout{30};
    unsigned file_workers = 2;
  };
  // The endpoint a request arrived on. Dispatch needs the private worker
  // endpoint; upgrade control needs the private health endpoint.
  enum class Lane { client, worker, health };
  explicit TaskHost(Configuration configuration);
  TaskHost(const TaskHost&) = delete;
  TaskHost& operator=(const TaskHost&) = delete;
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
  struct Lease {
    std::string token;
    Clock::time_point expires;
  };
  struct Writing {
    Store::Change change;
    std::future<void> result;
    std::exception_ptr error;
    bool done = false;
  };
  // Durable changes: at most one unconfirmed change per task.
  void collect_writes();
  std::shared_ptr<Writing> write(Store::Change change);
  PolledTask<Store::Change> commit(Store::Change change);
  // Active controls wait only for their own journal change. Cold database
  // access waits for the owned connection without blocking the I/O thread.
  PolledTask<> wait_for_store(std::string id = {});
  template <class F> auto file_work(F work) {
    return PollFuture{files_.submit([work = std::move(work)](std::stop_token) { work(); })};
  }
  PolledTask<data::v1::DataResponse> data_call(data::v1::DataRequest request,
                                               std::chrono::milliseconds timeout,
                                               std::string parent = {});
  // The I/O owner observes lease expiry even while file, RPC or journal work waits.
  void expire_leases();
  // Only original durable publication decisions are retried. No worker is rerun.
  PolledTask<> publish();

  PolledTask<std::string> serve(std::string frame, Lane lane);
  PolledTask<task::v1::TaskResponse> respond(std::string frame, Lane lane,
                                             Clock::time_point deadline);
  PolledTask<> handle(Call& call, std::string frame);
  // The task's own pending change has settled and a worker report still holds its lease.
  PolledTask<Clock::time_point> ready(const Call& call);
  PolledTask<data::v1::DataResponse> data_input(const Call& call, data::v1::DataRequest query);
  template <class F> PolledTask<Store::Change> submit(const Call& call, F make_input);
  void quiesce(Call& call);
  PolledTask<> history_usage(Call& call);
  PolledTask<> dispatch(Call& call);
  PolledTask<> submit_download(Call& call);
  PolledTask<> submit_daily_factor(Call& call);
  PolledTask<> submit_backtest(Call& call);
  PolledTask<> submit_factor(Call& call);
  PolledTask<> claim(Call& call);
  PolledTask<> finish(Call& call);

  const Configuration configuration_;
  const Clock::time_point started_ = Clock::now();
  const std::string process_;
  std::unique_ptr<Store> storage_;
  std::future<void> opening_;
  VerificationSlots verification_slots_, input_slots_;
  bool quiescing_ = false, degraded_ = false, initialized_ = false;
  ipc::RpcClient data_;
  std::map<std::string, Lease> leases_;
  std::atomic<bool> journal_failed_{false};
  std::map<std::string, std::shared_ptr<Writing>> writing_;
  static constexpr std::size_t journal_capacity = 16;
  // Requests wait on these pools; finished work prompts the I/O owner.
  ThreadPool journal_{1, journal_capacity, service::wake_io_owner};
  ThreadPool files_;
  bool publication_wake_ = true;
  std::optional<PolledTask<>> publication_;
  Clock::time_point next_publication_ = Clock::now();
};
} // namespace asterion::tasks
