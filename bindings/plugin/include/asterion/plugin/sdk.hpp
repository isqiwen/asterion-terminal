#pragma once
#include "abi.h"
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
namespace asterion::sdk {
// Never expose exception text (it may contain vendor responses or credentials).
template <class F> AstStatus boundary(F&& f) noexcept {
  try {
    f();
    return AST_OK;
  } catch (AstStatus status) {
    return status == AST_OK ? AST_FAILED : status;
  } catch (const std::invalid_argument&) {
    return AST_INVALID;
  } catch (...) {
    return AST_FAILED;
  }
}
inline void check(AstStatus status) {
  if (status != AST_OK)
    throw status;
}
inline std::string setting(const AstSetting* settings, uint32_t count, std::string_view key) {
  if (count > 64 || (count && !settings))
    throw std::invalid_argument("invalid plugin settings");
  std::string result;
  bool found = false;
  for (uint32_t i = 0; i < count; ++i) {
    if (!settings[i].key || !settings[i].value)
      throw std::invalid_argument("invalid plugin setting");
    if (key == settings[i].key) {
      if (found)
        throw std::invalid_argument("duplicate plugin setting");
      found = true;
      result = settings[i].value;
    }
  }
  return result;
}
} // namespace asterion::sdk
