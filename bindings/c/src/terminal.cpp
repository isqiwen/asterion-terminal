#include <asterion/terminal.h>
#include <asterion/foundation/serialization.hpp>
#include "terminal_application.hpp"
#include <cstring>
#include <cstdlib>
#include <stdexcept>
namespace {
using nlohmann::json;
using Terminal = asterion::terminal::Application;
char* copy(const std::string& value) {
  auto* result = static_cast<char*>(std::malloc(value.size() + 1));
  if (result)
    std::memcpy(result, value.c_str(), value.size() + 1);
  return result;
}
char* error(const char* code, const char* message) noexcept {
  try {
    return copy(json{{"error", {{"code", code}, {"message", message}}}}.dump());
  } catch (...) {
    return nullptr;
  }
}
} // namespace
extern "C" void* asterion_terminal_create() noexcept {
  try {
    return new Terminal;
  } catch (...) {
    return nullptr;
  }
}
extern "C" char* asterion_terminal_call(void* runtime, const char* request) noexcept {
  try {
    if (!runtime || !request || std::strlen(request) > 65536)
      throw std::invalid_argument("无效的本机 API 请求");
    return copy(
        json{{"result", static_cast<Terminal*>(runtime)->dispatch(asterion::parse_json(request))}}
            .dump());
  } catch (const asterion::Error& e) {
    return error(asterion::error_name(e.code()).data(), e.what());
  } catch (const std::invalid_argument& e) {
    return error("invalid_request", e.what());
  } catch (const json::exception&) {
    return error("invalid_request", "JSON 不符合当前请求契约");
  } catch (const std::exception& e) {
    return error("operation_failed", e.what());
  } catch (...) {
    return error("internal_error", "本机核心调用失败");
  }
}
extern "C" void asterion_terminal_free(char* response) noexcept {
  std::free(response);
}
extern "C" void asterion_terminal_destroy(void* runtime) noexcept {
  delete static_cast<Terminal*>(runtime);
}
