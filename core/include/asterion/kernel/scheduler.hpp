#pragma once
#include <asterion/foundation/time.hpp>
#include <exception>
#include <functional>
#include <map>
#include <vector>
namespace asterion {
// Serialized control-plane scheduler. Clock is injected; no wall-clock sleeps.
// Tasks scheduled from a callback are deferred until the next run_due call.
class Scheduler final {
public:
    using TaskId = std::uint64_t;
    explicit Scheduler(Clock& clock, std::size_t capacity = 256) : clock_(clock), capacity_(capacity) {
        if (!capacity) throw Error(ErrorCode::invalid_request, "invalid scheduler capacity");
    }
    TaskId after(Nanoseconds delay, std::function<void()> callback) {
        if (closed_) throw Error(ErrorCode::unavailable, "scheduler closed");
        if (!callback) throw Error(ErrorCode::invalid_request, "empty scheduled task");
        if (tasks_.size() >= capacity_ || next_ == std::numeric_limits<TaskId>::max()) throw Error(ErrorCode::resource_exhausted, "scheduler full");
        const auto due = checked_time_add(clock_.monotonic_now(), delay);
        const auto id = next_++;
        tasks_.emplace(std::pair{due, id}, std::move(callback));
        return id;
    }
    bool cancel(TaskId id) {
        for (auto it = tasks_.begin(); it != tasks_.end(); ++it) if (it->first.second == id) { tasks_.erase(it); return true; }
        return false;
    }
    std::size_t run_due(std::size_t budget = 64) {
        if (running_) throw Error(ErrorCode::conflict, "recursive scheduler pump");
        if (closed_) return 0;
        std::vector<std::pair<Nanoseconds, TaskId>> due;
        const auto now = clock_.monotonic_now();
        for (const auto& [key, callback] : tasks_) {
            static_cast<void>(callback);
            if (key.first > now || due.size() >= budget) break;
            due.push_back(key);
        }
        running_ = true;
        std::exception_ptr first_error;
        std::size_t count = 0;
        for (auto key : due) {
            auto node = tasks_.extract(key);
            if (node.empty()) continue; // An earlier callback may cancel this one.
            ++count;
            try { node.mapped()(); }
            catch (...) { if (!first_error) first_error = std::current_exception(); }
        }
        running_ = false;
        if (first_error) std::rethrow_exception(first_error);
        return count;
    }
    void close() noexcept { closed_ = true; tasks_.clear(); }
    std::size_t pending() const noexcept { return tasks_.size(); }
private:
    Clock& clock_;
    std::size_t capacity_;
    std::map<std::pair<Nanoseconds, TaskId>, std::function<void()>> tasks_;
    TaskId next_ = 1;
    bool running_ = false, closed_ = false;
};
}
