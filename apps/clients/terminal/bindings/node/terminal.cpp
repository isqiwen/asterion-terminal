#include "terminal_application.hpp"
#include "trading_client.hpp"
#include <asterion/base/serialization.hpp>
#include <atomic>
#include <cstring>
#include <node_api.h>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

constexpr unsigned request_limit = 16;
constexpr unsigned snapshot_limit = 8;
constexpr unsigned urgent_limit = 16;
constexpr std::size_t max_request_bytes = 65536;
constexpr std::size_t max_trading_request_bytes = 8 * 1024 * 1024;

enum class Admission
{
    request,
    snapshot,
    urgent
};

struct Runtime
{
    asterion::terminal::Application application;
    std::atomic<unsigned> pending_requests{0}, pending_snapshots{0}, pending_urgent{0};
    unsigned active_environments = 0;
};

using Owner = std::shared_ptr<Runtime>;

struct RuntimeRegistry
{
    std::mutex mutex;
    std::weak_ptr<Runtime> current;
};

RuntimeRegistry& runtime_registry()
{
    static RuntimeRegistry registry;
    return registry;
}

// One Terminal per process: it owns the root and the local services it runs,
// so every Node environment (the main thread and any worker) shares it. It
// stops and goes when the last environment does.
Owner acquire_runtime()
{
    auto& registry = runtime_registry();
    std::lock_guard lock(registry.mutex);
    auto runtime = registry.current.lock();
    if (!runtime || runtime->active_environments == 0) {
        runtime = std::make_shared<Runtime>();
        registry.current = runtime;
    }
    ++runtime->active_environments;
    return runtime;
}

void release_runtime(Owner runtime) noexcept
{
    if (!runtime)
        return;
    bool last_environment = false;
    {
        auto& registry = runtime_registry();
        std::lock_guard lock(registry.mutex);
        if (runtime->active_environments > 0 && --runtime->active_environments == 0) {
            last_environment = true;
            if (registry.current.lock() == runtime)
                registry.current.reset();
        }
    }
    // Cancel in-flight service I/O once the last environment exits, before
    // TSFN finalizers wait on Work::finished.
    if (last_environment)
        runtime->application.stop();
}

void check(napi_status status)
{
    if (status != napi_ok)
        throw std::runtime_error("Native bridge operation failed");
}

Admission classify_admission(const std::string& request)
{
    try {
        const auto max_bytes = request.find("\"trading.call\"") != std::string::npos
            ? max_trading_request_bytes
            : max_request_bytes;
        const auto value = asterion::parse_json(request, max_bytes);
        if (!value.is_object() || !value.contains("method"))
            return Admission::request;
        const auto& method = value.at("method");
        if (method == "runtime.snapshot")
            return Admission::snapshot;
        if (method == "trading.call" && value.contains("params")
            && value.at("params").is_object()) {
            const auto& params = value.at("params");
            if (params.contains("request")
                && asterion::terminal::urgent_trading_request(params.at("request")))
                return Admission::urgent;
        }
        return Admission::request;
    } catch (...) {
        // This only selects an admission counter. Native validates the request.
        return Admission::request;
    }
}

std::atomic<unsigned>& counter_for(Runtime& runtime, Admission kind) noexcept
{
    switch (kind) {
        case Admission::snapshot:
            return runtime.pending_snapshots;
        case Admission::urgent:
            return runtime.pending_urgent;
        case Admission::request:
            return runtime.pending_requests;
    }
    return runtime.pending_requests;
}

unsigned limit_for(Admission kind) noexcept
{
    switch (kind) {
        case Admission::snapshot:
            return snapshot_limit;
        case Admission::urgent:
            return urgent_limit;
        case Admission::request:
            return request_limit;
    }
    return request_limit;
}

struct Work
{
    void release_admission() noexcept
    {
        if (admitted) {
            counter_for(*runtime, kind).fetch_sub(1);
            admitted = false;
        }
    }

    ~Work()
    {
        release_admission();
    }

    Owner runtime;
    std::string request, error;
    asterion::Payload response;
    std::optional<asterion::terminal::MarketWatch::Batch> watch_batch;
    bool is_watch = false;
    asterion::ErrorCode error_code = asterion::ErrorCode::internal_error;
    napi_deferred deferred{};
    napi_threadsafe_function completion{};
    std::future<void> finished;
    std::mutex completion_mutex;
    bool closing = false;
    Admission kind = Admission::request;
    bool admitted = false;
};

napi_value
build_watch_value(napi_env env, const std::optional<asterion::terminal::MarketWatch::Batch>& batch)
{
    if (!batch) {
        napi_value null_val;
        check(napi_get_null(env, &null_val));
        return null_val;
    }
    napi_value obj, frames, online, err_str;
    check(napi_create_object(env, &obj));
    check(napi_create_array_with_length(env, batch->frames.size(), &frames));
    for (std::size_t i = 0; i < batch->frames.size(); ++i) {
        const auto& raw = batch->frames[i];
        void* data = nullptr;
        napi_value ab, u8;
        check(napi_create_arraybuffer(env, raw.size(), &data, &ab));
        if (!raw.empty())
            std::memcpy(data, raw.data(), raw.size());
        check(napi_create_typedarray(env, napi_uint8_array, raw.size(), ab, 0, &u8));
        check(napi_set_element(env, frames, static_cast<uint32_t>(i), u8));
    }
    check(napi_get_boolean(env, batch->online, &online));
    check(napi_create_string_utf8(env, batch->error.data(), batch->error.size(), &err_str));
    check(napi_set_named_property(env, obj, "frames", frames));
    check(napi_set_named_property(env, obj, "online", online));
    check(napi_set_named_property(env, obj, "error", err_str));
    return obj;
}

void complete(napi_env env, napi_value, void*, void* data) noexcept
{
    if (!env)
        return; // Teardown may drain queued callbacks after finalize has deleted Work.
    auto& job = *static_cast<Work*>(data);
    job.release_admission();
    const bool success = job.error.empty();
    if (success && job.is_watch) {
        try {
            napi_value value = build_watch_value(env, job.watch_batch);
            napi_resolve_deferred(env, job.deferred, value);
        } catch (const std::exception& failure) {
            napi_value msg, err;
            if (napi_create_string_utf8(env, failure.what(), NAPI_AUTO_LENGTH, &msg) == napi_ok
                && napi_create_error(env, nullptr, msg, &err) == napi_ok)
                napi_reject_deferred(env, job.deferred, err);
        }
        job.watch_batch.reset();
        return;
    }
    napi_value value;
    const auto& text = success ? *job.response : job.error;
    if (napi_create_string_utf8(env, text.data(), text.size(), &value) == napi_ok) {
        if (success)
            napi_resolve_deferred(env, job.deferred, value);
        else {
            napi_value error, code;
            const auto name = asterion::error_name(job.error_code);
            if (napi_create_string_utf8(env, name.data(), name.size(), &code) == napi_ok
                && napi_create_error(env, code, value, &error) == napi_ok)
                napi_reject_deferred(env, job.deferred, error);
        }
    }
    job.response.reset();
    job.watch_batch.reset();
}

void finalize(napi_env, void* data, void*) noexcept
{
    std::unique_ptr<Work> job(static_cast<Work*>(data));
    {
        std::lock_guard lock(job->completion_mutex);
        job->closing = true;
    }
    // Node may finalize a TSFN during forced environment teardown before its
    // producer returns. Keep the request and runtime alive until that producer
    // stops, without holding the mutex it needs to observe closing.
    if (job->finished.valid())
        job->finished.wait();
}

void notify_completion(Work* job) noexcept
{
    std::lock_guard lock(job->completion_mutex);
    if (job->closing)
        return;
    const auto completion = job->completion;
    // One native completion produces one queued JS notification.
    if (napi_call_threadsafe_function(completion, job, napi_tsfn_nonblocking) != napi_closing)
        napi_release_threadsafe_function(completion, napi_tsfn_release);
}

void deliver(Work* job, asterion::Payload response, std::exception_ptr failure) noexcept
{
    try {
        if (failure)
            std::rethrow_exception(failure);
        job->response = std::move(response);
    } catch (const std::exception& error) {
        job->error_code = asterion::classify(error);
        job->error = error.what();
    } catch (...) {
        job->error = "Native request failed";
    }
    notify_completion(job);
}

void deliver_watch(
    Work* job,
    std::optional<asterion::terminal::MarketWatch::Batch> batch,
    std::exception_ptr failure) noexcept
{
    try {
        if (failure)
            std::rethrow_exception(failure);
        job->watch_batch = std::move(batch);
    } catch (const std::exception& error) {
        job->error_code = asterion::classify(error);
        job->error = error.what();
    } catch (...) {
        job->error = "Native market watch failed";
    }
    notify_completion(job);
}

napi_value call(napi_env env, napi_callback_info info) noexcept
{
    try {
        size_t count = 1, length = 0;
        napi_value arg;
        check(napi_get_cb_info(env, info, &count, &arg, nullptr, nullptr));
        napi_valuetype type;
        if (count != 1)
            throw std::invalid_argument("Expected one request string");
        check(napi_typeof(env, arg, &type));
        if (type != napi_string)
            throw std::invalid_argument("Expected one request string");
        check(napi_get_value_string_utf8(env, arg, nullptr, 0, &length));
        if (length > max_trading_request_bytes)
            throw std::invalid_argument("Request is too large");
        auto task = std::make_unique<Work>();
        task->request.resize(length + 1);
        check(napi_get_value_string_utf8(env, arg, task->request.data(), length + 1, &length));
        task->request.resize(length);
        if (length > max_request_bytes
            && task->request.find("\"trading.call\"") == std::string::npos)
            throw std::invalid_argument("Request is too large");
        if (task->request.find('\0') != std::string::npos)
            throw std::invalid_argument("Request contains an invalid character");
        void* state = nullptr;
        check(napi_get_instance_data(env, &state));
        task->runtime = *static_cast<Owner*>(state);
        task->kind = classify_admission(task->request);
        auto& pending = counter_for(*task->runtime, task->kind);
        const auto limit = limit_for(task->kind);
        if (pending.fetch_add(1) >= limit) {
            pending.fetch_sub(1);
            throw std::runtime_error("Too many pending native requests");
        }
        task->admitted = true;
        napi_value promise, name;
        check(napi_create_string_utf8(env, "asterion.request", NAPI_AUTO_LENGTH, &name));
        check(napi_create_promise(env, &task->deferred, &promise));
        check(napi_create_threadsafe_function(
            env,
            nullptr,
            nullptr,
            name,
            1,
            1,
            task.get(),
            finalize,
            nullptr,
            complete,
            &task->completion));
        auto* job = task.release(); // The main-thread TSFN finalizer owns Work from here.
        try {
            job->finished = job->runtime->application.request(
                std::move(job->request),
                [job](asterion::Payload response, std::exception_ptr failure) noexcept {
                    deliver(job, std::move(response), failure);
                });
        } catch (...) {
            napi_release_threadsafe_function(job->completion, napi_tsfn_release);
            throw;
        }
        return promise;
    } catch (const std::exception& error) {
        napi_throw_error(env, asterion::error_name(asterion::classify(error)).data(), error.what());
    } catch (...) {
        napi_throw_error(env, nullptr, "Native bridge failed");
    }
    return nullptr;
}

napi_value market_watch(napi_env env, napi_callback_info info) noexcept
{
    try {
        size_t count = 1;
        napi_value arg;
        check(napi_get_cb_info(env, info, &count, &arg, nullptr, nullptr));
        napi_valuetype type;
        if (count != 1)
            throw std::invalid_argument("Expected one restart boolean");
        check(napi_typeof(env, arg, &type));
        if (type != napi_boolean)
            throw std::invalid_argument("Expected one restart boolean");
        bool restart = false;
        check(napi_get_value_bool(env, arg, &restart));
        auto task = std::make_unique<Work>();
        task->is_watch = true;
        task->kind = Admission::snapshot;
        void* state = nullptr;
        check(napi_get_instance_data(env, &state));
        task->runtime = *static_cast<Owner*>(state);
        auto& pending = counter_for(*task->runtime, task->kind);
        if (pending.fetch_add(1) >= limit_for(task->kind)) {
            pending.fetch_sub(1);
            throw std::runtime_error("Too many pending native requests");
        }
        task->admitted = true;
        napi_value promise, name;
        check(napi_create_string_utf8(env, "asterion.marketWatch", NAPI_AUTO_LENGTH, &name));
        check(napi_create_promise(env, &task->deferred, &promise));
        check(napi_create_threadsafe_function(
            env,
            nullptr,
            nullptr,
            name,
            1,
            1,
            task.get(),
            finalize,
            nullptr,
            complete,
            &task->completion));
        auto* job = task.release();
        try {
            job->finished = job->runtime->application.market_watch(
                restart,
                [job](
                    std::optional<asterion::terminal::MarketWatch::Batch> batch,
                    std::exception_ptr failure) noexcept {
                    deliver_watch(job, std::move(batch), failure);
                });
        } catch (...) {
            napi_release_threadsafe_function(job->completion, napi_tsfn_release);
            throw;
        }
        return promise;
    } catch (const std::exception& error) {
        napi_throw_error(env, asterion::error_name(asterion::classify(error)).data(), error.what());
    } catch (...) {
        napi_throw_error(env, nullptr, "Native bridge failed");
    }
    return nullptr;
}

napi_value shutdown(napi_env env, napi_callback_info) noexcept
{
    try {
        void* state = nullptr;
        check(napi_get_instance_data(env, &state));
        if (state)
            (*static_cast<Owner*>(state))->application.stop();
        napi_value undefined;
        check(napi_get_undefined(env, &undefined));
        return undefined;
    } catch (const std::exception& error) {
        napi_throw_error(env, asterion::error_name(asterion::classify(error)).data(), error.what());
    } catch (...) {
        napi_throw_error(env, nullptr, "Native bridge shutdown failed");
    }
    return nullptr;
}

napi_value init(napi_env env, napi_value exports) noexcept
{
    try {
        auto owner = std::make_unique<Owner>(acquire_runtime());
        try {
            check(napi_set_instance_data(
                env,
                owner.get(),
                [](napi_env, void* data, void*) noexcept {
                    std::unique_ptr<Owner> held(static_cast<Owner*>(data));
                    if (held)
                        release_runtime(std::move(*held));
                },
                nullptr));
        } catch (...) {
            release_runtime(std::move(*owner));
            throw;
        }
        owner.release();
        napi_value function, watch_function, stop_function;
        check(napi_create_function(env, "request", NAPI_AUTO_LENGTH, call, nullptr, &function));
        check(napi_set_named_property(env, exports, "request", function));
        check(napi_create_function(
            env,
            "marketWatch",
            NAPI_AUTO_LENGTH,
            market_watch,
            nullptr,
            &watch_function));
        check(napi_set_named_property(env, exports, "marketWatch", watch_function));
        check(napi_create_function(
            env,
            "shutdown",
            NAPI_AUTO_LENGTH,
            shutdown,
            nullptr,
            &stop_function));
        check(napi_set_named_property(env, exports, "shutdown", stop_function));
        return exports;
    } catch (const std::exception& error) {
        napi_throw_error(env, asterion::error_name(asterion::classify(error)).data(), error.what());
    } catch (...) {
        napi_throw_error(env, nullptr, "Cannot initialize the native bridge");
    }
    return nullptr;
}

} // namespace
NAPI_MODULE(asterion_terminal, init)
