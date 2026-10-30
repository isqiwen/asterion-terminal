#pragma once
#include <filesystem>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>

namespace asterion {

class FileLock
{
    int handle_ = -1;

public:
    enum class Access
    {
        exclusive,
        exclusive_existing,
        shared,
        shared_existing
    };

    // Non-blocking, cooperative process lock. Existing owners stay exclusive by default.
    explicit FileLock(
        const std::filesystem::path& directory,
        const std::string& name,
        Access access = Access::exclusive)
    {
        if (name.empty() || name.find_first_of("/\\") != std::string::npos || name == "."
            || name == "..")
            throw std::invalid_argument("invalid lock name");
        const auto path = directory / name;
        if (std::filesystem::is_symlink(path))
            throw std::invalid_argument("lock path cannot be a symbolic link: " + path.string());
        const bool existing =
            access == Access::shared_existing || access == Access::exclusive_existing;
        if (existing && !std::filesystem::is_regular_file(path))
            throw std::runtime_error("lock file is missing: " + path.string());
        handle_ = ::open(
            path.c_str(),
            (existing ? O_RDONLY : O_RDWR | O_CREAT) | O_NOFOLLOW | O_CLOEXEC,
            0600);
        if (handle_ < 0)
            throw std::runtime_error("cannot open lock file: " + path.string());
        const bool exclusive = access == Access::exclusive || access == Access::exclusive_existing;
        if (::flock(handle_, (exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB) != 0) {
            ::close(handle_);
            handle_ = -1;
            throw std::runtime_error("held by another process: " + path.string());
        }
    }

    ~FileLock()
    {
        if (handle_ >= 0)
            ::close(handle_);
    }

    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
};

} // namespace asterion
