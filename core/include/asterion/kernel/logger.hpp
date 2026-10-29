#pragma once
#include <asterion/foundation/serialization.hpp>
#include <atomic>
#include <filesystem>
#include <memory>
#include <initializer_list>

namespace spdlog {

class logger;

} // namespace spdlog

namespace asterion {

enum class LogLevel
{
    debug,
    info,
    warning,
    error,
    off
};

// One output of a logger: standard error or the daily files.
struct LogSinkOptions
{
    bool enabled = false;
    LogLevel level = LogLevel::info;
    // spdlog pattern of a text record; %n is the module and %v the event with
    // its fields. JSON records are written as they are.
    std::string pattern = "%Y-%m-%d %H:%M:%S.%e %z %l %n %v";
};

struct LoggerOptions
{
    std::string name = "asterion";
    // One readable line per record instead of a JSON object.
    bool text = false;
    LogSinkOptions console;
    // One file per local day, named "<stem>_YYYY-MM-DD<extension>" after
    // file_path; files older than retention_days are removed when the logger
    // opens and at rotation.
    LogSinkOptions file{true};
    std::filesystem::path file_path;
    unsigned retention_days = 30;
};
class Logger;
// Configure before starting application threads. Directory is inherited from
// ASTERION_LOG_DIRECTORY. Policy comes from `policy`, which must exist when
// given, or else from the optional node-level config/logging.json beside the
// log directory (without one: JSON records to the files at info):
//
//   {"format": "text", "retention_days": 30,
//    "console": {"enabled": false, "level": "info", "pattern": "…"},
//    "file": {"enabled": true, "level": "warning", "pattern": "…"}}
//
// format is "json" or "text"; levels are debug, info, warning, error or off;
// a pattern (spdlog syntax, optional) is allowed only with "text".
void initialize_process_logging(
    std::string program,
    std::string instance = {},
    std::filesystem::path policy = {});
std::filesystem::path process_log_file(
    const std::filesystem::path& directory,
    std::string_view program,
    std::uint64_t pid);
// Agent owns retention of exited processes' logs in its node directory.
void prune_process_logs(const std::filesystem::path& directory);
// Modules share the process sink; name is a record field, not a file name.
std::shared_ptr<Logger> process_logger(const std::string& name);
// Records one process event in that daily log when there is one; never throws.
void log_process_event(
    const std::string& name,
    LogLevel level,
    std::string_view event,
    Json fields = Json::object()) noexcept;
// Cumulative failed log writes/flushes/construction attempts in this process.
// Each failure also emits a fixed, payload-free stderr notice at powers of two.
std::uint64_t process_log_failures() noexcept;
// Bounded repeated-failure diagnostics: records counts 1, 2, 4, 8, ... only.
// Callers supply a cumulative count for a fixed stage; no exception text is logged.
void log_process_failure(
    const std::string& name,
    std::string_view event,
    ErrorCode code,
    std::uint64_t count) noexcept;
// An allowlisted set of identifiers, joined to the current trace when present.
// Values must satisfy validate_id; arbitrary messages or payloads are forbidden.
void log_identity_event(
    const std::string& name,
    std::string_view event,
    std::initializer_list<std::pair<std::string_view, std::string_view>> ids) noexcept;

// Owns an spdlog logger (not the global registry). Thread-safe sinks; JSON
// lines, or with `text` one line per record shaped by each sink's pattern:
//   2026-10-29 21:00:01.123 +08:00 info recorder catalog.applied phase=ready contracts=1234
// A text logger keeps the patterns its sinks were given.
class Logger final
{
public:
    explicit Logger(const LoggerOptions& options = {});
    explicit Logger(
        std::shared_ptr<spdlog::logger> backend,
        Json process = Json::object(),
        bool text = false);
    bool write(LogLevel level, std::string_view event, Json fields = Json::object()) noexcept;
    void flush() noexcept;

    std::uint64_t failures() const noexcept
    {
        return failures_->load();
    }

private:
    std::shared_ptr<spdlog::logger> backend_;
    Json process_ = Json::object();
    bool text_ = false;
    std::shared_ptr<std::atomic<std::uint64_t>> failures_ =
        std::make_shared<std::atomic<std::uint64_t>>(0);
    void configure_errors();
};

} // namespace asterion
