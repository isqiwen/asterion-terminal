#include <asterion/kernel/logger.hpp>
#include <asterion/kernel/native_plugin.hpp>
#include "engine.hpp"
#include "sqlite_journal.hpp"
#include "task_worker.hpp"
#include <CLI/CLI.hpp>
#include <asterion/kernel/process/owner.hpp>
#include <fstream>
#include <iostream>
#include <thread>
#include <stdexcept>
int main(int argc, char** argv) {
  CLI::App app{"Asterion single-day futures backtest (SMA long/flat, next-tick "
               "limit fills)"};
  app.set_version_flag("--version", "asterion-backtest " ASTERION_PRODUCT_VERSION);
  std::string input_path, output_path, endpoint, host, service, task;
  std::uint64_t owner_pid = 0;
  app.add_option("--owner-pid", owner_pid, "Owning supervisor process");
  unsigned short port = 0;
  asterion::ipc::TlsIdentity tls;
  app.add_option("--endpoint", endpoint, "Task service local IPC endpoint");
  app.add_option("--host", host, "Task service TCP host");
  app.add_option("--port", port)->check(CLI::Range(1, 65535));
  app.add_option("--tls-ca", tls.ca_file);
  app.add_option("--tls-cert", tls.certificate_file);
  app.add_option("--tls-key", tls.private_key_file);
  app.add_option("--session", service, "Task service identity");
  app.add_option("--task", task, "Queued task identity");
  app.add_option("--input", input_path, "Typed BacktestInput Protobuf file")
      ->check(CLI::ExistingFile);
  app.add_option("--directory", output_path, "Existing empty result journal directory")
      ->check(CLI::ExistingDirectory);
  std::string plugin_directory;
  app.add_option("--plugin-directory", plugin_directory)->check(CLI::ExistingDirectory);
  argv = app.ensure_utf8(argv);
  CLI11_PARSE(app, argc, argv);
  if (argc == 1) {
    std::cerr << "asterion-backtest: supply --input and --directory to run a "
                 "backtest.\n";
    return 3;
  }
  try {
    if (!plugin_directory.empty())
      asterion::configure_native_plugins(
          std::filesystem::path(std::u8string(plugin_directory.begin(), plugin_directory.end())));
    std::unique_ptr<asterion::ProcessOwner> owner;
    if (owner_pid)
      owner = std::make_unique<asterion::ProcessOwner>(owner_pid);
    std::jthread owner_watch([&](std::stop_token stop) {
      while (owner && !stop.stop_requested()) {
        if (!owner->alive())
          std::_Exit(4);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    });
    const bool worker = !task.empty() || !service.empty() || !endpoint.empty() || !host.empty() ||
                        port || !tls.ca_file.empty() || !tls.certificate_file.empty() ||
                        !tls.private_key_file.empty();
    if (worker) {
      if (!input_path.empty() || !output_path.empty())
        throw std::invalid_argument("choose task worker OR standalone file mode");
      return asterion::backtest::run_task(endpoint, host, port, tls, service, task);
    }
    if (input_path.empty() || output_path.empty())
      throw std::invalid_argument("both --input and --directory are required");
    const auto path = std::filesystem::path(std::u8string(input_path.begin(), input_path.end()));
    if (std::filesystem::is_symlink(path) || std::filesystem::file_size(path) > 16 * 1024 * 1024)
      throw std::invalid_argument("invalid backtest input file");
    std::ifstream stream(path, std::ios::binary);
    const std::string raw{std::istreambuf_iterator<char>(stream), {}};
    asterion::research::v1::BacktestInput input;
    if (stream.bad() || !input.ParseFromString(raw))
      throw std::invalid_argument("invalid backtest Protobuf");
    asterion::backtest::validate(input);
    if (std::filesystem::is_symlink(
            std::filesystem::path(std::u8string(output_path.begin(), output_path.end()))))
      throw std::invalid_argument("result directory must not be a symbolic link");
    asterion::SqliteJournal output(std::filesystem::absolute(
        std::filesystem::path(std::u8string(output_path.begin(), output_path.end()))));
    output.start();
    if (!output.read().empty())
      throw std::invalid_argument("result directory is not empty; existing "
                                  "results are never overwritten");
    const auto result = asterion::backtest::run(input);
    output.append({{"version", 1},
                   {"type", "backtest.result"},
                   {"input", asterion::protocol::decode_backtest(input)},
                   {"result", asterion::backtest::result_json(result)}});
    std::cout << asterion::backtest::result_json(result).dump() << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Backtest failed: " << error.what() << '\n';
    asterion::log_process_event("backtest", asterion::LogLevel::error, "service.failed",
                                {{"message", error.what()}});
    return 1;
  }
}
