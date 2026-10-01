#pragma once
// CTP provider-boundary helpers shared by the market and trader plugins.
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
namespace asterion::ctp {
inline std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
// A NUL-terminated or full-width SDK character field.
template <std::size_t N> std::string field(const char (&value)[N]) {
  return {value, std::find(value, value + N, '\0')};
}
// Exchange identifiers are right-aligned or padded with spaces.
template <std::size_t N> std::string trimmed(const char (&value)[N]) {
  auto text = field(value);
  const auto first = text.find_first_not_of(' ');
  if (first == std::string::npos)
    return {};
  return text.substr(first, text.find_last_not_of(' ') - first + 1);
}
template <std::size_t N> void copy(char (&dest)[N], const std::string& value) {
  if (value.empty() || value.size() >= N || value.find('\0') != std::string::npos)
    throw std::invalid_argument("invalid CTP field length");
  std::memcpy(dest, value.data(), value.size());
}
inline void erase(std::string& value) {
  volatile char* p = value.data();
  for (std::size_t i = 0; i < value.size(); ++i)
    p[i] = 0;
  value.clear();
}
template <std::size_t N> void erase(char (&value)[N]) {
  volatile char* p = value;
  for (std::size_t i = 0; i < N; ++i)
    p[i] = 0;
}
// Explicitly loaded vendor library. The SDK instance must be released before
// this object is destroyed.
class SharedLibrary {
public:
  SharedLibrary(const std::filesystem::path& path, const char* windows_symbol,
                const char* itanium_symbol) {
#ifdef _WIN32
    handle_ = LoadLibraryExW(path.c_str(), nullptr,
                             LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (handle_)
      symbol_ = reinterpret_cast<void*>(GetProcAddress(handle_, windows_symbol));
#else
    static_cast<void>(windows_symbol);
    handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle_)
      symbol_ = dlsym(handle_, itanium_symbol);
#endif
  }
  SharedLibrary(const SharedLibrary&) = delete;
  SharedLibrary& operator=(const SharedLibrary&) = delete;
  ~SharedLibrary() {
#ifdef _WIN32
    if (handle_)
      FreeLibrary(handle_);
#else
    if (handle_)
      dlclose(handle_);
#endif
  }
  // The factory symbol, or null when the library or symbol is missing.
  template <class F> F symbol() const noexcept { return reinterpret_cast<F>(symbol_); }

private:
#ifdef _WIN32
  HMODULE handle_ = nullptr;
#else
  void* handle_ = nullptr;
#endif
  void* symbol_ = nullptr;
};
} // namespace asterion::ctp
