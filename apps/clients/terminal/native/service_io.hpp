#pragma once
#include <asterion/kernel/ipc/rpc_client.hpp>
#include <asterion/kernel/polled_task.hpp>
#include <asterion/kernel/payload_budget.hpp>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <stop_token>
#include <type_traits>

namespace asterion::terminal {
// One Terminal owner advances service operations. Callbacks must only do bounded
// state work or suspend; blocking management and large reads belong to their pools.
// The owner advances them when a client's socket has made progress, when pool
// work has finished and when an operation is admitted, and otherwise every
// 2 ms, which is how it observes deadlines and stop requests.
// Admitted operations retain their captures even if the returned future is dropped.
// Client destruction requests cancellation without waiting on this owner; destroy
// ServiceIo after its clients to drain all admitted work and release transports.
class ServiceIo final {
public:
  static constexpr std::size_t application_capacity = 24;
  enum class Lane { request, administration, observation, application };
  enum class ReadLane { data, response };
  enum class PayloadLane { data, control, administration };
  PayloadBudget payload_budget(PayloadLane lane) const;
  // Where every client of this owner creates its transports.
  ipc::Reactor& reactor();
  ServiceIo();
  ~ServiceIo();
  ServiceIo(const ServiceIo&) = delete;
  ServiceIo& operator=(const ServiceIo&) = delete;
  template <class T>
  std::future<T> submit(std::function<PolledTask<T>(std::stop_token)> operation,
                        Lane lane = Lane::request) {
    auto result = std::make_shared<std::promise<T>>();
    auto future = result->get_future();
    enqueue(
        [operation = std::move(operation), result](std::stop_token stop) mutable {
          return complete<T>(std::move(operation), result, stop);
        },
        [result](std::exception_ptr error) { result->set_exception(error); }, lane);
    return future;
  }

  // Called by an owner coroutine. The suspended frame owns both the immutable
  // processing function and its result until the bounded read job has completed.
  // Response preparation has reserved CPU capacity, separate from bulk reads.
  template <class T>
  PolledTask<T> read(std::function<T()> process, ReadLane lane = ReadLane::data) {
    if constexpr (std::is_void_v<T>) {
      co_await PollFuture{read_work(std::move(process), lane)};
    } else {
      std::optional<T> result;
      co_await PollFuture{read_work([&] { result.emplace(process()); }, lane)};
      co_return std::move(*result);
    }
  }

  // File, keychain and other slow management work has its own bounded workers;
  // it cannot consume the result-reading pool or block the I/O owner.
  template <class T> PolledTask<T> admin(std::function<T()> process) {
    if constexpr (std::is_void_v<T>) {
      co_await PollFuture{admin_work(std::move(process))};
    } else {
      std::optional<T> result;
      co_await PollFuture{admin_work([&] { result.emplace(process()); })};
      co_return std::move(*result);
    }
  }

private:
  template <class T>
  static PolledTask<void> complete(std::function<PolledTask<T>(std::stop_token)> operation,
                                   std::shared_ptr<std::promise<T>> result, std::stop_token stop) {
    if constexpr (std::is_void_v<T>) {
      co_await operation(stop);
      result->set_value();
    } else
      result->set_value(co_await operation(stop));
  }
  std::future<void> read_work(std::function<void()>, ReadLane);
  std::future<void> admin_work(std::function<void()>);
  void enqueue(std::function<PolledTask<void>(std::stop_token)>,
               std::function<void(std::exception_ptr)>, Lane);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace asterion::terminal
