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
extern "C" void* asterion_terminal_create(char** failure) noexcept {
  if (failure)
    *failure = nullptr;
  try {
    return new Terminal;
  } catch (const std::exception& e) {
    if (failure)
      *failure = error(asterion::error_name(asterion::classify(e)).data(), e.what());
  } catch (...) {
    if (failure)
      *failure = error("internal_error", "native core initialization failed");
  }
  return nullptr;
}
extern "C" char* asterion_terminal_call(void* runtime, const char* request) noexcept {
  try {
    if (!runtime || !request || std::strlen(request) > 65536)
      throw std::invalid_argument("invalid native API request");
    asterion::Payload response;
    std::exception_ptr failure;
    static_cast<Terminal*>(runtime)
        ->request(request,
                  [&](asterion::Payload value, std::exception_ptr error) noexcept {
                    failure = error;
                    if (!failure)
                      response = std::move(value);
                  })
        .get();
    if (failure)
      std::rethrow_exception(failure);
    return copy(*response);
  } catch (const std::exception& e) {
    return error(asterion::error_name(asterion::classify(e)).data(), e.what());
  } catch (...) {
    return error("internal_error", "native core call failed");
  }
}
extern "C" void asterion_terminal_free(char* response) noexcept {
  std::free(response);
}
extern "C" void asterion_terminal_destroy(void* runtime) noexcept {
  delete static_cast<Terminal*>(runtime);
}
