#include <asterion/kernel/environment.hpp>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#else
#include <cstdlib>
#endif
namespace asterion {
namespace {
#ifdef _WIN32
std::optional<std::wstring> wide(const char* name) {
  const std::wstring key(name, name + std::char_traits<char>::length(name));
  const DWORD size = GetEnvironmentVariableW(key.c_str(), nullptr, 0);
  if (size <= 1)
    return std::nullopt;
  std::wstring value(size, L'\0');
  const DWORD copied = GetEnvironmentVariableW(key.c_str(), value.data(), size);
  if (!copied || copied >= size)
    return std::nullopt;
  value.resize(copied);
  return value;
}
#endif
} // namespace
std::optional<std::string> environment_variable(const char* name) {
#ifdef _WIN32
  const auto value = wide(name);
  if (!value)
    return std::nullopt;
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, value->data(), static_cast<int>(value->size()),
                                        nullptr, 0, nullptr, nullptr);
  if (bytes <= 0)
    throw std::runtime_error("environment variable is not valid Unicode");
  std::string result(static_cast<std::size_t>(bytes), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value->data(), static_cast<int>(value->size()), result.data(),
                      bytes, nullptr, nullptr);
  return result;
#else
  const char* value = std::getenv(name);
  if (!value || !*value)
    return std::nullopt;
  return std::string(value);
#endif
}
std::optional<std::filesystem::path> environment_path(const char* name) {
#ifdef _WIN32
  if (auto value = wide(name))
    return std::filesystem::path(std::move(*value));
  return std::nullopt;
#else
  const auto value = environment_variable(name);
  if (!value)
    return std::nullopt;
  return std::filesystem::path(std::u8string(value->begin(), value->end()));
#endif
}
} // namespace asterion
