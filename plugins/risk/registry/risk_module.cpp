#include "risk_module.hpp"
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/foundation/error.hpp>
#include <algorithm>
#include <fstream>
#include <optional>
namespace asterion::risk_providers {
namespace {
bool supported(const NativeLibrary& library) {
  return std::ranges::any_of(library.descriptor().capabilities, [](const auto& item) {
    return item.id == AST_RISK_V1 && item.version == 1 && item.kind == "risk";
  });
}
} // namespace
Module Module::selected() {
  std::optional<NativeLibrary> selected;
  for (auto& library : discover_native_plugins(native_plugin_directory())) {
    if (!supported(library))
      continue;
    if (selected)
      throw std::invalid_argument("select exactly one pre-trade risk plugin");
    selected = std::move(library);
  }
  if (!selected)
    throw Error(ErrorCode::unavailable, "pre-trade risk plugin is unavailable");
  return Module(std::move(*selected));
}
std::filesystem::path Module::filename(const std::filesystem::path& directory) {
  return directory / (current_platform().os == "macos" ? "risk-plugin.dylib" : "risk-plugin.so");
}
Module Module::pinned(const std::filesystem::path& directory, const std::string& artifact) {
  if (artifact.size() != 64 || !std::ranges::all_of(artifact, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      }))
    throw std::invalid_argument("invalid risk plugin artifact");
  const auto file = filename(directory);
  if (!directory.is_absolute() || std::filesystem::is_symlink(directory) ||
      !std::filesystem::is_directory(directory) || std::filesystem::is_symlink(file) ||
      !std::filesystem::is_regular_file(file) || sha256_file(file) != artifact)
    throw std::invalid_argument("risk plugin artifact does not match its owner");
  NativeLibrary library(file);
  if (library.sha256() != artifact || !supported(library))
    throw std::invalid_argument("risk plugin artifact does not match its owner");
  return Module(std::move(library));
}
void Module::capture(const std::filesystem::path& directory) const {
  const auto file = filename(directory);
  if (!directory.is_absolute() || std::filesystem::is_symlink(directory) ||
      !std::filesystem::is_directory(directory) || std::filesystem::exists(file) ||
      std::filesystem::is_symlink(file))
    throw std::invalid_argument("risk plugin snapshot already exists or directory is invalid");
  std::ifstream input(library_.path(), std::ios::binary);
  const std::string bytes{std::istreambuf_iterator<char>(input), {}};
  if (sha256_bytes(bytes) != artifact())
    throw std::invalid_argument("risk plugin artifact changed before capture");
  replace_file_durably(file, bytes, true);
}
std::shared_ptr<NativeRisk> Module::create(const OrderLimitsConfig& config) const {
  config.validate();
  return std::make_shared<NativeRisk>(
      library_, std::vector<std::pair<std::string, std::string>>{
                    {"max_order_quantity", config.max_order_quantity.str()},
                    {"max_gross_quantity", config.max_gross_quantity.str()},
                    {"max_working_orders", std::to_string(config.max_working_orders)}});
}
} // namespace asterion::risk_providers
