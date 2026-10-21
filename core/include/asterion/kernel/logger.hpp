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

struct LoggerOptions
{
    std::string name = "asterion";
    LogLevel level = LogLevel::info;
    bool stderr_sink = true;
    // One file per local day, named "<stem>_YYYY-MM-DD<extension>"; files older
    // than retention_days are removed when the logger opens and at rotation.
    std::filesystem::path file;
    unsigned retention_days = 30;
};
class Logger;
// Configure before starting application threads. Directory is inherited from
// ASTERION_LOG_DIRECTORY; the optional node-level config/logging.json owns policy.
void initialize_process_logging(std::string program, std::string instance = {});
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

// Owns an spdlog logger (not the global registry). Thread-safe sinks; JSON lines.
class Logger final
{
public:
    explicit Logger(const LoggerOptions& options = {});
    explicit Logger(std::shared_ptr<spdlog::logger> backend, Json process = Json::object());
    bool write(LogLevel level, std::string_view event, Json fields = Json::object()) noexcept;
    void flush() noexcept;

    std::uint64_t failures() const noexcept
    {
        return failures_->load();
    }

private:
    std::shared_ptr<spdlog::logger> backend_;
    Json process_ = Json::object();
    std::shared_ptr<std::atomic<std::uint64_t>> failures_ =
        std::make_shared<std::atomic<std::uint64_t>>(0);
    void configure_errors();
};

} // namespace asterion
