#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/child.hpp>
#include <utility>
#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace asterion {
namespace {
std::atomic<int> directory_sync_failures{0};
[[noreturn]] void failed(const std::filesystem::path& path, const char* action) {
  auto name = path.filename().u8string();
  throw std::runtime_error(std::string("cannot ") + action + " " +
                           std::string(name.begin(), name.end()));
}
#ifndef _WIN32
bool flush(int file) {
#ifdef __APPLE__
  // fsync on macOS only reaches the drive cache; F_FULLFSYNC reaches media.
  if (::fcntl(file, F_FULLFSYNC) == 0)
    return true;
#endif
  return ::fsync(file) == 0;
}
struct Descriptor {
  int value;
  explicit Descriptor(int fd) : value(fd) {}
  Descriptor(const Descriptor&) = delete;
  ~Descriptor() {
    if (value >= 0)
      ::close(value);
  }
  void close(const std::filesystem::path& path) {
    if (::close(std::exchange(value, -1)) != 0)
      failed(path, "close");
  }
};
struct stat owned_regular(int file, const std::filesystem::path& path) {
  struct stat info{};
  if (::fstat(file, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != ::geteuid() ||
      info.st_nlink != 1)
    failed(path, "validate owned regular file");
  return info;
}
void write_open_file(Descriptor& file, const std::filesystem::path& path, std::string_view contents,
                     bool owner_only) {
  // Never truncate or change permissions until the opened object is checked.
  (void)owned_regular(file.value, path);
  if ((owner_only && ::fchmod(file.value, 0600) != 0) || ::ftruncate(file.value, 0) != 0)
    failed(path, "prepare durable write");
  std::size_t offset = 0;
  while (offset < contents.size()) {
    const auto count = ::write(file.value, contents.data() + offset, contents.size() - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      failed(path, "durably write");
    offset += static_cast<std::size_t>(count);
  }
  if (!flush(file.value))
    failed(path, "durably write");
}
void verify_identity(int file, const std::filesystem::path& path) {
  const auto original = owned_regular(file, path);
  struct stat named{};
  if (::lstat(path.c_str(), &named) != 0 || named.st_dev != original.st_dev ||
      named.st_ino != original.st_ino)
    failed(path, "verify publication identity");
}

#endif
} // namespace
void write_file_durably(const std::filesystem::path& path, std::string_view contents,
                        bool owner_only) {
#ifdef _WIN32
  (void)owner_only; // Windows inherits the per-user profile ACL of the parent.
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    failed(path, "create");
  bool ok = true;
  std::size_t offset = 0;
  while (ok && offset < contents.size()) {
    DWORD written = 0;
    const auto chunk = static_cast<DWORD>(std::min<std::size_t>(contents.size() - offset, 1 << 20));
    ok = WriteFile(file, contents.data() + offset, chunk, &written, nullptr) && written > 0;
    offset += written;
  }
  ok = ok && FlushFileBuffers(file);
  CloseHandle(file);
  if (!ok)
    failed(path, "durably write");
#else
  Descriptor file(::open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK,
                         owner_only ? 0600 : 0644));
  if (file.value < 0)
    failed(path, "create");
  write_open_file(file, path, contents, owner_only);
  file.close(path);
#endif
  sync_directory(path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path());
}
void sync_file_durably(const std::filesystem::path& path) {
#ifdef _WIN32
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    failed(path, "open");
  BY_HANDLE_FILE_INFORMATION info{};
  const bool valid =
      GetFileInformationByHandle(file, &info) && info.nNumberOfLinks == 1 &&
      !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
  const bool ok = valid && FlushFileBuffers(file);
  CloseHandle(file);
  if (!ok)
    failed(path, "sync");
#else
  Descriptor file(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  if (file.value < 0)
    failed(path, "open");
  (void)owned_regular(file.value, path);
  if (!flush(file.value))
    failed(path, "sync");
  verify_identity(file.value, path);
  file.close(path);
#endif
  sync_directory(path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path());
}
void fail_next_directory_syncs_for_testing(int count) {
  directory_sync_failures = count;
}
void sync_directory(const std::filesystem::path& directory) {
  auto remaining = directory_sync_failures.load();
  while (remaining > 0)
    if (directory_sync_failures.compare_exchange_weak(remaining, remaining - 1))
      failed(directory, "sync directory");
#ifdef _WIN32
  (void)directory;
#else
  const int handle = ::open(directory.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
  if (handle < 0)
    failed(directory, "open directory");
  const bool ok = flush(handle);
  ::close(handle);
  if (!ok)
    failed(directory, "sync directory");
#endif
}
void create_directories_durably(const std::filesystem::path& directory) {
  if (directory.empty() || std::filesystem::is_symlink(directory))
    throw std::runtime_error("durable directory creation requires a regular directory path");
  const auto parent =
      directory.parent_path().empty() ? std::filesystem::path(".") : directory.parent_path();
  if (!std::filesystem::exists(directory)) {
    if (!std::filesystem::exists(parent))
      create_directories_durably(parent);
    std::filesystem::create_directory(directory);
  }
  if (std::filesystem::is_symlink(directory) || !std::filesystem::is_directory(directory))
    throw std::runtime_error("durable directory creation requires a regular directory path");
  sync_directory(directory);
  sync_directory(parent);
}
void publish_file_durably(const std::filesystem::path& temporary,
                          const std::filesystem::path& path) {
  if (std::filesystem::is_symlink(temporary) || std::filesystem::is_symlink(path))
    throw std::runtime_error("durable publication refuses symbolic links");
#ifdef _WIN32
  if (!MoveFileExW(temporary.c_str(), path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    failed(path, "publish");
#else
  Descriptor file(::open(temporary.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  if (file.value < 0)
    failed(temporary, "open");
  (void)owned_regular(file.value, temporary);
  if (!flush(file.value))
    failed(temporary, "sync");
  verify_identity(file.value, temporary);
  // Callers own private directories. The check rejects accidental replacement;
  // trusted same-user code is not a security isolation boundary.
  std::filesystem::rename(temporary, path);
  file.close(temporary);
  sync_directory(path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path());
  if (temporary.parent_path() != path.parent_path())
    sync_directory(temporary.parent_path().empty() ? std::filesystem::path(".")
                                                   : temporary.parent_path());
#endif
}
void replace_file_durably(const std::filesystem::path& path, std::string_view contents,
                          bool owner_only) {
#ifdef _WIN32
  // Windows product work remains paused; keep the existing portable path.
  auto temporary = path;
  temporary += ".tmp";
  if (std::filesystem::is_symlink(temporary) || std::filesystem::is_symlink(path))
    throw std::runtime_error("durable replacement refuses symbolic links");
  write_file_durably(temporary, contents, owner_only);
  if (!MoveFileExW(temporary.c_str(), path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    failed(path, "replace");
#else
  if (std::filesystem::is_symlink(path))
    throw std::runtime_error("durable replacement refuses symbolic links");
  auto temporary = path;
  temporary += ".tmp." + unique_process_id();
  // O_EXCL reserves exactly the descriptor subsequently written and flushed.
  // An existing temporary file is never truncated or adopted.
  Descriptor file(::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                         owner_only ? 0600 : 0644));
  if (file.value < 0)
    failed(temporary, "create");
  struct Cleanup {
    const std::filesystem::path& path;
    bool active = true;
    ~Cleanup() {
      if (active) {
        std::error_code error;
        std::filesystem::remove(path, error);
      }
    }
  } cleanup{temporary};
  write_open_file(file, temporary, contents, owner_only);
  if (std::filesystem::is_symlink(path))
    throw std::runtime_error("durable replacement refuses symbolic links");
  verify_identity(file.value, temporary);
  std::filesystem::rename(temporary, path);
  cleanup.active = false;
  file.close(temporary);
  sync_directory(path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path());
#endif
}
} // namespace asterion
