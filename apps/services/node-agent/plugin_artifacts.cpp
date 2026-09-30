#include "plugin_artifacts.hpp"
#include "managed_paths.hpp"
#include <asterion/kernel/native_plugin.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/foundation/serialization.hpp>
#include <fstream>
#include <set>
namespace asterion::agent {
namespace fs = std::filesystem;
fs::path PluginArtifacts::binary(const std::string& hash) const {
  return root_ / "artifacts" / (hash + (current_platform().os == "windows" ? ".exe" : ".bin"));
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
    NativeLibrary library(binary(hash));
    if (!identities.insert(library.descriptor().id).second)
      throw std::invalid_argument("duplicate native plugin identity");
  }
}
fs::path PluginArtifacts::materialize(const std::string& name,
                                      const std::vector<std::string>& artifacts) const {
  const auto parent = root_ / "services" / name / "plugins";
  require_managed_path(parent);
  fs::create_directory(parent);
  const auto folder = parent / sha256_bytes(Json(artifacts).dump());
  require_managed_path(folder);
  fs::create_directory(folder);
  verify(artifacts);
  for (const auto& hash : artifacts) {
    const auto target = folder / (hash + (current_platform().os == "macos" ? ".dylib" : ".so"));
    require_managed_path(target);
    if (!fs::exists(target)) {
      std::ifstream input(binary(hash), std::ios::binary);
      std::string bytes((std::istreambuf_iterator<char>(input)), {});
      write_file_durably(target, bytes);
    }
    if (sha256_file(target) != hash)
      throw std::invalid_argument("native plugin artifact is missing or corrupted");
  }
  // Never accidentally load an unlisted library left in an installation directory.
  if (static_cast<std::size_t>(std::distance(fs::directory_iterator(folder),
                                             fs::directory_iterator{})) != artifacts.size())
    throw std::invalid_argument("native plugin installation contains unlisted files");
  return folder;
}
} // namespace asterion::agent
