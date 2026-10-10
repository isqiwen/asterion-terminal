#include "service_io.hpp"
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/trace.hpp>
#include <asterion/kernel/thread_pool.hpp>
#include <array>
#include <deque>
#include <list>
#include <mutex>
#include <numeric>
#include <thread>

namespace asterion::terminal {
struct ServiceIo::Impl {
  const std::array<PayloadBudget, 3> payloads{PayloadBudget{128 * 1024 * 1024},
                                              PayloadBudget{128 * 1024 * 1024},
                                              PayloadBudget{8 * 1024 * 1024}};
  struct Pending {
    std::function<PolledTask<void>(std::stop_token)> begin;
    std::size_t lane;
    std::string trace;
    std::function<void(std::exception_ptr)> fail;
    std::optional<PolledTask<void>> task;
  };
  // Includes suspended operations, not just the incoming queue.
  static constexpr std::array<std::size_t, 4> capacity{32, 8, 64, ServiceIo::application_capacity};
  std::mutex mutex;
  std::deque<std::unique_ptr<Pending>> incoming;
  std::array<std::size_t, 4> admitted{};
  bool closing = false;
  // The owner waits here; sockets, finished pool work and admissions end the wait.
  ipc::Reactor reactor;
  const std::function<void()> finished = [this] { reactor.wake(); };
  ThreadPool readers{2, 8, finished};
  // One response job per admitted operation; bulk reads cannot consume these slots.
  ThreadPool responses{1, std::accumulate(capacity.begin(), capacity.end(), std::size_t{0}),
                       finished};
  ThreadPool administrators{2, 8, finished};
  std::jthread owner;
  Impl() : owner([this](std::stop_token stop) { run(stop); }) {}
  // Returns once every admitted operation has finished on the owner.
  void stop() {
    {
      std::lock_guard lock(mutex);
      closing = true;
    }
    owner.request_stop();
    reactor.wake();
    owner.join();
  }
  void run(std::stop_token stop) {
    std::list<std::unique_ptr<Pending>> active;
    for (;;) {
      {
        std::lock_guard lock(mutex);
        while (!incoming.empty()) {
          active.push_back(std::move(incoming.front()));
          incoming.pop_front();
        }
      }
      for (auto item = active.begin(); item != active.end();) {
        auto& operation = **item;
        TraceScope trace(operation.trace);
        try {
          if (!operation.task)
            operation.task.emplace(operation.begin(stop));
          if (!operation.task->poll()) {
            ++item;
            continue;
          }
          operation.task->take();
        } catch (...) {
          operation.fail(std::current_exception());
        }
        const auto lane = operation.lane;
        item = active.erase(item);
        std::lock_guard lock(mutex);
        --admitted[lane];
      }
      {
        std::lock_guard lock(mutex);
        if (stop.stop_requested() && active.empty() && incoming.empty())
          return;
      }
      // An admission or a stop request after the check above has already
      // woken the reactor, so this wait returns for it.
      reactor.wait(active.empty() ? std::nullopt : std::optional(std::chrono::milliseconds(2)));
    }
  }
};
ServiceIo::ServiceIo() : impl_(std::make_unique<Impl>()) {}
// Operations still running on the owner submit work through this object, so
// the owner is stopped while impl_ still points at its pools. Destroying
// impl_ first would clear the pointer before the owner had finished.
ServiceIo::~ServiceIo() {
  impl_->stop();
}
PayloadBudget ServiceIo::payload_budget(PayloadLane lane) const {
  return impl_->payloads[static_cast<std::size_t>(lane)];
}
ipc::Reactor& ServiceIo::reactor() {
  return impl_->reactor;
}
std::future<void> ServiceIo::read_work(std::function<void()> process, ReadLane lane) {
  auto& pool = lane == ReadLane::response ? impl_->responses : impl_->readers;
  return pool.submit(
      [process = std::move(process), trace = std::string(current_trace_id())](std::stop_token) {
        TraceScope context(trace);
        process();
      });
}
std::future<void> ServiceIo::admin_work(std::function<void()> process) {
  return impl_->administrators.submit(
      [process = std::move(process), trace = std::string(current_trace_id())](std::stop_token) {
        TraceScope context(trace);
        process();
      });
}
void ServiceIo::enqueue(std::function<PolledTask<void>(std::stop_token)> operation,
                        std::function<void(std::exception_ptr)> fail, Lane lane) {
  auto pending = std::make_unique<Impl::Pending>();
  pending->begin = std::move(operation);
  pending->lane = static_cast<std::size_t>(lane);
  pending->trace = lane == Lane::observation ? std::string{} : std::string(current_trace_id());
  pending->fail = std::move(fail);
  {
    std::lock_guard lock(impl_->mutex);
    const auto slot = pending->lane;
    if (impl_->closing || impl_->admitted[slot] == Impl::capacity[slot])
      throw Error(ErrorCode::resource_exhausted, "Terminal service operation capacity reached");
    impl_->incoming.push_back(std::move(pending));
    ++impl_->admitted[slot];
  }
  impl_->reactor.wake();
}
} // namespace asterion::terminal
