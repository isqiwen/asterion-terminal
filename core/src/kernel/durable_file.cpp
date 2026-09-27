#include <asterion/kernel/durable_file.hpp>
#include <algorithm>
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
  const int file =
      ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, owner_only ? 0600 : 0644);
  if (file < 0)
    failed(path, "create");
  bool ok = !owner_only || ::fchmod(file, 0600) == 0;
  std::size_t offset = 0;
  while (ok && offset < contents.size()) {
    const auto n = ::write(file, contents.data() + offset, contents.size() - offset);
    if (n < 0 && errno == EINTR)
      continue;
    ok = n > 0;
    if (ok)
      offset += static_cast<std::size_t>(n);
  }
  ok = ok && flush(file);
  ok = ::close(file) == 0 && ok;
  if (!ok)
    failed(path, "durably write");
#endif
}
void sync_directory(const std::filesystem::path& directory) {
#ifdef _WIN32
  (void)directory;
#else
  const int handle = ::open(directory.c_str(), O_RDONLY | O_CLOEXEC);
  if (handle < 0)
    failed(directory, "open directory");
  const bool ok = flush(handle);
  ::close(handle);
  if (!ok)
    failed(directory, "sync directory");
#endif
}
void replace_file_durably(const std::filesystem::path& path, std::string_view contents,
                          bool owner_only) {
  auto temporary = path;
  temporary += ".tmp";
  if (std::filesystem::is_symlink(temporary) || std::filesystem::is_symlink(path))
    throw std::runtime_error("durable replacement refuses symbolic links");
  write_file_durably(temporary, contents, owner_only);
#ifdef _WIN32
  if (!MoveFileExW(temporary.c_str(), path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    failed(path, "replace");
#else
  std::filesystem::rename(temporary, path);
  sync_directory(path.parent_path());
#endif
}
} // namespace asterion
