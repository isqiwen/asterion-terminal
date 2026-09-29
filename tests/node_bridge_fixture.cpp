// Test-only C ABI fixture: deterministic slow I/O without services or user state.
#include <asterion/terminal.h>
#include <asterion/foundation/serialization.hpp>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
namespace {
struct Runtime {
  std::atomic<unsigned> running{0};
  ~Runtime() {
    if (running.load() != 0)
      std::terminate();
  }
};
} // namespace
extern "C" void* asterion_terminal_create() noexcept {
  return new (std::nothrow) Runtime;
}
extern "C" void asterion_terminal_destroy(void* runtime) noexcept {
  delete static_cast<Runtime*>(runtime);
}
extern "C" void asterion_terminal_free(char* value) noexcept {
  std::free(value);
}
extern "C" char* asterion_terminal_call(void* pointer, const char* request) noexcept {
  try {
    auto& runtime = *static_cast<Runtime*>(pointer);
    const auto value = asterion::parse_json(request);
    const auto method = value.at("method");
    if (method == "fixture.slow") {
      runtime.running.fetch_add(1);
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      runtime.running.fetch_sub(1);
    }
    if (method == "fixture.fail")
      return nullptr;
    const auto result = asterion::Json{{"running", runtime.running.load()}}.dump();
    auto* response = static_cast<char*>(std::malloc(result.size() + 1));
    if (response)
      std::memcpy(response, result.c_str(), result.size() + 1);
    return response;
  } catch (...) {
    return nullptr;
  }
}
