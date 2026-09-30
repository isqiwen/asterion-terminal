#include "file_journal.hpp"
#include <asterion/kernel/durable_file.hpp>
#include <fstream>
#include <algorithm>
#include <cerrno>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif
namespace asterion {
namespace {
std::string name(std::size_t index) {
  std::ostringstream out;
  out << std::setfill('0') << std::setw(8) << index << ".json";
  return out.str();
}
} // namespace
FileJournal::FileJournal(std::filesystem::path directory, std::set<std::string> sidecars)
    : directory_(std::move(directory)), sidecar_directories_(std::move(sidecars)) {
  for (const auto& name : sidecar_directories_)
    if (name.empty() || name == "." || name == ".." ||
        std::filesystem::path(name).filename() != std::filesystem::path(name) ||
        name == "writer.lock" || name == "pending.tmp")
      throw std::invalid_argument("invalid journal sidecar directory");
  if (!directory_.is_absolute())
    throw std::invalid_argument("trading record directory must be an absolute path");
}
FileJournal::~FileJournal() {
  stop();
}
PluginDescriptor FileJournal::descriptor() const {
  return {"asterion.storage.filesystem-journal", PluginKind::storage, plugin_contract_version, {}};
}
void FileJournal::start() {
  if (handle_ != -1)
    throw std::logic_error("storage plugin already started");
  if (!std::filesystem::is_directory(directory_))
    throw std::invalid_argument("choose an existing trading record directory");
  if (std::filesystem::is_symlink(directory_ / "writer.lock"))
    throw std::invalid_argument("symbolic link lock file refused");
#ifdef _WIN32
  const auto handle =
      CreateFileW((directory_ / "writer.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                  OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE)
    throw std::runtime_error("trading record directory is in use or not writable");
  handle_ = reinterpret_cast<std::intptr_t>(handle);
#else
  const auto handle = ::open((directory_ / "writer.lock").c_str(), O_RDWR | O_CREAT, 0600);
  if (handle < 0)
    throw std::runtime_error("trading record directory is not writable");
  if (::flock(handle, LOCK_EX | LOCK_NB) != 0) {
    ::close(handle);
    throw std::runtime_error("trading record directory is in use by another writer");
  }
  handle_ = handle;
#endif
  try {
    poisoned_ = false;
    count_ = read().size();
    bytes_ = 0;
    for (std::size_t i = 0; i < count_; ++i)
      bytes_ += std::filesystem::file_size(directory_ / name(i));
  } catch (...) {
    stop();
    throw;
  }
}
void FileJournal::stop() noexcept {
  if (handle_ == -1)
    return;
#ifdef _WIN32
  CloseHandle(reinterpret_cast<HANDLE>(handle_));
#else
  ::close(static_cast<int>(handle_));
#endif
  handle_ = -1;
}
std::vector<Json> FileJournal::read() const {
  if (handle_ == -1)
    throw std::logic_error("storage plugin is not started");
  std::vector<std::filesystem::path> paths;
  for (const auto& entry : std::filesystem::directory_iterator(directory_)) {
    const auto file = entry.path().filename().string();
    if (sidecar_directories_.contains(file)) {
      if (!entry.is_directory() || entry.is_symlink())
        throw std::invalid_argument("invalid journal sidecar directory");
      continue;
    }
    if (file == "writer.lock" || file == "pending.tmp") {
      if (!entry.is_regular_file() || entry.is_symlink())
        throw std::invalid_argument("invalid internal file in trading directory");
      continue;
    }
    if (!entry.is_regular_file() || entry.is_symlink() || entry.path().extension() != ".json")
      throw std::invalid_argument(
          "trading directory contains unknown files; use a dedicated directory");
    paths.push_back(entry.path());
  }
  if (paths.size() > 20001)
    throw std::invalid_argument("trading journal exceeds this version's capacity");
  std::sort(paths.begin(), paths.end());
  std::vector<Json> result;
  std::uintmax_t total = 0;
  for (std::size_t i = 0; i < paths.size(); ++i) {
    const auto size = std::filesystem::file_size(paths[i]);
    total += size;
    if (paths[i].filename() != name(i) || size > (i == 0 ? 16ULL * 1024 * 1024 : 65536ULL) ||
        total > 64 * 1024 * 1024)
      throw std::invalid_argument("trading journal has a gap or an oversized file");
    std::ifstream input(paths[i], std::ios::binary);
    if (!input)
      throw std::runtime_error("cannot read trading journal");
    std::string raw{std::istreambuf_iterator<char>(input), {}};
    if (raw.size() != size || input.bad())
      throw std::runtime_error("trading journal read incomplete");
    result.push_back(parse_json(raw, 16 * 1024 * 1024));
  }
  return result;
}
void FileJournal::append(const Json& record) {
  if (handle_ == -1 || poisoned_)
    throw std::runtime_error("trading storage is not writable; close and reopen the session");
  if (count_ >= 20001)
    throw std::invalid_argument("trading journal capacity reached");
  const auto data = record.dump();
  if (data.size() > (count_ == 0 ? 16ULL * 1024 * 1024 : 65536ULL) ||
      bytes_ + data.size() > 64 * 1024 * 1024)
    throw std::invalid_argument("trading journal record too large");
  try {
    const auto temporary = directory_ / "pending.tmp";
    if (std::filesystem::is_symlink(temporary))
      throw std::invalid_argument("symbolic link temporary journal refused");
    write_file_durably(temporary, data);
    const auto target = directory_ / name(count_);
    if (std::filesystem::exists(target))
      throw std::runtime_error("trading journal sequence conflict");
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH))
      throw std::runtime_error("trading journal commit failed");
#else
    std::filesystem::rename(temporary, target);
    sync_directory(directory_);
#endif
    ++count_;
    bytes_ += data.size();
  } catch (...) {
    poisoned_ = true;
    throw;
  }
}
} // namespace asterion
