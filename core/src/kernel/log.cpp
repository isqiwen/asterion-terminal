#include <asterion/kernel/log.hpp>
#include <asterion/foundation/error.hpp>
#include <asterion/foundation/serialization.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/owner.hpp>
#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#ifdef __APPLE__
#include <crt_externs.h>
#endif

namespace asterion {
namespace {

namespace fs = std::filesystem;
constexpr auto default_pattern = "%Y-%m-%d %H:%M:%S.%e %z %l %n %v";

// argv[0] as the program was started, before any symbolic link is resolved.
std::string started_as()
{
#ifdef __APPLE__
    if (const auto* argv = *_NSGetArgv(); argv && argv[0])
        return argv[0];
#else
    std::ifstream input("/proc/self/cmdline", std::ios::binary);
    std::string first;
    if (input && std::getline(input, first, '\0'))
        return first;
#endif
    return {};
}

} // namespace

fs::path installation_directory()
{
    static const fs::path directory = [] {
        const auto argv0 = started_as();
        // Found through PATH, argv[0] carries no directory: the executable's
        // own location is all there is.
        const auto program = argv0.find('/') == std::string::npos
            ? current_executable()
            : fs::absolute(fs::path(argv0)).lexically_normal();
        return program.parent_path().parent_path();
    }();
    return directory;
}

namespace log {
namespace {

struct Output
{
    bool enabled = false;
    Level level = Level::info;
    std::string pattern = default_pattern;
};

struct Settings
{
    Output console{true}, file;
    std::string path = "../logs/{program}-{pid}.log";
    unsigned retention_days = 30;
};

std::mutex mutex;
std::shared_ptr<spdlog::logger> current;
std::atomic<std::uint64_t> failed{0};

spdlog::level::level_enum native(Level level)
{
    switch (level) {
        case Level::debug:
            return spdlog::level::debug;
        case Level::info:
            return spdlog::level::info;
        case Level::warning:
            return spdlog::level::warn;
        case Level::error:
            return spdlog::level::err;
    }
    return spdlog::level::info;
}

Settings read_settings(const fs::path& file)
{
    Settings settings;
    if (!fs::exists(file))
        return settings;
    std::ifstream input(file, std::ios::binary);
    const std::string raw{std::istreambuf_iterator<char>(input), {}};
    const auto config = Json::parse(raw, nullptr, false);
    const auto invalid = [&](const std::string& what) {
        return Error(
            ErrorCode::invalid_request,
            "invalid logging configuration " + file.string() + ": " + what);
    };
    const auto exact = [&](const Json& object,
                           const std::set<std::string>& required,
                           const std::set<std::string>& optional,
                           const std::string& where) {
        if (!object.is_object())
            throw invalid((where.empty() ? std::string("the file") : where) + " must be an object");
        const auto prefix = where.empty() ? std::string{} : where + ".";
        for (const auto& [key, _] : object.items())
            if (!required.contains(key) && !optional.contains(key))
                throw invalid(prefix + key + " is not a setting");
        for (const auto& key : required)
            if (!object.contains(key))
                throw invalid(prefix + key + " is missing");
    };
    static const std::map<std::string, Level> levels{
        {"debug", Level::debug},
        {"info", Level::info},
        {"warning", Level::warning},
        {"error", Level::error}
    };
    exact(config, {"console", "file"}, {}, "");
    const auto output = [&](const std::string& name, Output& into) {
        const auto& value = config.at(name);
        exact(
            value,
            name == "file" ? std::set<std::string>{"enabled", "level", "path", "retention_days"}
                           : std::set<std::string>{"enabled", "level"},
            {"pattern"},
            name);
        if (!value.at("enabled").is_boolean())
            throw invalid(name + ".enabled must be true or false");
        into.enabled = value.at("enabled").get<bool>();
        const auto level = value.at("level").is_string()
            ? levels.find(value.at("level").get<std::string>())
            : levels.end();
        if (level == levels.end())
            throw invalid(name + ".level must be debug, info, warning or error");
        into.level = level->second;
        if (value.contains("pattern")) {
            const auto& pattern = value.at("pattern");
            if (!pattern.is_string() || pattern.get<std::string>().empty()
                || pattern.get<std::string>().size() > 256)
                throw invalid(name + ".pattern must be 1-256 characters");
            into.pattern = pattern.get<std::string>();
        }
    };
    output("console", settings.console);
    output("file", settings.file);
    const auto& file_settings = config.at("file");
    const auto& path = file_settings.at("path");
    if (!path.is_string() || path.get<std::string>().empty())
        throw invalid("file.path must be a path");
    settings.path = path.get<std::string>();
    const auto& days = file_settings.at("retention_days");
    if (!days.is_number_unsigned() || days.get<std::uint64_t>() < 1
        || days.get<std::uint64_t>() > 366)
        throw invalid("file.retention_days must be 1-366");
    settings.retention_days = static_cast<unsigned>(days.get<std::uint64_t>());
    return settings;
}

std::string substitute(std::string text, const std::string& key, const std::string& value)
{
    for (auto at = text.find(key); at != std::string::npos; at = text.find(key, at + value.size()))
        text.replace(at, key.size(), value);
    return text;
}

// Daily files ("<stem>_YYYY-MM-DD.log") older than the retention, of any
// program or process in this log directory.
void remove_expired(const fs::path& directory, unsigned retention_days)
{
    static const std::regex daily(R"(.+_([0-9]{4})-([0-9]{2})-([0-9]{2})\.log)");
    const auto today = std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now());
    std::error_code ignored;
    for (const auto& entry : fs::directory_iterator(directory, ignored)) {
        std::smatch match;
        const auto name = entry.path().filename().string();
        if (entry.is_symlink() || !entry.is_regular_file() || !std::regex_match(name, match, daily))
            continue;
        const std::chrono::year_month_day day{
            std::chrono::year(std::stoi(match[1])),
            std::chrono::month(static_cast<unsigned>(std::stoi(match[2]))),
            std::chrono::day(static_cast<unsigned>(std::stoi(match[3])))
        };
        if (day.ok() && std::chrono::sys_days(day) < today - std::chrono::days(retention_days))
            fs::remove(entry.path(), ignored);
    }
}

fs::path file_base(
    const Settings& settings,
    const fs::path& installation,
    const std::string& program,
    std::uint64_t pid)
{
    auto path = fs::path(
        substitute(substitute(settings.path, "{program}", program), "{pid}", std::to_string(pid)));
    if (path.is_relative())
        path = installation / "bin" / path;
    return path.lexically_normal();
}

std::shared_ptr<spdlog::logger> open(const fs::path& installation, const std::string& program)
{
    const auto settings = read_settings(installation / "config" / "logging.json");
    std::vector<spdlog::sink_ptr> sinks;
    auto lowest = spdlog::level::off;
    const auto add = [&](spdlog::sink_ptr sink, const Output& output) {
        sink->set_level(native(output.level));
        sink->set_pattern(output.pattern);
        lowest = std::min(lowest, native(output.level));
        sinks.push_back(std::move(sink));
    };
    if (settings.console.enabled)
        add(std::make_shared<spdlog::sinks::stderr_sink_mt>(), settings.console);
    if (settings.file.enabled) {
        const auto path = file_base(settings, installation, program, current_process_id());
        fs::create_directories(path.parent_path());
        remove_expired(path.parent_path(), settings.retention_days);
        add(std::make_shared<spdlog::sinks::daily_file_sink_mt>(
                path.string(),
                0,
                0,
                false,
                static_cast<std::uint16_t>(settings.retention_days)),
            settings.file);
    }
    auto logger = std::make_shared<spdlog::logger>(program, sinks.begin(), sinks.end());
    logger->set_level(lowest);
    // Few records, each worth keeping through a crash.
    logger->flush_on(spdlog::level::trace);
    logger->set_error_handler([](const std::string&) {
        const auto count = ++failed;
        // A fixed notice at powers of two; never the record or the cause.
        if ((count & (count - 1)) == 0)
            std::fprintf(
                stderr,
                "asterion: %llu log writes failed\n",
                static_cast<unsigned long long>(count));
    });
    return logger;
}

std::string program_name()
{
    const auto name = fs::path(started_as()).filename().string();
    return name.empty() ? current_executable().filename().string() : name;
}

std::shared_ptr<spdlog::logger> logger()
{
    std::lock_guard lock(mutex);
    if (!current) {
        try {
            current = open(installation_directory(), program_name());
        } catch (const std::exception& error) {
            current = std::make_shared<spdlog::logger>(
                program_name(),
                std::make_shared<spdlog::sinks::stderr_sink_mt>());
            current->set_pattern(default_pattern);
            current->error("{}", error.what());
        }
    }
    return current;
}

} // namespace

void init()
{
    auto opened = open(installation_directory(), program_name());
    std::lock_guard lock(mutex);
    current = std::move(opened);
}

void init(const fs::path& installation, std::string_view program)
{
    auto opened = open(installation, std::string(program));
    std::lock_guard lock(mutex);
    current = std::move(opened);
}

fs::path file(const fs::path& installation, std::string_view program, std::uint64_t pid)
{
    const auto settings = read_settings(installation / "config" / "logging.json");
    if (!settings.file.enabled)
        return {};
    const auto base = file_base(settings, installation, std::string(program), pid);
    const auto now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    char date[16];
    std::strftime(date, sizeof date, "_%Y-%m-%d", &local);
    return base.parent_path() / (base.stem().string() + date + base.extension().string());
}

bool enabled(Level level) noexcept
{
    try {
        return logger()->should_log(native(level));
    } catch (...) {
        return false;
    }
}

void write(Level level, std::string_view message) noexcept
{
    try {
        logger()->log(native(level), "{}", message);
    } catch (...) {
        ++failed;
    }
}

std::uint64_t failures() noexcept
{
    return failed.load();
}

} // namespace log
} // namespace asterion
