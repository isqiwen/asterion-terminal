#pragma once
#include <asterion/foundation/error.hpp>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <stop_token>
namespace asterion {
// Multi-producer/multi-consumer. Close rejects producers but permits draining.
// Owners must join consumers before destroying the queue.
template<class T> class BoundedQueue final {
public:
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {
        if (!capacity) throw Error(ErrorCode::invalid_request, "queue capacity must be positive");
    }
    bool try_push(T value) {
        std::lock_guard lock(mutex_);
        if (closed_ || queue_.size() >= capacity_) return false;
        queue_.push_back(std::move(value));
        ready_.notify_one();
        return true;
    }
    std::optional<T> try_pop() {
        std::lock_guard lock(mutex_);
        return pop_locked();
    }
    std::optional<T> wait_pop(std::stop_token stop = {}) {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, stop, [&] { return closed_ || !queue_.empty(); });
        if (stop.stop_requested()) return std::nullopt;
        return pop_locked();
    }
    void close() { std::lock_guard lock(mutex_); closed_ = true; ready_.notify_all(); }
private:
    std::optional<T> pop_locked() {
        if (queue_.empty()) return std::nullopt;
        T value = std::move(queue_.front()); queue_.pop_front(); return value;
    }
    std::size_t capacity_;
    std::mutex mutex_;
    std::condition_variable_any ready_;
    std::deque<T> queue_;
    bool closed_ = false;
};
}
