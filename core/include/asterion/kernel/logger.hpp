#pragma once
#include <asterion/foundation/serialization.hpp>
#include <atomic>
#include <filesystem>
#include <memory>
namespace spdlog {
class logger;
}
namespace asterion {
enum class LogLevel { debug, info, warning, error, off };
struct LoggerOptions {
  std::string name = "asterion";
  LogLevel level = LogLevel::info;
  bool stderr_sink = true;
  std::filesystem::path file;
  std::size_t max_file_bytes = 5 * 1024 * 1024;
  std::size_t retained_files = 3;
};
// Owns an spdlog logger (not the global registry). Thread-safe sinks; JSON lines.
class Logger final {
public:
  explicit Logger(const LoggerOptions& options = {});
  explicit Logger(std::shared_ptr<spdlog::logger> backend);
  bool write(LogLevel level, std::string_view event, Json fields = Json::object()) noexcept;
  void flush() noexcept;
  std::uint64_t failures() const noexcept { return failures_->load(); }

private:
  std::shared_ptr<spdlog::logger> backend_;
  std::shared_ptr<std::atomic<std::uint64_t>> failures_ =
      std::make_shared<std::atomic<std::uint64_t>>(0);
  void configure_errors();
};
} // namespace asterion
