#pragma once
#include <filesystem>
#include <string_view>
namespace asterion {
// Creates or truncates `path`, writes all of `contents` and forces it to stable
// storage before returning (F_FULLFSYNC on macOS, fsync elsewhere, and
// FlushFileBuffers on Windows). owner_only creates the file as 0600 and resets
// an existing file to 0600 on POSIX, so secrets never exist with wider access.
// Throws std::runtime_error; on failure the file content is unspecified.
void write_file_durably(const std::filesystem::path& path, std::string_view contents,
                        bool owner_only = true);
// Makes a completed create/rename inside `directory` durable. Windows callers
// rely on MOVEFILE_WRITE_THROUGH instead; this is a no-op there.
void sync_directory(const std::filesystem::path& directory);
// Durable replacement: write a sibling temporary file, sync it, rename it over
// `path` and sync the directory. A crash leaves the old or the new content.
void replace_file_durably(const std::filesystem::path& path, std::string_view contents,
                          bool owner_only = true);
} // namespace asterion
