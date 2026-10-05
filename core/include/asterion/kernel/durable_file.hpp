#pragma once
#include <filesystem>
#include <string_view>
#include <stdexcept>
namespace asterion {
// Creates or truncates `path`, writes all of `contents` and forces it to stable
// storage before returning (F_FULLFSYNC on macOS, fsync elsewhere, and
// FlushFileBuffers on Windows), then synchronizes its directory entry on POSIX.
// owner_only creates the file as 0600 and resets
// an existing file to 0600 on POSIX, so secrets never exist with wider access.
// POSIX checks the opened descriptor before truncation: no symbolic links,
// nonregular files, foreign ownership or multiple hard links are accepted.
// Throws std::runtime_error; on failure the file content is unspecified.
void write_file_durably(const std::filesystem::path& path, std::string_view contents,
                        bool owner_only = true);
// Flushes an existing file produced by another writer without replacing its
// inode, changing permissions, or rewriting its bytes; then syncs its directory.
// POSIX requires a private caller-owned parent and an owned, single-link regular
// file. Used before acknowledging externally generated credentials.
void sync_file_durably(const std::filesystem::path& path);
// Makes a completed create/rename inside `directory` durable. Windows callers
// rely on MOVEFILE_WRITE_THROUGH instead; this is a no-op there.
void sync_directory(const std::filesystem::path& directory);
// Creates missing ancestors and synchronizes each new directory and its parent
// before returning. Rechecking an existing directory also finishes publication
// after an earlier failed synchronization. Symbolic-link destinations are refused.
void create_directories_durably(const std::filesystem::path& directory);
// Durable replacement: on POSIX exclusively create a unique sibling temporary
// file, write and sync its owned descriptor, then rename and sync the directory.
// A crash leaves the old or the new content; callers own private parent paths.
void replace_file_durably(const std::filesystem::path& path, std::string_view contents,
                          bool owner_only = true);
// Publishes a file another writer produced: forces `temporary` to stable
// storage, verifies the opened regular file still names the same object, renames
// it to `path` and syncs both parents if different. Callers own private parents;
// trusted same-user code is not a filesystem security isolation boundary.
void publish_file_durably(const std::filesystem::path& temporary,
                          const std::filesystem::path& path);
// Test hook: fail the next directory synchronization operations before callers
// can commit an authoritative database/configuration reference.
void fail_next_directory_syncs_for_testing(int count);
} // namespace asterion
