#pragma once
#include <filesystem>
#include <regex>
#include <stdexcept>
#include <string>
namespace asterion::agent {
inline void validate_service_id(const std::string& s) {
  if (!std::regex_match(s, std::regex("[A-Za-z0-9][A-Za-z0-9_-]{0,63}")))
    throw std::invalid_argument("invalid service id");
}
inline void validate_artifact_digest(const std::string& s) {
  if (!std::regex_match(s, std::regex("[a-f0-9]{64}")))
    throw std::invalid_argument("invalid artifact digest");
}
inline void require_managed_path(const std::filesystem::path& p) {
  if (std::filesystem::is_symlink(p))
    throw std::invalid_argument("managed paths cannot be symlinks");
}
} // namespace asterion::agent
