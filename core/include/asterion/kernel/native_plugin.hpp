#pragma once
#include <asterion/plugin/abi.h>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
namespace asterion {
void check_plugin_status(AstStatus status);
struct NativeCapability {
  std::string id, kind;
  uint32_t version;
};
struct NativeDescriptor {
  std::string id, version;
  std::vector<NativeCapability> capabilities;
};
class NativeInstance;
class NativeLibrary {
public:
  explicit NativeLibrary(const std::filesystem::path&);
  const NativeDescriptor& descriptor() const;
  const std::string& sha256() const;
  const std::filesystem::path& path() const;
  std::unique_ptr<NativeInstance> create(const std::string& capability,
                                         const std::vector<AstSetting>& settings) const;

private:
  struct State;
  std::shared_ptr<State> state_;
  friend class NativeInstance;
};
class NativeInstance {
public:
  ~NativeInstance();
  NativeInstance(const NativeInstance&) = delete;
  NativeInstance& operator=(const NativeInstance&) = delete;
  void* handle() const noexcept { return instance_; }
  void start();
  void stop() noexcept;
  const void* query(const char* capability, uint32_t version, uint32_t size) const;

private:
  NativeInstance(std::shared_ptr<NativeLibrary::State>, void*);
  std::shared_ptr<NativeLibrary::State> library_;
  void* instance_;
  bool running_ = false;
  friend class NativeLibrary;
};
// Explicit service configuration or executable-adjacent plugins, never cwd/PATH.
std::filesystem::path native_plugin_directory();
void configure_native_plugins(const std::filesystem::path&);
std::vector<NativeLibrary> discover_native_plugins(const std::filesystem::path&);
} // namespace asterion
