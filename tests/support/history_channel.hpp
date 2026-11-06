#pragma once
#include <asterion/system/ipc/rpc_client.hpp>
#include <deque>

namespace asterion::testing_support {

// Synchronous test view over the production TLS client, including watch frames.
class MarketChannel
{
    struct State
    {
        std::deque<Payload> frames;
        std::future<void> finished;
        ipc::RpcClient client;

        State(std::uint16_t port, ipc::TlsIdentity tls)
            : client("127.0.0.1", port, std::move(tls), 1, PayloadBudget(64 * 1024 * 1024))
        {}
    };

    std::unique_ptr<State> state_;

public:
    MarketChannel(std::uint16_t port, ipc::TlsIdentity tls)
        : state_(std::make_unique<State>(port, std::move(tls)))
    {}

    void send(std::string value, std::chrono::milliseconds timeout)
    {
        auto* s = state_.get();
        s->client.cancel();
        s->client.poll();
        s->frames.clear();
        s->finished = s->client.watch(std::move(value), timeout, [s](Payload frame) {
            s->frames.push_back(std::move(frame));
        });
    }

    std::string receive(std::chrono::milliseconds timeout)
    {
        auto& s = *state_;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (s.frames.empty()) {
            s.client.poll(std::chrono::milliseconds(10));
            if (s.frames.empty()
                && s.finished.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                s.finished.get();
                throw std::runtime_error("History closed before a frame");
            }
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("History response timed out");
        }
        auto value = *s.frames.front();
        s.frames.pop_front();
        return value;
    }

    void close()
    {
        state_->client.cancel();
    }
};

} // namespace asterion::testing_support
