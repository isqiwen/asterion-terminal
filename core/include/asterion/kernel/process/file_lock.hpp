#pragma once
#include <filesystem>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#endif
namespace asterion {
class FileLock {
#ifdef _WIN32
  HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
  int handle_ = -1;
#endif
public:
  explicit FileLock(const std::filesystem::path& directory, const std::string& name) {
    if (name.empty() || name.find_first_of("/\\") != std::string::npos || name == "." ||
        name == "..")
      throw std::invalid_argument("invalid lock name");
    const auto path = directory / name;
    if (std::filesystem::is_symlink(path))
      throw std::invalid_argument("invalid agent lock path");
#ifdef _WIN32
    handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE)
      throw std::runtime_error("agent directory is already owned or unavailable");
#else
    handle_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (handle_ < 0)
      throw std::runtime_error("agent directory is unavailable");
    if (::flock(handle_, LOCK_EX | LOCK_NB) != 0) {
      ::close(handle_);
      handle_ = -1;
      throw std::runtime_error("agent directory is already owned");
    }
#endif
  }
  ~FileLock() {
#ifdef _WIN32
    if (handle_ != INVALID_HANDLE_VALUE)
      CloseHandle(handle_);
#else
    if (handle_ >= 0)
      ::close(handle_);
#endif
  }
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;
};
} // namespace asterion
