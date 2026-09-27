#pragma once
#include <asterion/foundation/bounded_queue.hpp>
#include <asterion/kernel/event_bus.hpp>
#include <vector>
namespace asterion {
// Producers may post concurrently; subscriptions and dispatch are serialized by
// the host. Overflow is explicit; callback failures do not cause automatic replay.
template <class Event> class MessageBus final {
public:
  explicit MessageBus(std::size_t capacity = 256) : queue_(capacity) {}
  auto subscribe(typename EventBus<Event>::Handler callback) {
    return bus_.subscribe(std::move(callback));
  }
  bool unsubscribe(typename EventBus<Event>::Subscription id) { return bus_.unsubscribe(id); }
  bool post(Event event) { return queue_.try_push(std::move(event)); }
  std::size_t dispatch(std::size_t budget = 64) {
    if (dispatching_)
      throw Error(ErrorCode::conflict, "recursive message dispatch");
    if (closed_)
      return 0;
    std::vector<Event> batch;
    while (batch.size() < budget) {
      auto event = queue_.try_pop();
      if (!event)
        break;
      batch.push_back(std::move(*event));
    }
    dispatching_ = true;
    std::exception_ptr first_error;
    std::size_t delivered = 0;
    for (const auto& event : batch) {
      if (closed_)
        break;
      ++delivered;
      try {
        bus_.publish(event);
      } catch (...) {
        if (!first_error)
          first_error = std::current_exception();
      }
    }
    dispatching_ = false;
    if (first_error)
      std::rethrow_exception(first_error);
    return delivered;
  }
  // Close discards queued events. An event already being delivered completes.
  void close() {
    closed_ = true;
    queue_.close();
    while (queue_.try_pop()) {
    }
  }

private:
  BoundedQueue<Event> queue_;
  EventBus<Event> bus_;
  bool dispatching_ = false, closed_ = false;
};
} // namespace asterion
