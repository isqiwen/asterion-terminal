#pragma once
#include <asterion/foundation/error.hpp>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
namespace asterion {
class BrokerSendGate;
// Move-only, bound to one account gate and one prepared order. Destruction
// abandons an unused grant. Consumption and invalidation share one atomic state.
class BrokerSendPermit {
public:
  BrokerSendPermit() = default;
  BrokerSendPermit(const BrokerSendPermit&) = delete;
  BrokerSendPermit& operator=(const BrokerSendPermit&) = delete;
  BrokerSendPermit(BrokerSendPermit&& other) noexcept
      : state_(std::move(other.state_)), order_id_(std::move(other.order_id_)),
        armed_(other.armed_) {}
  BrokerSendPermit& operator=(BrokerSendPermit&& other) noexcept {
    if (this != &other) {
      reset();
      state_ = std::move(other.state_);
      order_id_ = std::move(other.order_id_);
      armed_ = other.armed_;
    }
    return *this;
  }
  ~BrokerSendPermit() { reset(); }
  bool consume(std::string_view order_id) noexcept {
    if (order_id != order_id_) {
      reset();
      return false;
    }
    auto state = std::exchange(state_, {});
    auto expected = armed_;
    return state && state->compare_exchange_strong(expected, armed_ & ~std::uint64_t{1});
  }

private:
  friend class BrokerSendGate;
  BrokerSendPermit(std::shared_ptr<std::atomic<std::uint64_t>> state, std::string order_id,
                   std::uint64_t armed) noexcept
      : state_(std::move(state)), order_id_(std::move(order_id)), armed_(armed) {}
  void reset() noexcept {
    if (auto state = std::exchange(state_, {})) {
      auto expected = armed_;
      state->compare_exchange_strong(expected, armed_ & ~std::uint64_t{1});
    }
  }
  std::shared_ptr<std::atomic<std::uint64_t>> state_;
  std::string order_id_;
  std::uint64_t armed_ = 0;
};
class BrokerSendGate {
public:
  BrokerSendGate() : state_(std::make_shared<std::atomic<std::uint64_t>>(0)) {}
  BrokerSendGate(const BrokerSendGate&) = delete;
  BrokerSendGate& operator=(const BrokerSendGate&) = delete;
  std::uint64_t revision() const noexcept { return state_->load() >> 2; }
  void invalidate() noexcept {
    auto state = state_->load();
    while (!state_->compare_exchange_weak(state, (((state >> 2) + 1) << 2) | (state & 2))) {
    }
  }
  // Producers close the gate before publishing an event that changes its basis.
  void pending() noexcept {
    auto state = state_->load();
    while (!state_->compare_exchange_weak(state, (((state >> 2) + 1) << 2) | 2)) {
    }
  }
  // The state owner calls this only after applying the complete ingress prefix.
  // A later arrival cannot be acknowledged by an older completion.
  void acknowledge(std::uint64_t revision) noexcept {
    auto expected = (revision << 2) | 2;
    state_->compare_exchange_strong(expected, revision << 2);
  }
  BrokerSendPermit issue(std::uint64_t revision, std::string order_id) {
    auto expected = revision << 2;
    if (!state_->compare_exchange_strong(expected, (revision << 2) | 1))
      throw Error(ErrorCode::conflict,
                  "order permission changed before dispatch; the order was not sent");
    return BrokerSendPermit(state_, std::move(order_id), (revision << 2) | 1);
  }

private:
  // Low bits: armed grant, unapplied ingress. Remaining bits: revision.
  std::shared_ptr<std::atomic<std::uint64_t>> state_;
};
} // namespace asterion
