#include "remote_bundle.hpp"
#include <asterion/kernel/environment.hpp>
#include <vector>
#include <asterion/kernel/process/artifact.hpp>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif
namespace asterion::terminal {
namespace fs = std::filesystem;
namespace {
fs::path checked_bundle(const std::string& arch) {
  if (arch != "x86_64")
    throw std::invalid_argument("unsupported remote Linux architecture");
#ifdef _WIN32
  // Read the process environment directly: the Electron host sets this after
  // startup.
  const auto size = GetEnvironmentVariableW(L"ASTERION_REMOTE_RESOURCES", nullptr, 0);
  if (!size)
    throw std::runtime_error("desktop package lacks bundled Linux service resources");
  std::wstring configured(size, L'\0');
  const auto copied =
      GetEnvironmentVariableW(L"ASTERION_REMOTE_RESOURCES", configured.data(), size);
  if (!copied || copied >= size)
    throw std::runtime_error("invalid bundled resource environment");
  configured.resize(copied);
  const fs::path root(configured);
#else
  const auto configured = environment_path("ASTERION_REMOTE_RESOURCES");
  if (!configured)
    throw std::runtime_error("desktop package lacks bundled Linux service resources");
  const fs::path root(*configured);
#endif
  const auto folder = root / arch;
  if (!root.is_absolute() || fs::is_symlink(root) || fs::is_symlink(folder))
    throw std::invalid_argument("invalid bundled resource directory");
  const auto manifest = folder / "manifest.json";
  if (fs::is_symlink(manifest) || !fs::is_regular_file(manifest) || fs::file_size(manifest) > 65536)
    throw std::runtime_error("desktop package lacks resources for the target Linux architecture; "
                             "reinstall the complete package");
  std::ifstream input(manifest);
  const auto info = nlohmann::json::parse(input);
  if (info.size() != 6 || info.at("version") != 2 ||
      info.at("product_version") != ASTERION_PRODUCT_VERSION ||
      info.at("source_sha256") != ASTERION_SERVICE_SOURCE_SHA256 || info.at("os") != "linux" ||
      info.at("arch") != arch || info.at("files").size() != 13)
    throw std::invalid_argument("bundled Linux service version mismatch");
  std::vector<std::string> names{"asterion-node-agent",
                                 "asterion-trading",
                                 "asterion-market-data",
                                 "asterion-task-service",
                                 "asterion-backtest",
                                 "asterion-factor",
                                 "asterion-data-pipeline",
                                 "asterion-strategy",
                                 "initialize-linux.py",
                                 "plugins/asterion-tushare.so",
                                 "plugins/asterion-order-limits.so"};
  if (arch == "x86_64") {
    names.push_back("ctp-md.so");
    names.push_back("ctp-trader.so");
  }
  for (const auto& name : names) {
    const auto file = folder / name;
    if (fs::is_symlink(file.parent_path()) || fs::is_symlink(file) || !fs::is_regular_file(file) ||
        fs::file_size(file) > 256 * 1024 * 1024 ||
        sha256_file(file) != info.at("files").at(name).get<std::string>())
      throw std::invalid_argument("bundled Linux resource verification failed");
    if (std::string(name) != "initialize-linux.py") {
      const auto platform = artifact_platform(file);
      if (platform.os != "linux" || platform.arch != arch)
        throw std::invalid_argument("bundled Linux program architecture mismatch");
    }
  }
  return folder;
}
} // namespace
fs::path bundled_linux_program(const std::string& arch, const std::string& program) {
  if (program != "asterion-node-agent" && program != "asterion-trading" &&
      program != "asterion-market-data" && program != "asterion-task-service" &&
      program != "asterion-backtest" && program != "asterion-factor" &&
      program != "asterion-data-pipeline" && program != "asterion-strategy" &&
      !((program == "ctp-md.so" || program == "ctp-trader.so") && arch == "x86_64"))
    throw std::invalid_argument("invalid bundled program");
  return checked_bundle(arch) / program;
}
std::string bundled_linux_initializer() {
  const auto path = checked_bundle("x86_64") / "initialize-linux.py";
  if (fs::file_size(path) > 65536)
    throw std::invalid_argument("initializer is too large");
  std::ifstream input(path);
  return {std::istreambuf_iterator<char>(input), {}};
}
void export_bundled_initializer(const fs::path& destination) {
  if (!destination.is_absolute() || fs::exists(destination) || fs::is_symlink(destination))
    throw std::invalid_argument("choose a save path for the initializer that does not exist yet");
  fs::copy_file(checked_bundle("x86_64") / "initialize-linux.py", destination);
}

} // namespace asterion::terminal
