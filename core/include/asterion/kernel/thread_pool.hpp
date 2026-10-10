#pragma once
#include <asterion/foundation/bounded_queue.hpp>
#include <functional>
#include <future>
#include <thread>
#include <vector>
namespace asterion {
// Concurrent submit; one host owns shutdown/destruction. Running callbacks must
// cooperate with stop_token. Multiple workers do not guarantee completion order.
// `completed`, when given, runs on the worker once a task's result is
// available: an owner that polls those results uses it to look at once
// instead of at its next periodic check.
class ThreadPool final {
  struct Task {
    std::function<void(std::stop_token)> run;
    std::promise<void> completion;
  };

public:
  explicit ThreadPool(std::size_t workers = 2, std::size_t capacity = 256,
                      std::function<void()> completed = {})
      : queue_(capacity), completed_(std::move(completed)) {
    if (!workers || workers > 256)
      throw Error(ErrorCode::invalid_request, "invalid worker count");
    workers_.reserve(workers);
    try {
      for (std::size_t i = 0; i < workers; ++i)
        workers_.emplace_back([this] {
          active_pool_ = this;
          const auto token = stop_.get_token();
          while (auto task = queue_.wait_pop(token)) {
            try {
              task->run(token);
              task->completion.set_value();
            } catch (...) {
              task->completion.set_exception(std::current_exception());
            }
            if (completed_)
              completed_();
          }
          active_pool_ = nullptr;
        });
    } catch (...) {
      stop_.request_stop();
      queue_.close();
      workers_.clear();
      throw;
    }
  }
  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;
  ~ThreadPool() { shutdown(); }
  std::future<void> submit(std::function<void(std::stop_token)> callback) {
    if (!callback)
      throw Error(ErrorCode::invalid_request, "empty task");
    Task task{std::move(callback), {}};
    auto future = task.completion.get_future();
    if (!queue_.try_push(std::move(task)))
      throw Error(ErrorCode::resource_exhausted, "thread pool closed or queue full");
    return future;
  }
  void shutdown() {
    if (active_pool_ == this)
      throw Error(ErrorCode::conflict, "worker cannot join its pool");
    stop_.request_stop();
    queue_.close();
    for (auto& worker : workers_)
      if (worker.joinable())
        worker.join();
    while (auto task = queue_.try_pop())
      task->completion.set_exception(
          std::make_exception_ptr(Error(ErrorCode::cancelled, "task cancelled during shutdown")));
  }

private:
  inline static thread_local const ThreadPool* active_pool_ = nullptr;
  BoundedQueue<Task> queue_;
  const std::function<void()> completed_;
  std::stop_source stop_;
  std::vector<std::jthread> workers_;
};
} // namespace asterion
