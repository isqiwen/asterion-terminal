#include "plugin_catalog.hpp"
#include "node_client.hpp"
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/file_lock.hpp>
#include <asterion/kernel/process/child.hpp>
#include <fstream>
#include <regex>
#include <asterion/kernel/process/artifact.hpp>
#include <algorithm>
#include <map>
#include <set>
namespace asterion::terminal {
bool PluginCatalogEntry::supports_history() const {
  return std::ranges::any_of(descriptor.capabilities, [](const NativeCapability& capability) {
    return capability.id == "asterion.history.v2" && capability.version == 2;
  });
}
PluginCatalog PluginCatalog::inspect(const std::filesystem::path& directory) {
  if (!directory.is_absolute() || std::filesystem::is_symlink(directory) ||
      !std::filesystem::is_directory(directory))
    throw std::invalid_argument("invalid native plugin directory");
  std::vector<std::filesystem::path> files;
  for (const auto& entry : std::filesystem::directory_iterator(directory))
    if (entry.path().extension() == ".dylib" || entry.path().extension() == ".so")
      files.push_back(entry.path());
  if (files.size() > 128)
    throw std::invalid_argument("native plugin directory exceeds limit");
  std::sort(files.begin(), files.end());
  PluginCatalog catalog{.directory = directory};
  std::map<std::string, unsigned> identities;
  for (const auto& file : files) {
    PluginCatalogEntry entry{.artifact = {.path = file}};
    try {
      const auto expected = sha256_file(file);
      entry.artifact.sha256 = expected;
      NativeLibrary library(file);
      entry.descriptor = library.descriptor();
      entry.artifact.sha256 = sha256_file(file);
      if (entry.artifact.sha256 != expected)
        throw std::invalid_argument("native plugin catalog changed; inspect again");
      entry.availability = PluginAvailability::available;
      entry.error.clear();
      ++identities[entry.descriptor.id + "@" + entry.descriptor.version];
    } catch (const std::exception&) {
      // One invalid module must not hide the rest of the catalog. Do not expose vendor errors.
    }
    catalog.entries.push_back(std::move(entry));
  }
  for (auto& entry : catalog.entries)
    if (identities[entry.descriptor.id + "@" + entry.descriptor.version] > 1) {
      entry.availability = PluginAvailability::invalid;
      entry.error = "duplicate native plugin identity";
    }
  return catalog;
}
namespace {
bool data_task_capable(const PluginCatalogEntry& entry) {
  return entry.supports_history() ||
         std::ranges::any_of(entry.descriptor.capabilities, [](const auto& item) {
           return item.id == "asterion.risk.pre-trade.v1" && item.version == 1 &&
                  item.kind == "risk";
         });
}
} // namespace
PluginSelection
PluginCatalog::select_data_task_plugins(std::span<const std::string> hashes,
                                        std::span<const std::string> installed) const {
  if (hashes.size() > 128 ||
      std::set<std::string>(hashes.begin(), hashes.end()).size() != hashes.size())
    throw std::invalid_argument("invalid native plugin selection");
  PluginSelection selection{.hashes = {hashes.begin(), hashes.end()}};
  std::set<std::string> selected_ids;
  for (const auto& hash : hashes) {
    const auto entry =
        std::ranges::find(entries, hash, [](const auto& item) { return item.artifact.sha256; });
    if (entry != entries.end() && !entry->descriptor.id.empty() &&
        !selected_ids.insert(entry->descriptor.id).second)
      throw std::invalid_argument("select one version per native plugin");
    if (std::ranges::find(installed, hash) != installed.end())
      continue;
    const auto found = std::ranges::find_if(entries, [&](const PluginCatalogEntry& entry) {
      return entry.availability == PluginAvailability::available && entry.artifact.sha256 == hash;
    });
    if (found == entries.end())
      throw std::invalid_argument("native plugin catalog changed; inspect again");
    if (!data_task_capable(*found))
      throw std::invalid_argument("plugin does not provide a supported data or task capability");
    selection.uploads.push_back(found->artifact);
  }
  // Plugins shipped with the application are part of the product: one version
  // of each stays enabled. Only user-installed plugins are optional.
  for (const auto& entry : entries)
    if (!entry.managed && entry.availability == PluginAvailability::available &&
        data_task_capable(entry) && !selected_ids.contains(entry.descriptor.id))
      throw std::invalid_argument("bundled plugin is required: " + entry.descriptor.id);
  return selection;
}
namespace {
// What a native plugin file is called on this machine.
std::string plugin_extension() {
  return current_platform().os == "macos" ? ".dylib" : ".so";
}
void prepare_managed(const std::filesystem::path& directory) {
  if (!directory.is_absolute() || std::filesystem::is_symlink(directory) ||
      std::filesystem::is_symlink(directory.parent_path()))
    throw std::invalid_argument("invalid native plugin directory");
  create_directories_durably(directory);
  std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
}
} // namespace
PluginCatalogEntry preview_plugin(const std::filesystem::path& path) {
  if (!path.is_absolute() || path.extension() != plugin_extension() ||
      std::filesystem::is_symlink(path) || !std::filesystem::is_regular_file(path) ||
      std::filesystem::file_size(path) > 128 * 1024 * 1024)
    throw std::invalid_argument("invalid native plugin installation file");
  const auto hash = sha256_file(path);
  NativeLibrary library(path);
  if (sha256_file(path) != hash)
    throw std::invalid_argument("native plugin catalog changed; inspect again");
  return {.artifact = {path, hash},
          .descriptor = library.descriptor(),
          .availability = PluginAvailability::available,
          .error = {}};
}
PluginCatalog plugin_inventory(const std::filesystem::path& bundled,
                               const std::filesystem::path& managed) {
  auto catalog = PluginCatalog::inspect(bundled);
  catalog.managed_directory = managed;
  if (std::filesystem::exists(managed)) {
    auto installed = PluginCatalog::inspect(managed);
    for (auto& entry : installed.entries) {
      entry.managed = true;
      catalog.entries.push_back(std::move(entry));
    }
  } else if (std::filesystem::is_symlink(managed))
    throw std::invalid_argument("invalid native plugin directory");
  if (catalog.entries.size() > 128)
    throw std::invalid_argument("native plugin directory exceeds limit");
  std::map<std::pair<std::string, std::string>, unsigned> identities;
  for (const auto& entry : catalog.entries)
    if (!entry.descriptor.id.empty())
      ++identities[{entry.descriptor.id, entry.descriptor.version}];
  for (auto& entry : catalog.entries)
    if (identities[{entry.descriptor.id, entry.descriptor.version}] > 1) {
      entry.availability = PluginAvailability::invalid;
      entry.error = "duplicate native plugin identity";
    }
  return catalog;
}
PluginCatalog local_plugin_catalog() {
  return plugin_inventory(native_plugin_directory(), local_node_directory() / "plugins");
}
void install_plugin(const std::filesystem::path& bundled, const std::filesystem::path& managed,
                    const std::filesystem::path& source, const std::string& hash) {
  if (!std::regex_match(hash, std::regex("[a-f0-9]{64}")))
    throw std::invalid_argument("invalid native plugin selection");
  prepare_managed(managed);
  FileLock lock(managed, "catalog.lock");
  const auto candidate = preview_plugin(source);
  if (candidate.artifact.sha256 != hash)
    throw std::invalid_argument("native plugin catalog changed; inspect again");
  const auto inventory = plugin_inventory(bundled, managed);
  if (inventory.entries.size() >= 128)
    throw std::invalid_argument("native plugin directory exceeds limit");
  for (const auto& entry : inventory.entries)
    if (entry.descriptor.id == candidate.descriptor.id &&
        entry.descriptor.version == candidate.descriptor.version)
      throw std::invalid_argument("native plugin version is already installed");
  const auto temporary = managed / ("install-" + unique_process_id());
  std::filesystem::create_directory(temporary);
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  } cleanup{temporary};
  const auto staged = temporary / (hash + plugin_extension());
  std::ifstream stream(source, std::ios::binary);
  const std::string contents{std::istreambuf_iterator<char>(stream), {}};
  if (contents.size() > 128 * 1024 * 1024)
    throw std::invalid_argument("invalid native plugin installation file");
  write_file_durably(staged, contents);
  if (sha256_file(staged) != hash)
    throw std::invalid_argument("native plugin catalog changed; inspect again");
  (void)preview_plugin(staged);
  const auto destination = managed / (hash + plugin_extension());
  if (std::filesystem::exists(destination) || std::filesystem::is_symlink(destination))
    throw std::invalid_argument("native plugin version is already installed");
  publish_file_durably(staged, destination);
}
void uninstall_plugin(const std::filesystem::path& managed, const std::string& filename,
                      const std::string& hash) {
  if (!std::regex_match(hash, std::regex("[a-f0-9]{64}")))
    throw std::invalid_argument("invalid native plugin selection");
  prepare_managed(managed);
  FileLock lock(managed, "catalog.lock");
  const std::filesystem::path relative(filename);
  if (relative.empty() || relative.filename() != relative ||
      relative.extension() != plugin_extension())
    throw std::invalid_argument("invalid native plugin installation file");
  const auto file = managed / relative;
  if (std::filesystem::is_symlink(file) || !std::filesystem::is_regular_file(file) ||
      sha256_file(file) != hash)
    throw std::invalid_argument("native plugin catalog changed; inspect again");
  std::filesystem::remove(file);
  sync_directory(managed);
}
Json plugin_catalog_json(const PluginCatalog& catalog) {
  Json items = Json::array();
  for (const auto& entry : catalog.entries) {
    Json capabilities = Json::array();
    for (const auto& capability : entry.descriptor.capabilities)
      capabilities.push_back(
          {{"id", capability.id}, {"kind", capability.kind}, {"version", capability.version}});
    items.push_back(
        {{"file", entry.artifact.path.filename().string()},
         {"managed", entry.managed},
         {"id", entry.descriptor.id},
         {"version", entry.descriptor.version},
         {"sha256", entry.artifact.sha256},
         {"capabilities", capabilities},
         {"state", entry.availability == PluginAvailability::available ? "available" : "invalid"},
         {"error", entry.error}});
  }
  return {{"directory", catalog.directory.string()},
          {"managed_directory", catalog.managed_directory.string()},
          {"items", items}};
}
} // namespace asterion::terminal
