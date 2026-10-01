#include <asterion/terminal.h>
#include <asterion/foundation/serialization.hpp>
#include <asterion/kernel/thread_pool.hpp>
#include <atomic>
#include <node_api.h>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace {
constexpr unsigned request_limit = 16;
constexpr unsigned snapshot_limit = 8;
struct Runtime {
  std::unique_ptr<void, decltype(&asterion_terminal_destroy)> handle{asterion_terminal_create(),
                                                                     asterion_terminal_destroy};
  // Status reads must not wait for either application I/O or Node's shared pool.
  asterion::ThreadPool requests{4, request_limit};
  asterion::ThreadPool snapshots{1, snapshot_limit};
  std::atomic<unsigned> pending_requests{0}, pending_snapshots{0};
  Runtime() {
    if (!handle)
      throw std::runtime_error("Cannot initialize the C++ runtime");
  }
};
using Owner = std::shared_ptr<Runtime>;
void check(napi_status status) {
  if (status != napi_ok)
    throw std::runtime_error("Native bridge operation failed");
}
bool is_snapshot(const std::string& request) {
  try {
    const auto value = asterion::parse_json(request);
    return value.is_object() && value.contains("method") &&
           value.at("method") == "runtime.snapshot";
  } catch (...) {
    // This only selects a queue. The C ABI remains the protocol validator.
    return false;
  }
}
struct Work {
  Owner runtime;
  std::string request, response, error;
  napi_deferred deferred{};
  napi_threadsafe_function completion{};
  std::future<void> finished;
  std::mutex completion_mutex;
  bool closing = false;
  bool snapshot = false, admitted = false;
  void release_admission() noexcept {
    if (admitted) {
      (snapshot ? runtime->pending_snapshots : runtime->pending_requests).fetch_sub(1);
      admitted = false;
    }
  }
  ~Work() { release_admission(); }
};
void complete(napi_env env, napi_value, void*, void* data) noexcept {
  auto& job = *static_cast<Work*>(data);
  job.release_admission();
  if (!env)
    return; // Environment teardown still drains the queue and finalizes Work.
  napi_value value;
  const bool success = job.error.empty();
  const auto& text = success ? job.response : job.error;
  if (napi_create_string_utf8(env, text.data(), text.size(), &value) == napi_ok) {
    if (success)
      napi_resolve_deferred(env, job.deferred, value);
    else {
      napi_value error;
      if (napi_create_error(env, nullptr, value, &error) == napi_ok)
        napi_reject_deferred(env, job.deferred, error);
    }
  }
}
void finalize(napi_env, void* data, void*) noexcept {
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
void execute(Work* job) noexcept {
  {
    std::lock_guard lock(job->completion_mutex);
    if (job->closing)
      return;
  }
  try {
    std::unique_ptr<char, decltype(&asterion_terminal_free)> response(
        asterion_terminal_call(job->runtime->handle.get(), job->request.c_str()),
        asterion_terminal_free);
    if (!response)
      throw std::runtime_error("Cannot allocate the C++ response");
    job->response = response.get();
  } catch (const std::exception& error) {
    job->error = error.what();
  } catch (...) {
    job->error = "Native request failed";
  }
  std::lock_guard lock(job->completion_mutex);
  if (job->closing)
    return;
  const auto completion = job->completion;
  // Each function receives exactly one item, so its one-slot queue cannot fill.
  // After napi_closing Node owns teardown; the handle must not be used again.
  if (napi_call_threadsafe_function(completion, job, napi_tsfn_nonblocking) != napi_closing)
    napi_release_threadsafe_function(completion, napi_tsfn_release);
}
napi_value call(napi_env env, napi_callback_info info) noexcept {
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
    if (length > 65536)
      throw std::invalid_argument("Request exceeds 64 KiB");
    auto task = std::make_unique<Work>();
    task->request.resize(length + 1);
    check(napi_get_value_string_utf8(env, arg, task->request.data(), length + 1, &length));
    task->request.resize(length);
    if (task->request.find('\0') != std::string::npos)
      throw std::invalid_argument("Request contains an invalid character");
    void* state = nullptr;
    check(napi_get_instance_data(env, &state));
    task->runtime = *static_cast<Owner*>(state);
    task->snapshot = is_snapshot(task->request);
    auto& pending =
        task->snapshot ? task->runtime->pending_snapshots : task->runtime->pending_requests;
    const auto limit = task->snapshot ? snapshot_limit : request_limit;
    if (pending.fetch_add(1) >= limit) {
      pending.fetch_sub(1);
      throw std::runtime_error("Too many pending native requests");
    }
    task->admitted = true;
    napi_value promise, name;
    check(napi_create_string_utf8(env, "asterion.request", NAPI_AUTO_LENGTH, &name));
    check(napi_create_promise(env, &task->deferred, &promise));
    check(napi_create_threadsafe_function(env, nullptr, nullptr, name, 1, 1, task.get(), finalize,
                                          nullptr, complete, &task->completion));
    auto* job = task.release(); // The main-thread TSFN finalizer owns Work from here.
    try {
      auto& pool = job->snapshot ? job->runtime->snapshots : job->runtime->requests;
      job->finished = pool.submit([job](std::stop_token) { execute(job); });
    } catch (...) {
      napi_release_threadsafe_function(job->completion, napi_tsfn_release);
      throw;
    }
    return promise;
  } catch (const std::exception& error) {
    napi_throw_error(env, nullptr, error.what());
  } catch (...) {
    napi_throw_error(env, nullptr, "Native bridge failed");
  }
  return nullptr;
}
napi_value init(napi_env env, napi_value exports) noexcept {
  try {
    auto owner = std::make_unique<Owner>(std::make_shared<Runtime>());
    check(napi_set_instance_data(
        env, owner.get(),
        [](napi_env, void* data, void*) noexcept { delete static_cast<Owner*>(data); }, nullptr));
    owner.release();
    napi_value function;
    check(napi_create_function(env, "request", NAPI_AUTO_LENGTH, call, nullptr, &function));
    check(napi_set_named_property(env, exports, "request", function));
    return exports;
  } catch (const std::exception& error) {
    napi_throw_error(env, nullptr, error.what());
  } catch (...) {
    napi_throw_error(env, nullptr, "Cannot initialize the native bridge");
  }
  return nullptr;
}
} // namespace
NAPI_MODULE(asterion_terminal, init)
