#include "plugin_artifacts.hpp"
#include "managed_paths.hpp"
#include <asterion/kernel/native_plugin.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/foundation/serialization.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <set>
namespace asterion::agent {
namespace fs = std::filesystem;
fs::path PluginArtifacts::binary(const std::string& hash) const {
  return root_ / "artifacts" / (hash + ".bin");
}
void PluginArtifacts::verify(const std::vector<std::string>& artifacts) const {
  if (artifacts.size() > 128)
    throw std::invalid_argument("too many native plugin artifacts");
  std::set<std::string> unique, identities;
  for (const auto& hash : artifacts) {
    validate_artifact_digest(hash);
    require_managed_path(binary(hash));
    if (!unique.insert(hash).second || sha256_file(binary(hash)) != hash)
      throw std::invalid_argument("native plugin artifact is missing or corrupted");
    if (!identities.insert(identity(binary(hash))).second)
      throw std::invalid_argument("duplicate native plugin identity");
  }
}
std::string PluginArtifacts::identity(const fs::path& library) const {
  const auto output = root_ / ("plugin-inspect-" + unique_process_id() + ".out");
  require_managed_path(output);
  std::string id;
  {
    ChildProcess child(inspector_, {"--inspect-plugin", library.string()}, false, output);
    const bool done = child.wait(std::chrono::seconds(10));
    if (done && child.exit_code() == 0) {
      std::ifstream input(output);
      std::getline(input, id);
    }
  }
  std::error_code ignored;
  fs::remove(output, ignored);
  if (id.empty() || id.size() > 256)
    throw std::invalid_argument("native plugin artifact is missing or corrupted");
  return id;
}
int inspect_plugin(const std::string& path) {
  try {
    NativeLibrary library(fs::path(std::u8string(path.begin(), path.end())));
    std::cout << library.descriptor().id << '\n';
    return 0;
  } catch (...) {
    return 1;
  }
}
fs::path PluginArtifacts::materialize(const std::string& name,
                                      const std::vector<std::string>& artifacts) const {
  const auto parent = root_ / "services" / name / "plugins";
  require_managed_path(parent);
  create_directories_durably(parent);
  const auto folder = parent / sha256_bytes(Json(artifacts).dump());
  require_managed_path(folder);
  create_directories_durably(folder);
  verify(artifacts);
  std::set<fs::path> expected;
  for (const auto& hash : artifacts) {
    const auto target = folder / (hash + (current_platform().os == "macos" ? ".dylib" : ".so"));
    require_managed_path(target);
    expected.insert(target);
    // A copy interrupted mid-write is replaced from the verified artifact; a
    // rename publishes it, so a reader never sees a partial library.
    if (!fs::exists(target) || sha256_file(target) != hash) {
      std::ifstream input(binary(hash), std::ios::binary);
      std::string bytes((std::istreambuf_iterator<char>(input)), {});
      replace_file_durably(target, bytes);
    }
    if (sha256_file(target) != hash)
      throw std::invalid_argument("native plugin artifact is missing or corrupted");
  }
  // Only listed libraries may be loadable from this folder. Everything else
  // here is a derived copy or an interrupted temporary, never user data.
  for (const auto& entry : fs::directory_iterator(folder)) {
    if (expected.contains(entry.path()))
      continue;
    if (entry.is_symlink() || !entry.is_regular_file())
      throw std::invalid_argument("native plugin installation contains unexpected entries");
    fs::remove(entry.path());
  }
  sync_directory(folder);
  return folder;
}
} // namespace asterion::agent
