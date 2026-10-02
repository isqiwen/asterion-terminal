#pragma once
#include <asterion/kernel/environment.hpp>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace asterion::terminal {
// Selected by the application entry point, never by a page or connection form.
inline bool development_environment() {
  const auto value = environment_variable("ASTERION_ENVIRONMENT");
  if (!value || *value == "production")
    return false;
  if (*value == "development")
    return true;
  throw std::invalid_argument("invalid Terminal environment");
}
inline std::string local_node_service_name() {
  return development_environment() ? "me.asterion.node-agent.dev" : "";
}
} // namespace asterion::terminal
