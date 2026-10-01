#include <asterion/kernel/native_plugin.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/environment.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/foundation/error.hpp>
#include <dlfcn.h>
#include <algorithm>
#include <mutex>
#include <set>
#include <cstring>
namespace asterion {
namespace {
std::mutex configuration_mutex;
std::filesystem::path configured;
std::string bounded(const char* value) {
  if (!value || strnlen(value, 257) == 257 || !*value)
    throw std::invalid_argument("invalid native plugin descriptor");
  return value;
}
} // namespace
void check_plugin_status(AstStatus status) {
  switch (status) {
  case AST_OK:
    return;
  case AST_INVALID:
    throw Error(ErrorCode::invalid_request, "native plugin rejected request");
  case AST_UNSUPPORTED:
    throw Error(ErrorCode::unavailable, "native plugin capability is unsupported");
  case AST_PERMISSION:
    throw Error(ErrorCode::permission_denied, "native plugin access denied");
  case AST_UNAVAILABLE:
    throw Error(ErrorCode::unavailable, "native plugin is unavailable");
  case AST_CANCELLED:
    throw Error(ErrorCode::cancelled, "native plugin operation cancelled");
  case AST_LIMIT:
    throw Error(ErrorCode::resource_exhausted, "native plugin resource limit exceeded");
  default:
    throw Error(ErrorCode::operation_failed, "native plugin operation failed");
  }
}
struct NativeLibrary::State {
  void* library = nullptr;
  const AstPluginV1* api = nullptr;
  NativeDescriptor descriptor;
  std::string sha256;
  std::filesystem::path path;
  ~State() {
    if (library)
      dlclose(library);
  }
};
NativeLibrary::NativeLibrary(const std::filesystem::path& path)
    : state_(std::make_shared<State>()) {
  if (!path.is_absolute() || std::filesystem::is_symlink(path) ||
      !std::filesystem::is_regular_file(path))
    throw std::invalid_argument("native plugin requires an absolute regular library");
  state_->path = path;
  state_->sha256 = sha256_file(path);
  state_->library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!state_->library)
    throw Error(ErrorCode::unavailable, "cannot load native plugin library");
  if (sha256_file(path) != state_->sha256)
    throw std::invalid_argument("native plugin catalog changed; inspect again");
  auto entry = reinterpret_cast<AstPluginEntryV1>(dlsym(state_->library, ASTERION_PLUGIN_ENTRY));
  if (!entry)
    throw std::invalid_argument("native plugin entry is missing");
  const auto* api = entry(ASTERION_PLUGIN_ABI_VERSION, sizeof(AstPluginV1));
  if (!api || api->size != sizeof(AstPluginV1) || api->abi_version != ASTERION_PLUGIN_ABI_VERSION)
    throw std::invalid_argument("native plugin ABI mismatch");
  if (bounded(api->platform) != ASTERION_PLUGIN_PLATFORM)
    throw std::invalid_argument("native plugin platform mismatch");
  if (!api->create || !api->destroy || !api->start || !api->stop || !api->query ||
      !api->capabilities || !api->capability_count || api->capability_count > 64)
    throw std::invalid_argument("invalid native plugin descriptor");
  state_->descriptor = {bounded(api->id), bounded(api->version), {}};
  std::set<std::string> ids;
  for (uint32_t i = 0; i < api->capability_count; ++i) {
    const auto& capability = api->capabilities[i];
    auto id = bounded(capability.id), kind = bounded(capability.kind);
    if (!capability.version || !ids.insert(id).second ||
        (kind != "data" && kind != "strategy" && kind != "risk" && kind != "execution" &&
         kind != "storage" && kind != "tool"))
      throw std::invalid_argument("invalid native plugin capability");
    state_->descriptor.capabilities.push_back({id, kind, capability.version});
  }
  state_->api = api;
}
const std::filesystem::path& NativeLibrary::path() const {
  return state_->path;
}
const std::string& NativeLibrary::sha256() const {
  return state_->sha256;
}
const NativeDescriptor& NativeLibrary::descriptor() const {
  return state_->descriptor;
}
std::unique_ptr<NativeInstance>
NativeLibrary::create(const std::string& capability,
                      const std::vector<AstSetting>& settings) const {
  if (std::ranges::none_of(descriptor().capabilities,
                           [&](const auto& c) { return c.id == capability; }))
    check_plugin_status(AST_UNSUPPORTED);
  if (settings.size() > 64)
    throw std::invalid_argument("invalid native plugin settings");
  std::set<std::string> keys;
  for (const auto& setting : settings)
    if (!setting.key || !setting.value || !keys.insert(bounded(setting.key)).second)
      throw std::invalid_argument("invalid native plugin settings");
  void* instance = nullptr;
  const auto status = state_->api->create(capability.c_str(), settings.data(),
                                          static_cast<uint32_t>(settings.size()), &instance);
  if (status != AST_OK) {
    if (instance)
      state_->api->destroy(instance);
    check_plugin_status(status);
  }
  if (!instance)
    throw std::invalid_argument("native plugin returned an empty instance");
  try {
    return std::unique_ptr<NativeInstance>(new NativeInstance(state_, instance));
  } catch (...) {
    state_->api->destroy(instance);
    throw;
  }
}
NativeInstance::NativeInstance(std::shared_ptr<NativeLibrary::State> library, void* instance)
    : library_(std::move(library)), instance_(instance) {}
NativeInstance::~NativeInstance() {
  stop();
  library_->api->destroy(instance_);
}
void NativeInstance::start() {
  if (running_)
    return;
  const auto status = library_->api->start(instance_);
  if (status != AST_OK) {
    library_->api->stop(instance_);
    check_plugin_status(status);
  }
  running_ = true;
}
void NativeInstance::stop() noexcept {
  if (running_) {
    library_->api->stop(instance_);
    running_ = false;
  }
}
const void* NativeInstance::query(const char* capability, uint32_t version, uint32_t size) const {
  const void* table = nullptr;
  check_plugin_status(library_->api->query(instance_, capability, version, size, &table));
  if (!table)
    throw std::invalid_argument("native plugin returned an empty capability");
  return table;
}
std::filesystem::path native_plugin_directory() {
  std::lock_guard lock(configuration_mutex);
  if (!configured.empty())
    return configured;
  const auto environment = environment_path("ASTERION_PLUGIN_DIRECTORY");
  return environment.value_or(current_executable().parent_path() / "plugins");
}
void configure_native_plugins(const std::filesystem::path& path) {
  if (!path.is_absolute() || std::filesystem::is_symlink(path) ||
      !std::filesystem::is_directory(path))
    throw std::invalid_argument("invalid native plugin directory");
  std::lock_guard lock(configuration_mutex);
  configured = path;
}
std::vector<NativeLibrary> discover_native_plugins(const std::filesystem::path& directory) {
  if (!directory.is_absolute() || std::filesystem::is_symlink(directory) ||
      !std::filesystem::is_directory(directory))
    throw std::invalid_argument("invalid native plugin directory");
  std::vector<std::filesystem::path> paths;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (entry.path().extension() == ".dylib" || entry.path().extension() == ".so")
      paths.push_back(entry.path());
  }
  std::ranges::sort(paths);
  if (paths.size() > 128)
    throw std::invalid_argument("native plugin directory exceeds limit");
  std::vector<NativeLibrary> result;
  std::set<std::string> ids;
  for (const auto& path : paths) {
    result.emplace_back(path);
    if (!ids.insert(result.back().descriptor().id).second)
      throw std::invalid_argument("duplicate native plugin identity");
  }
  return result;
}
} // namespace asterion
