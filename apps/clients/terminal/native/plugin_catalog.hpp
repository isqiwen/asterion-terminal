#pragma once
#include <asterion/kernel/native_plugin.hpp>
#include <asterion/foundation/serialization.hpp>
#include <span>
namespace asterion::terminal {
struct PluginArtifact {
  std::filesystem::path path;
  std::string sha256;
};
enum class PluginAvailability { available, invalid };
struct PluginCatalogEntry {
  PluginArtifact artifact;
  NativeDescriptor descriptor;
  PluginAvailability availability = PluginAvailability::invalid;
  std::string error = "cannot load native plugin library";
  bool managed = false;
  bool supports_history() const;
};
struct PluginSelection {
  std::vector<std::string> hashes;
  std::vector<PluginArtifact> uploads;
};
struct PluginCatalog {
  std::filesystem::path directory;
  std::vector<PluginCatalogEntry> entries;
  std::filesystem::path managed_directory;
  static PluginCatalog inspect(const std::filesystem::path& directory);
  // Already deployed hashes can be retained when their source files are absent.
  PluginSelection select_data_task_plugins(std::span<const std::string> hashes,
                                           std::span<const std::string> installed = {}) const;
};
PluginCatalog local_plugin_catalog();
PluginCatalog plugin_inventory(const std::filesystem::path& bundled,
                               const std::filesystem::path& managed);
PluginCatalogEntry preview_plugin(const std::filesystem::path& path);
void install_plugin(const std::filesystem::path& bundled, const std::filesystem::path& managed,
                    const std::filesystem::path& source, const std::string& sha256);
void uninstall_plugin(const std::filesystem::path& managed, const std::string& file,
                      const std::string& sha256);
Json plugin_catalog_json(const PluginCatalog& catalog);
} // namespace asterion::terminal
