#include <asterion/kernel/environment.hpp>
#include <stdexcept>
#include <cstdlib>
namespace asterion {
std::optional<std::string> environment_variable(const char* name) {
  const char* value = std::getenv(name);
  if (!value || !*value)
    return std::nullopt;
  return std::string(value);
}
std::optional<std::filesystem::path> environment_path(const char* name) {
  const auto value = environment_variable(name);
  if (!value)
    return std::nullopt;
  return std::filesystem::path(std::u8string(value->begin(), value->end()));
}
} // namespace asterion
