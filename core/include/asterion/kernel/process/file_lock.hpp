#pragma once
#include <filesystem>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
namespace asterion {
class FileLock {
  int handle_ = -1;

public:
  enum class Access { exclusive, shared, shared_existing };
  // Non-blocking, cooperative process lock. Existing owners stay exclusive by default.
  explicit FileLock(const std::filesystem::path& directory, const std::string& name,
                    Access access = Access::exclusive) {
    if (name.empty() || name.find_first_of("/\\") != std::string::npos || name == "." ||
        name == "..")
      throw std::invalid_argument("invalid lock name");
    const auto path = directory / name;
    if (std::filesystem::is_symlink(path))
      throw std::invalid_argument("invalid agent lock path");
    if (access == Access::shared_existing && !std::filesystem::is_regular_file(path))
      throw std::runtime_error("agent directory is unavailable");
    handle_ = ::open(path.c_str(),
                     (access == Access::shared_existing ? O_RDONLY : O_RDWR | O_CREAT) |
                         O_NOFOLLOW | O_CLOEXEC,
                     0600);
    if (handle_ < 0)
      throw std::runtime_error("agent directory is unavailable");
    if (::flock(handle_, (access != Access::exclusive ? LOCK_SH : LOCK_EX) | LOCK_NB) != 0) {
      ::close(handle_);
      handle_ = -1;
      throw std::runtime_error("agent directory is already owned");
    }
  }
  ~FileLock() {
    if (handle_ >= 0)
      ::close(handle_);
  }
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;
};
} // namespace asterion
