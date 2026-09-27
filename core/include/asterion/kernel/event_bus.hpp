#pragma once

#include <cstddef>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <utility>

namespace asterion {

// Synchronous, single-threaded control-plane dispatcher. Not a durable market feed.
// A publish snapshots subscriptions; changes during delivery affect the next event.
// Every handler is invoked in subscription order; the first exception is rethrown
// after delivery. Nested publish is rejected to preserve event order.
template <typename Event> class EventBus final {
public:
  EventBus() = default;
  EventBus(const EventBus&) = delete;
  EventBus& operator=(const EventBus&) = delete;
  using Handler = std::function<void(const Event&)>;
  using Subscription = std::size_t;
  Subscription subscribe(Handler handler) {
    if (!handler)
      throw std::invalid_argument("empty event handler");
    if (next_ == std::numeric_limits<Subscription>::max())
      throw std::overflow_error("subscription IDs exhausted");
    const auto id = next_++;
    handlers_.emplace(id, std::make_shared<Handler>(std::move(handler)));
    return id;
  }
  bool unsubscribe(Subscription id) { return handlers_.erase(id) != 0; }
  void publish(const Event& event) {
    if (publishing_)
      throw std::logic_error("recursive event publication is unsupported");
    const auto snapshot = handlers_;
    publishing_ = true;
    std::exception_ptr first_error;
    for (const auto& [id, handler] : snapshot) {
      static_cast<void>(id);
      try {
        (*handler)(event);
      } catch (...) {
        if (!first_error)
          first_error = std::current_exception();
      }
    }
    publishing_ = false;
    if (first_error)
      std::rethrow_exception(first_error);
  }

private:
  std::map<Subscription, std::shared_ptr<Handler>> handlers_;
  Subscription next_ = 1;
  bool publishing_ = false;
};
} // namespace asterion
