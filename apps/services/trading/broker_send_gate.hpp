#pragma once
#include <asterion/base/error.hpp>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace asterion {

class BrokerSendGate;

// Move-only, bound to one account gate and one prepared order. Destruction
// abandons an unused grant.
class BrokerSendPermit
{
public:
    BrokerSendPermit() = default;
    BrokerSendPermit(const BrokerSendPermit&) = delete;
    BrokerSendPermit& operator=(const BrokerSendPermit&) = delete;

    BrokerSendPermit(BrokerSendPermit&& other) noexcept
        : state_(std::move(other.state_)),
          order_id_(std::move(other.order_id_)),
          revision_(other.revision_)
    {}

    BrokerSendPermit& operator=(BrokerSendPermit&& other) noexcept
    {
        if (this != &other) {
            reset();
            state_ = std::move(other.state_);
            order_id_ = std::move(other.order_id_);
            revision_ = other.revision_;
        }
        return *this;
    }

    ~BrokerSendPermit()
    {
        reset();
    }

    bool consume(std::string_view order_id) noexcept
    {
        auto state = std::exchange(state_, {});
        if (!state)
            return false;
        std::lock_guard lock(state->mutex);
        const bool valid = state->armed && !state->pending && state->revision == revision_;
        if (state->revision == revision_)
            state->armed = false;
        return valid && order_id == order_id_;
    }

private:
    friend class BrokerSendGate;

    struct State
    {
        std::mutex mutex;
        std::uint64_t revision = 0;
        bool armed = false;
        bool pending = false;
    };

    BrokerSendPermit(
        std::shared_ptr<State> state,
        std::string order_id,
        std::uint64_t revision) noexcept
        : state_(std::move(state)),
          order_id_(std::move(order_id)),
          revision_(revision)
    {}

    void reset() noexcept
    {
        if (auto state = std::exchange(state_, {})) {
            std::lock_guard lock(state->mutex);
            if (state->revision == revision_)
                state->armed = false;
        }
    }

    std::shared_ptr<State> state_;
    std::string order_id_;
    std::uint64_t revision_ = 0;
};

class BrokerSendGate
{
public:
    BrokerSendGate()
        : state_(std::make_shared<BrokerSendPermit::State>())
    {}

    BrokerSendGate(const BrokerSendGate&) = delete;
    BrokerSendGate& operator=(const BrokerSendGate&) = delete;

    std::uint64_t revision() const noexcept
    {
        std::lock_guard lock(state_->mutex);
        return state_->revision;
    }

    void invalidate() noexcept
    {
        std::lock_guard lock(state_->mutex);
        ++state_->revision;
        state_->armed = false;
    }

    // Producers close the gate before publishing an event that changes its basis.
    void pending() noexcept
    {
        std::lock_guard lock(state_->mutex);
        ++state_->revision;
        state_->armed = false;
        state_->pending = true;
    }

    // The state owner calls this only after applying the complete ingress prefix.
    void acknowledge(std::uint64_t revision) noexcept
    {
        std::lock_guard lock(state_->mutex);
        if (state_->revision == revision)
            state_->pending = false;
    }

    BrokerSendPermit issue(std::uint64_t revision, std::string order_id)
    {
        {
            std::lock_guard lock(state_->mutex);
            if (state_->revision != revision || state_->armed || state_->pending)
                throw Error(
                    ErrorCode::conflict,
                    "order permission changed before dispatch; the order was not sent");
            state_->armed = true;
        }
        return BrokerSendPermit(state_, std::move(order_id), revision);
    }

private:
    std::shared_ptr<BrokerSendPermit::State> state_;
};

} // namespace asterion
