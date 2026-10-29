#include <asterion/kernel/logger.hpp>
#include <asterion/kernel/trace.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <algorithm>
#include <asterion/kernel/environment.hpp>
#include <chrono>
#include <cctype>
#include <ctime>
#include <regex>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <mutex>
#include <fstream>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/owner.hpp>

namespace asterion {
namespace {

std::atomic<std::uint64_t> log_failures{0};
std::mutex process_mutex;
std::string process_program, process_instance;
std::filesystem::path process_policy;
const auto process_started = SystemClock{}.utc_now();

void record_log_failure() noexcept
{
    const auto count = log_failures.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!count || (count & (count - 1)))
        return;
    // Independent of spdlog, allocation and the failing sink. Never echo the
    // original payload, path, exception text or credentials, even on failure.
    char notice[160];
    const int size = std::snprintf(
        notice,
        sizeof notice,
        "{\"event\":\"logging.failed\",\"level\":\"error\",\"failures\":%llu}\n",
        static_cast<unsigned long long>(count));
    if (size > 0 && static_cast<std::size_t>(size) < sizeof notice)
        (void)std::fwrite(notice, 1, static_cast<std::size_t>(size), stderr);
}

spdlog::level::level_enum native_level(LogLevel level)
{
    switch (level) {
        case LogLevel::debug:
            return spdlog::level::debug;
        case LogLevel::info:
            return spdlog::level::info;
        case LogLevel::warning:
            return spdlog::level::warn;
        case LogLevel::error:
            return spdlog::level::err;
        case LogLevel::off:
            return spdlog::level::off;
    }
    throw Error(ErrorCode::invalid_request, "invalid log level");
}

void redact(Json& value, std::size_t depth = 0)
{
    if (depth > 16)
        throw Error(ErrorCode::invalid_request, "log nesting limit exceeded");
    if (value.is_array()) {
        for (auto& item : value)
            redact(item, depth + 1);
    }
    if (!value.is_object())
        return;
    for (auto& [key, item] : value.items()) {
        std::string normalized = key;
        std::transform(
            normalized.begin(),
            normalized.end(),
            normalized.begin(),
            [](unsigned char c) {
                return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A'))
                                            : static_cast<char>(c);
            });
        if (normalized.find("password") != std::string::npos
            || normalized.find("passwd") != std::string::npos
            || normalized.find("secret") != std::string::npos
            || normalized.find("token") != std::string::npos
            || normalized.find("credential") != std::string::npos || normalized == "authorization"
            || normalized == "api_key" || normalized == "apikey")
            item = "[REDACTED]";
        else
            redact(item, depth + 1);
    }
}

// Deletes only this logger's own dated files, "<stem>_YYYY-MM-DD<extension>",
// whose date is older than the retention window.
void remove_expired(const std::filesystem::path& file, unsigned retention_days)
{
    const auto directory = file.parent_path();
    const auto stem = file.stem().string(), extension = file.extension().string();
    const std::regex dated(R"((\d{4})-(\d{2})-(\d{2}))");
    // File names use the local date, as the daily sink does.
    const auto now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    const auto today = std::chrono::sys_days(
        std::chrono::year_month_day{
            std::chrono::year(local.tm_year + 1900),
            std::chrono::month(local.tm_mon + 1),
            std::chrono::day(local.tm_mday)
        });
    std::error_code ignored;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ignored)) {
        const auto name = entry.path().filename().string();
        if (!entry.is_regular_file() || entry.is_symlink()
            || name.size() != stem.size() + 11 + extension.size() || !name.starts_with(stem + "_")
            || !name.ends_with(extension))
            continue;
        std::smatch date;
        const auto text = name.substr(stem.size() + 1, 10);
        if (!std::regex_match(text, date, dated))
            continue;
        const std::chrono::year_month_day day{
            std::chrono::year(std::stoi(date[1])),
            std::chrono::month(std::stoul(date[2])),
            std::chrono::day(std::stoul(date[3]))
        };
        if (day.ok() && std::chrono::sys_days(day) < today - std::chrono::days(retention_days))
            std::filesystem::remove(entry.path(), ignored);
    }
}

std::optional<LogLevel> level_named(const Json& value)
{
    static const std::map<std::string, LogLevel> levels{
        {"debug", LogLevel::debug},
        {"info", LogLevel::info},
        {"warning", LogLevel::warning},
        {"error", LogLevel::error},
        {"off", LogLevel::off}
    };
    if (!value.is_string())
        return std::nullopt;
    const auto found = levels.find(value.get<std::string>());
    return found == levels.end() ? std::nullopt : std::optional(found->second);
}

LoggerOptions logging_policy(const std::filesystem::path& directory)
{
    LoggerOptions options;
    const auto file = process_policy.empty() ? directory.parent_path() / "config" / "logging.json"
                                             : process_policy;
    if (process_policy.empty() && !std::filesystem::exists(file))
        return options;
    std::ifstream input(file);
    if (!input)
        throw Error(
            ErrorCode::invalid_request,
            "cannot read logging configuration: " + file.string());
    const auto config = Json::parse(input, nullptr, false);
    const auto invalid = [&](const std::string& key) {
        return Error(
            ErrorCode::invalid_request,
            "invalid logging configuration " + file.string() + ": " + key);
    };
    const auto exact =
        [&](const Json& object, std::set<std::string> keys, const std::string& where) {
            if (!object.is_object())
                throw invalid(where.empty() ? "not an object" : where);
            for (const auto& [key, _] : object.items())
                if (!keys.erase(key))
                    throw invalid((where.empty() ? "" : where + ".") + key + " is not a setting");
            for (const auto& key : keys)
                if (key != "pattern")
                    throw invalid((where.empty() ? "" : where + ".") + key + " is missing");
        };
    exact(config, {"format", "retention_days", "console", "file"}, "");
    if (config.at("format") != "json" && config.at("format") != "text")
        throw invalid("format must be json or text");
    options.text = config.at("format") == "text";
    const auto& days = config.at("retention_days");
    if (!days.is_number_unsigned() || days.get<std::uint64_t>() < 1
        || days.get<std::uint64_t>() > 366)
        throw invalid("retention_days must be 1-366");
    options.retention_days = static_cast<unsigned>(days.get<std::uint64_t>());
    for (const auto& [name, sink] :
        {std::pair<std::string, LogSinkOptions*>{"console", &options.console},
            {"file", &options.file}}) {
        const auto& value = config.at(name);
        exact(value, {"enabled", "level", "pattern"}, name);
        if (!value.at("enabled").is_boolean())
            throw invalid(name + ".enabled must be true or false");
        sink->enabled = value.at("enabled").get<bool>();
        const auto level = level_named(value.at("level"));
        if (!level)
            throw invalid(name + ".level must be debug, info, warning, error or off");
        sink->level = *level;
        if (value.contains("pattern")) {
            const auto& pattern = value.at("pattern");
            if (!options.text)
                throw invalid(name + ".pattern applies only to the text format");
            if (!pattern.is_string() || pattern.get<std::string>().empty()
                || pattern.get<std::string>().size() > 256)
                throw invalid(name + ".pattern must be 1-256 characters");
            sink->pattern = pattern.get<std::string>();
        }
    }
    return options;
}

// The sinks options ask for, each with its level and, for text, its pattern.
std::vector<spdlog::sink_ptr> make_sinks(const LoggerOptions& options)
{
    std::vector<spdlog::sink_ptr> sinks;
    const auto configure = [&](spdlog::sink_ptr sink, const LogSinkOptions& output) {
        sink->set_level(native_level(output.level));
        sink->set_pattern(options.text ? output.pattern : "%v");
        sinks.push_back(std::move(sink));
    };
    if (options.console.enabled)
        configure(std::make_shared<spdlog::sinks::stderr_sink_mt>(), options.console);
    if (options.file.enabled) {
        if (!options.file_path.is_absolute() || !options.retention_days
            || options.retention_days > 366)
            throw Error(ErrorCode::invalid_request, "invalid log file or retention");
        std::filesystem::create_directories(options.file_path.parent_path());
        remove_expired(options.file_path, options.retention_days);
        const auto utf8 = options.file_path.u8string();
        configure(
            std::make_shared<spdlog::sinks::daily_file_sink_mt>(
                std::string(utf8.begin(), utf8.end()),
                0,
                0,
                false,
                static_cast<std::uint16_t>(options.retention_days)),
            options.file);
    }
    return sinks;
}

// The logger passes what its most verbose output wants; each sink filters.
spdlog::level::level_enum lowest_level(const LoggerOptions& options)
{
    auto level = spdlog::level::off;
    for (const auto* output : {&options.console, &options.file})
        if (output->enabled)
            level = std::min(level, native_level(output->level));
    return level;
}

// An event and its fields as key=value; text with spaces, quotes or "=" and
// nested values are written as JSON.
std::string text_record(std::string_view event, const Json& fields)
{
    std::string line(event);
    for (const auto& [key, value] : fields.items()) {
        line += " " + key + "=";
        if (value.is_string()) {
            const auto& text = value.get_ref<const std::string&>();
            line += text.empty() || text.find_first_of(" \t\n\"=") != std::string::npos
                ? value.dump()
                : text;
        } else
            line += value.dump();
    }
    return line;
}

} // namespace

std::uint64_t process_log_failures() noexcept
{
    return log_failures.load(std::memory_order_relaxed);
}

void initialize_process_logging(
    std::string program,
    std::string instance,
    std::filesystem::path policy)
{
    validate_id(program);
    if (!instance.empty())
        validate_id(instance);
    if (!policy.empty() && !std::filesystem::is_regular_file(policy))
        throw Error(
            ErrorCode::invalid_request,
            "cannot read logging configuration: " + policy.string());
    std::lock_guard lock(process_mutex);
    process_program = std::move(program);
    process_instance = std::move(instance);
    process_policy = std::move(policy);
}

std::filesystem::path process_log_file(
    const std::filesystem::path& directory,
    std::string_view program,
    std::uint64_t pid)
{
    const auto now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    char date[16];
    std::strftime(date, sizeof date, "%Y-%m-%d", &local);
    return directory / (std::string(program) + "-" + std::to_string(pid) + "_" + date + ".log");
}

void prune_process_logs(const std::filesystem::path& directory)
{
    const auto days = logging_policy(directory).retention_days;
    const auto limit = std::filesystem::file_time_type::clock::now() - std::chrono::days(days);
    const std::regex pattern(R"([A-Za-z0-9._-]+-[0-9]+_[0-9]{4}-[0-9]{2}-[0-9]{2}\.log)");
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_symlink() || !entry.is_regular_file()
            || !std::regex_match(entry.path().filename().string(), pattern))
            continue;
        if (entry.last_write_time() < limit)
            std::filesystem::remove(entry.path());
    }
}

std::shared_ptr<Logger> process_logger(const std::string& name)
{
    const auto directory = environment_path("ASTERION_LOG_DIRECTORY");
    if (!directory)
        return nullptr;
    validate_id(name);

    struct ProcessLog
    {
        std::filesystem::path directory;
        std::string program, instance;
        std::filesystem::path policy_file;
        LoggerOptions policy;
        std::vector<spdlog::sink_ptr> sinks;
        std::map<std::string, std::shared_ptr<Logger>> modules;
    };

    static ProcessLog current;
    std::lock_guard lock(process_mutex);
    const auto program =
        process_program.empty() ? current_executable().filename().string() : process_program;
    if (current.sinks.empty() || current.directory != *directory || current.program != program
        || current.instance != process_instance || current.policy_file != process_policy) {
        if (!directory->is_absolute())
            throw Error(ErrorCode::invalid_request, "invalid log file or retention");
        ProcessLog next;
        next.directory = *directory;
        next.program = program;
        next.instance = process_instance;
        next.policy_file = process_policy;
        next.policy = logging_policy(*directory);
        next.policy.file_path =
            *directory / (program + "-" + std::to_string(current_process_id()) + ".log");
        next.sinks = make_sinks(next.policy);
        if (next.sinks.empty())
            return nullptr; // Both outputs are switched off.
        current = std::move(next);
    }
    if (const auto found = current.modules.find(name); found != current.modules.end())
        return found->second;
    if (current.modules.size() >= 32)
        throw Error(ErrorCode::resource_exhausted, "process logger capacity exhausted");
    auto backend =
        std::make_shared<spdlog::logger>(name, current.sinks.begin(), current.sinks.end());
    backend->set_level(lowest_level(current.policy));
    backend->flush_on(spdlog::level::warn);
    auto logger = std::make_shared<Logger>(
        std::move(backend),
        Json{
            {"program", program},
            {"pid", current_process_id()},
            {"instance", process_instance},
            {"started_ns", std::to_string(process_started)}
        },
        current.policy.text);
    current.modules.emplace(name, logger);
    return logger;
}

void log_process_event(
    const std::string& name,
    LogLevel level,
    std::string_view event,
    Json fields) noexcept
{
    try {
        if (const auto logger = process_logger(name)) {
            logger->write(level, event, std::move(fields));
            logger->flush();
        }
    } catch (...) {
        record_log_failure();
    }
}

void log_process_failure(
    const std::string& name,
    std::string_view event,
    ErrorCode code,
    std::uint64_t count) noexcept
{
    if (!count || (count & (count - 1)))
        return;
    try {
        log_process_event(
            name,
            LogLevel::warning,
            event,
            {{"code", error_name(code)}, {"failures", count}});
    } catch (...) {
        record_log_failure();
    }
}

void log_identity_event(
    const std::string& name,
    std::string_view event,
    std::initializer_list<std::pair<std::string_view, std::string_view>> ids) noexcept
{
    try {
        Json fields = Json::object();
        if (const auto trace = current_trace_id(); !trace.empty()) {
            validate_id(trace);
            fields["trace_id"] = trace;
        }
        for (const auto& [key, value] : ids) {
            validate_id(key);
            if (!value.empty()) {
                validate_id(value);
                fields[std::string(key)] = value;
            }
        }
        log_process_event(name, LogLevel::info, event, std::move(fields));
    } catch (...) {
        record_log_failure();
    }
}

Logger::Logger(const LoggerOptions& options)
{
    validate_id(options.name);
    auto sinks = make_sinks(options);
    if (sinks.empty())
        throw Error(ErrorCode::invalid_request, "logger requires a sink");
    backend_ = std::make_shared<spdlog::logger>(options.name, sinks.begin(), sinks.end());
    backend_->set_level(lowest_level(options));
    backend_->flush_on(spdlog::level::warn);
    text_ = options.text;
    configure_errors();
}

Logger::Logger(std::shared_ptr<spdlog::logger> backend, Json process, bool text)
    : backend_(std::move(backend)),
      process_(std::move(process)),
      text_(text)
{
    if (!backend_)
        throw Error(ErrorCode::invalid_request, "null logger backend");
    if (!text_)
        backend_->set_pattern("%v");
    configure_errors();
}

void Logger::configure_errors()
{
    backend_->set_error_handler([failures = failures_](const std::string&) {
        ++*failures;
        record_log_failure();
    });
}

bool Logger::write(LogLevel level, std::string_view event, Json fields) noexcept
{
    try {
        const auto native = native_level(level);
        if (level == LogLevel::off || !backend_->should_log(native))
            return true;
        validate_id(event);
        if (!fields.is_object())
            throw Error(ErrorCode::invalid_request, "log fields must be an object");
        redact(fields);
        const auto level_name = std::string(
            spdlog::level::to_string_view(native).data(),
            spdlog::level::to_string_view(native).size());
        auto wire = text_ ? text_record(event, fields)
                          : Json{
                                {"event", event},
                                {"level", level_name},
                                {"module", backend_->name()},
                                {"process", process_},
                                {"timestamp_ns", std::to_string(SystemClock{}.utc_now())},
                                {"fields", std::move(fields)}
                            }.dump();
        if (wire.size() > 16384)
            throw Error(ErrorCode::resource_exhausted, "log record too large");
        const auto before = failures_->load();
        backend_->log(native, "{}", wire);
        return failures_->load() == before;
    } catch (...) {
        ++*failures_;
        record_log_failure();
        return false;
    }
}

void Logger::flush() noexcept
{
    try {
        backend_->flush();
    } catch (...) {
        ++*failures_;
        record_log_failure();
    }
}

} // namespace asterion
