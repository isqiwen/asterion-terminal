#pragma once
#include <asterion/base/serialization.hpp>
#include <spdlog/fmt/fmt.h>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <source_location>
#include <type_traits>

namespace asterion {

// The installation the running program belongs to: the parent of the bin/
// directory in the path it was started by (argv[0]). Symbolic links are kept,
// so a link placed in an installation's bin/ belongs to that installation.
//
//   <installation>/bin/     the program
//   <installation>/lib/     its libraries and plugins
//   <installation>/config/  its configuration, logging.json among it
//   <installation>/logs/
//   <installation>/data/
std::filesystem::path installation_directory();

} // namespace asterion

// The process log, configured by <installation>/config/logging.json:
//
//   {"console": {"enabled": false, "level": "info", "pattern": "…"},
//    "file": {"enabled": true, "level": "info", "pattern": "…",
//             "path": "../logs/{program}-{pid}.log", "retention_days": 30}}
//
// Levels are debug, info, warning or error. A pattern (spdlog syntax, optional)
// defaults to "[%Y-%m-%d %H:%M:%S.%e %z] [%l] [%n] [%s:%#] %v", %n being the program. The
// file path, relative to the program's bin/ or absolute, may use {program}
// and {pid}; files are split by local day ("<stem>_YYYY-MM-DD.log") and those
// older than retention_days are removed when the log opens. Without the file
// records go to standard error at info. Every record is written at once.
//
// Messages are free text: never format a password, token, authentication code
// or credential into one.
namespace asterion::log {

enum class Level
{
    debug,
    info,
    warning,
    error
};

// Opens the log for this program, named after its executable; an invalid
// configuration throws, naming the file and the setting. Without a call the
// log opens on first use, and an invalid configuration then sends records to
// standard error. Call before starting threads that log.
void init();
// For a program that does not run from an installation's bin/ (the Terminal
// inside its application): its installation and its name.
void init(const std::filesystem::path& installation, std::string_view program);

// Today's log file of a process of `program` in `installation`, by that
// installation's settings; empty when it writes no file.
std::filesystem::path
file(const std::filesystem::path& installation, std::string_view program, std::uint64_t pid);

bool enabled(Level level) noexcept;
void write(
    Level level,
    std::string_view message,
    std::source_location location = std::source_location::current()) noexcept;
// Records that could not be formatted or written in this process.
std::uint64_t failures() noexcept;

template <class... Args>
struct Format
{
    template <class T>
    consteval Format(const T& value, std::source_location source = std::source_location::current())
        : text(value),
          location(source)
    {}

    fmt::format_string<Args...> text;
    std::source_location location;
};

template <class... Args>
void emit(Level level, Format<std::type_identity_t<Args>...> format, Args&&... args) noexcept
{
    if (!enabled(level))
        return;
    try {
        write(level, fmt::format(format.text, std::forward<Args>(args)...), format.location);
    } catch (...) {
        write(Level::error, "a log record could not be formatted", format.location);
    }
}

template <class... Args>
void debug(Format<std::type_identity_t<Args>...> format, Args&&... args) noexcept
{
    emit(Level::debug, format, std::forward<Args>(args)...);
}

template <class... Args>
void info(Format<std::type_identity_t<Args>...> format, Args&&... args) noexcept
{
    emit(Level::info, format, std::forward<Args>(args)...);
}

template <class... Args>
void warn(Format<std::type_identity_t<Args>...> format, Args&&... args) noexcept
{
    emit(Level::warning, format, std::forward<Args>(args)...);
}

template <class... Args>
void error(Format<std::type_identity_t<Args>...> format, Args&&... args) noexcept
{
    emit(Level::error, format, std::forward<Args>(args)...);
}

} // namespace asterion::log
