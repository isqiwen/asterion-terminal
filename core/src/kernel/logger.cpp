#include <asterion/kernel/logger.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <algorithm>
#include <asterion/kernel/environment.hpp>
#include <chrono>
#include <regex>
namespace asterion {
namespace {
spdlog::level::level_enum native_level(LogLevel level) {
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
void redact(Json& value, std::size_t depth = 0) {
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
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
      return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : static_cast<char>(c);
    });
    if (normalized.find("password") != std::string::npos ||
        normalized.find("passwd") != std::string::npos ||
        normalized.find("secret") != std::string::npos ||
        normalized.find("token") != std::string::npos ||
        normalized.find("credential") != std::string::npos || normalized == "authorization" ||
        normalized == "api_key" || normalized == "apikey")
      item = "[REDACTED]";
    else
      redact(item, depth + 1);
  }
}
// Deletes only this logger's own dated files, "<stem>_YYYY-MM-DD<extension>",
// whose date is older than the retention window.
void remove_expired(const std::filesystem::path& file, unsigned retention_days) {
  const auto directory = file.parent_path();
  const auto stem = file.stem().string(), extension = file.extension().string();
  const std::regex dated(R"((\d{4})-(\d{2})-(\d{2}))");
  const auto today = std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now());
  std::error_code ignored;
  for (const auto& entry : std::filesystem::directory_iterator(directory, ignored)) {
    const auto name = entry.path().filename().string();
    if (!entry.is_regular_file() || entry.is_symlink() ||
        name.size() != stem.size() + 11 + extension.size() || !name.starts_with(stem + "_") ||
        !name.ends_with(extension))
      continue;
    std::smatch date;
    const auto text = name.substr(stem.size() + 1, 10);
    if (!std::regex_match(text, date, dated))
      continue;
    const std::chrono::year_month_day day{std::chrono::year(std::stoi(date[1])),
                                          std::chrono::month(std::stoul(date[2])),
                                          std::chrono::day(std::stoul(date[3]))};
    if (day.ok() && std::chrono::sys_days(day) < today - std::chrono::days(retention_days))
      std::filesystem::remove(entry.path(), ignored);
  }
}
} // namespace
std::shared_ptr<Logger> process_logger(const std::string& name) {
  const auto directory = environment_path("ASTERION_LOG_DIRECTORY");
  if (!directory)
    return nullptr;
  LoggerOptions options;
  options.name = name;
  options.stderr_sink = false;
  options.file = *directory / (name + ".log");
  return std::make_shared<Logger>(options);
}
void log_process_event(const std::string& name, LogLevel level, std::string_view event,
                       Json fields) noexcept {
  try {
    if (const auto logger = process_logger(name)) {
      logger->write(level, event, std::move(fields));
      logger->flush();
    }
  } catch (...) {
  }
}
Logger::Logger(const LoggerOptions& options) {
  validate_id(options.name);
  std::vector<spdlog::sink_ptr> sinks;
  if (options.stderr_sink)
    sinks.push_back(std::make_shared<spdlog::sinks::stderr_sink_mt>());
  if (!options.file.empty()) {
    if (!options.file.is_absolute() || !options.retention_days || options.retention_days > 366)
      throw Error(ErrorCode::invalid_request, "invalid log file or retention");
    std::filesystem::create_directories(options.file.parent_path());
    remove_expired(options.file, options.retention_days);
    const auto utf8 = options.file.u8string();
    sinks.push_back(std::make_shared<spdlog::sinks::daily_file_sink_mt>(
        std::string(utf8.begin(), utf8.end()), 0, 0, false,
        static_cast<std::uint16_t>(options.retention_days)));
  }
  if (sinks.empty())
    throw Error(ErrorCode::invalid_request, "logger requires a sink");
  backend_ = std::make_shared<spdlog::logger>(options.name, sinks.begin(), sinks.end());
  backend_->set_level(native_level(options.level));
  backend_->set_pattern("%v");
  backend_->flush_on(spdlog::level::warn);
  configure_errors();
}
Logger::Logger(std::shared_ptr<spdlog::logger> backend) : backend_(std::move(backend)) {
  if (!backend_)
    throw Error(ErrorCode::invalid_request, "null logger backend");
  backend_->set_pattern("%v");
  configure_errors();
}
void Logger::configure_errors() {
  backend_->set_error_handler([failures = failures_](const std::string&) { ++*failures; });
}
bool Logger::write(LogLevel level, std::string_view event, Json fields) noexcept {
  try {
    const auto native = native_level(level);
    if (level == LogLevel::off || !backend_->should_log(native))
      return true;
    validate_id(event);
    if (!fields.is_object())
      throw Error(ErrorCode::invalid_request, "log fields must be an object");
    redact(fields);
    auto wire = Json{{"event", event},
                     {"level", std::string(spdlog::level::to_string_view(native).data(),
                                           spdlog::level::to_string_view(native).size())},
                     {"logger", backend_->name()},
                     {"timestamp_ns", std::to_string(SystemClock{}.utc_now())},
                     {"fields", std::move(fields)}}
                    .dump();
    if (wire.size() > 16384)
      throw Error(ErrorCode::resource_exhausted, "log record too large");
    const auto before = failures_->load();
    backend_->log(native, "{}", wire);
    return failures_->load() == before;
  } catch (...) {
    ++*failures_;
    return false;
  }
}
void Logger::flush() noexcept {
  try {
    backend_->flush();
  } catch (...) {
    ++*failures_;
  }
}
} // namespace asterion
