#include <asterion/kernel/logger.hpp>
#include <asterion/kernel/native_plugin.hpp>
#include "engine.hpp"
#include "risk_module.hpp"
#include <asterion/protocol/task_client.hpp>
#include <CLI/CLI.hpp>
#include <iostream>
#include <stdexcept>
int main(int argc, char** argv) {
  CLI::App app{"Asterion managed futures backtest worker"};
  app.set_version_flag("--version", "asterion-backtest " ASTERION_PRODUCT_VERSION);
  std::string endpoint, host, service, task;
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
  std::string plugin_directory;
  app.add_option("--plugin-directory", plugin_directory)->check(CLI::ExistingDirectory);
  argv = app.ensure_utf8(argv);
  CLI11_PARSE(app, argc, argv);
  try {
    if (!plugin_directory.empty())
      asterion::configure_native_plugins(
          std::filesystem::path(std::u8string(plugin_directory.begin(), plugin_directory.end())));
    return asterion::protocol::run_task_worker(
        endpoint, host, port, tls, service, task, asterion::task::v1::BACKTEST,
        [](const auto& input, auto stop, const auto& progress) {
          const auto module = asterion::risk_providers::Module::selected();
          if (module.artifact() != input.task().risk_artifact())
            throw std::invalid_argument("risk plugin artifact does not match its owner");
          asterion::task::v1::TaskFinish result;
          *result.mutable_result() =
              asterion::backtest::run(input.task().input(), stop, progress, &module);
          return result;
        },
        owner_pid);
  } catch (const std::exception& error) {
    std::cerr << "Backtest failed: " << error.what() << '\n';
    asterion::log_process_event("backtest", asterion::LogLevel::error, "service.failed",
                                {{"message", error.what()}});
    return 1;
  }
}
