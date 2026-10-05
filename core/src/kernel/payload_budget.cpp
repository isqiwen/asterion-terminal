#include <asterion/kernel/payload_budget.hpp>
#include <asterion/foundation/error.hpp>
#include <atomic>
#include <stdexcept>
namespace asterion {
struct PayloadBudget::State {
  const std::size_t limit;
  std::atomic<std::size_t> used{0};
  explicit State(std::size_t limit) : limit(limit) {}
  struct Buffer {
    std::shared_ptr<State> owner;
    std::size_t charge = 0;
    std::string bytes;
    explicit Buffer(std::shared_ptr<State> owner) : owner(std::move(owner)) {}
    ~Buffer() {
      std::string{}.swap(bytes);
      owner->used.fetch_sub(charge, std::memory_order_relaxed);
    }
  };
  void reserve(std::size_t bytes) {
    auto current = used.load(std::memory_order_relaxed);
    do {
      if (bytes > limit - current)
        throw Error(ErrorCode::resource_exhausted, "IPC payload capacity reached");
    } while (!used.compare_exchange_weak(current, current + bytes, std::memory_order_relaxed));
  }
};
PayloadBudget::PayloadBudget(std::size_t limit) : state_(std::make_shared<State>(limit)) {
  if (!limit)
    throw std::invalid_argument("payload budget must be positive");
}
std::size_t PayloadBudget::limit() const {
  return state_->limit;
}
std::shared_ptr<std::string> PayloadBudget::allocate(std::size_t bytes) const {
  auto buffer = std::make_shared<State::Buffer>(state_);
  state_->reserve(bytes);
  buffer->charge = bytes;
  buffer->bytes.resize(bytes);
  return {buffer, &buffer->bytes};
}
Payload PayloadBudget::retain(std::string bytes) const {
  auto buffer = std::make_shared<State::Buffer>(state_);
  state_->reserve(bytes.size());
  buffer->charge = bytes.size();
  buffer->bytes = std::move(bytes);
  return {buffer, &buffer->bytes};
}
} // namespace asterion
