#pragma once
#include <asterion/foundation/id.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <typeindex>
namespace asterion {
class ResourceRegistry final {
    struct Entry { std::type_index type; std::shared_ptr<void> resource; };
    struct State {
        std::mutex mutex;
        bool active = true;
        std::size_t capacity;
        std::map<std::string, std::shared_ptr<Entry>> entries;
        explicit State(std::size_t limit) : capacity(limit) {}
    };
public:
    template<class T> class Handle {
    public:
        std::shared_ptr<T> lock() const {
            const auto scope = scope_.lock();
            if (!scope) throw Error(ErrorCode::unavailable, "resource scope expired");
            std::lock_guard guard(scope->mutex);
            const auto entry = entry_.lock();
            if (!scope->active || !entry) throw Error(ErrorCode::unavailable, "resource revoked");
            return std::static_pointer_cast<T>(entry->resource);
        }
    private:
        friend class ResourceRegistry;
        Handle(std::weak_ptr<State> scope, std::weak_ptr<Entry> entry) : scope_(scope), entry_(entry) {}
        std::weak_ptr<State> scope_;
        std::weak_ptr<Entry> entry_;
    };
    class Scope final {
    public:
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        Scope(Scope&&) noexcept = default;
        Scope& operator=(Scope&&) = delete;
        ~Scope() { close(); }
        template<class T> void publish(std::string key, std::shared_ptr<T> value) {
            validate_id(key);
            if (!state_ || !value) throw Error(ErrorCode::invalid_request, "invalid resource");
            std::lock_guard guard(state_->mutex);
            if (!state_->active) throw Error(ErrorCode::unavailable, "resource scope closed");
            if (state_->entries.contains(key)) throw Error(ErrorCode::conflict, "duplicate resource");
            if (state_->entries.size() >= state_->capacity) throw Error(ErrorCode::resource_exhausted, "resource scope full");
            state_->entries.emplace(std::move(key), std::make_shared<Entry>(Entry{typeid(T), std::move(value)}));
        }
        void close() noexcept {
            if (!state_) return;
            std::map<std::string, std::shared_ptr<Entry>> retired;
            {
                std::lock_guard guard(state_->mutex);
                state_->active = false;
                retired.swap(state_->entries);
            }
            // Resource destructors may call other services; run them without locks.
        }
    private:
        friend class ResourceRegistry;
        explicit Scope(std::shared_ptr<State> state) : state_(std::move(state)) {}
        std::shared_ptr<State> state_;
    };
    explicit ResourceRegistry(std::size_t scopes = 128, std::size_t per_scope = 256) : capacity_(scopes), per_scope_(per_scope) {
        if (!scopes || !per_scope) throw Error(ErrorCode::invalid_request, "invalid resource limits");
    }
    Scope create_scope(std::string owner) {
        validate_id(owner);
        std::lock_guard lock(mutex_);
        if (closed_) throw Error(ErrorCode::unavailable, "resource registry closed");
        for (auto it = scopes_.begin(); it != scopes_.end();) {
            auto state = it->second.lock();
            bool active = false;
            if (state) { std::lock_guard guard(state->mutex); active = state->active; }
            if (!active) it = scopes_.erase(it); else ++it;
        }
        if (scopes_.contains(owner)) throw Error(ErrorCode::conflict, "resource owner already active");
        if (scopes_.size() >= capacity_) throw Error(ErrorCode::resource_exhausted, "resource registry full");
        auto state = std::make_shared<State>(per_scope_);
        scopes_.emplace(std::move(owner), state);
        return Scope(std::move(state));
    }
    template<class T> Handle<T> resolve(const std::string& owner, const std::string& key) const {
        std::lock_guard lock(mutex_);
        const auto found = scopes_.find(owner);
        auto state = found == scopes_.end() ? nullptr : found->second.lock();
        if (!state) throw Error(ErrorCode::unavailable, "resource owner unavailable");
        std::lock_guard guard(state->mutex);
        const auto entry = state->entries.find(key);
        if (!state->active || entry == state->entries.end()) throw Error(ErrorCode::unavailable, "resource unavailable");
        if (entry->second->type != std::type_index(typeid(T))) throw Error(ErrorCode::invalid_request, "resource type mismatch");
        return Handle<T>(state, entry->second);
    }
    void clear() noexcept {
        std::map<std::string, std::weak_ptr<State>> scopes;
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
            scopes.swap(scopes_);
        }
        for (auto& [owner, weak] : scopes) {
            static_cast<void>(owner);
            if (auto state = weak.lock()) {
                std::map<std::string, std::shared_ptr<Entry>> retired;
                {
                    std::lock_guard guard(state->mutex);
                    state->active = false;
                    retired.swap(state->entries);
                }
            }
        }
    }
    ~ResourceRegistry() { clear(); }
private:
    bool closed_ = false;
    std::size_t capacity_, per_scope_;
    mutable std::mutex mutex_;
    std::map<std::string, std::weak_ptr<State>> scopes_;
};
}
