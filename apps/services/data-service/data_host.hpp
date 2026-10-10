#pragma once
#include "data_store.hpp"
#include "download_budget.hpp"
#include <asterion/kernel/rpc_host.hpp>
#include <asterion/kernel/thread_pool.hpp>
#include <chrono>
#include <deque>
#include <filesystem>
#include <future>
#include <map>
#include <optional>
namespace asterion::data {
// The data service's state and I/O owner. Requests are ordered per object
// here; large reads and provider calls run on the reader pool and every durable
// change on one writer thread. Only the I/O thread touches ordering and
// visibility: a request keeps its place until durable completion, including
// when its client stops reading.
class DataHost {
public:
  struct Configuration {
    std::filesystem::path directory;
    std::string service, task_instance;
    unsigned file_workers = 2;
  };
  explicit DataHost(Configuration configuration);
  DataHost(const DataHost&) = delete;
  DataHost& operator=(const DataHost&) = delete;
  // Starts opening the store on the writer. Call once the service has claimed
  // its endpoints; requests report "initializing" until it completes.
  void open();
  // worker: the private endpoint that download coordination requires.
  service::RpcHost::Reply accept(std::string frame, bool worker);
  // The private health endpoint: heartbeat and upgrade control only.
  service::RpcHost::Reply health(const std::string& frame);
  bool advance(service::RpcHost::Stage stage);

private:
  struct Work {
    v1::DataRequest request;
    v1::DataResponse response;
    std::function<void()> commit;
    std::future<void> completion;
    std::string frame, publication_dataset;
    v1::StoredDownloadAuthorization authorization;
    std::optional<DownloadBudget::Configuration> budget_configuration;
    enum class Phase { queued, preparing, prepared, writing, complete };
    Phase phase = Phase::queued;
    std::vector<std::string> objects;
    bool reads_catalog = false, writes_catalog = false, creates_dataset = false;
    bool reading_catalog = false, awaiting_catalog_write = false, writing_catalog = false;
  };
  v1::DataRequest parse(const std::string& frame) const;
  v1::DataResponse health_response(const v1::DataRequest& request) const;
  void require_ready() const;
  // The objects whose order this request keeps, and whether it may run here.
  void admit(Work& work, bool worker);
  bool is_turn(const Work& work) const;
  void release(Work& work);
  // Reader pool. Preparation owns large reads and provider calls; a returned
  // commit is handed to the serial writer after preparation releases its slot.
  std::function<void()> prepare(const v1::DataRequest& request, v1::DataResponse& response,
                                std::stop_token stop, bool& creates_dataset,
                                std::string& publication_dataset);
  void write(const std::shared_ptr<Work>& work);
  // One step of a request on the I/O thread; a frame once it has completed.
  std::optional<std::string> step(const std::shared_ptr<Work>& work);
  std::optional<std::string> budget_step(const std::shared_ptr<Work>& work);

  const Configuration configuration_;
  const std::string process_;
  const std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
  bool initialized_ = false, degraded_ = false, quiescing_ = false;
  std::unique_ptr<Store> store_;
  std::unique_ptr<DownloadBudget> budgets_;
  // Requests wait on these pools; finished work prompts the I/O owner.
  ThreadPool writer_{1, 8, service::wake_io_owner};
  ThreadPool readers_;
  std::future<void> opening_;
  std::size_t named_dataset_count_ = 0;
  std::map<std::string, std::deque<const Work*>> object_order_;
  std::size_t catalog_readers_ = 0, catalog_waiting_writers_ = 0;
  bool catalog_writing_ = false;
};
} // namespace asterion::data
