#include <asterion/kernel/logger.hpp>
#include "factor_engine.hpp"
#include <CLI/CLI.hpp>
#include <asterion/protocol/task_client.hpp>
#include <iostream>
#include <stdexcept>
int main(int argc, char** argv) {
  CLI::App app{"Asterion managed factor evaluation worker"};
  app.set_version_flag("--version", "asterion-factor " ASTERION_PRODUCT_VERSION);
  std::string endpoint, host, service, task;
  bool daily = false;
  app.add_flag("--daily-factor", daily, "Evaluate a service-owned daily-close task");
  unsigned short port = 0;
  asterion::ipc::TlsIdentity tls;
  std::uint64_t owner_pid = 0;
  app.add_option("--owner-pid", owner_pid, "Owning supervisor process");
  app.add_option("--endpoint", endpoint);
  app.add_option("--host", host);
  app.add_option("--port", port)->check(CLI::Range(1, 65535));
  app.add_option("--tls-ca", tls.ca_file);
  app.add_option("--tls-cert", tls.certificate_file);
  app.add_option("--tls-key", tls.private_key_file);
  app.add_option("--session", service);
  app.add_option("--task", task);
  argv = app.ensure_utf8(argv);
  CLI11_PARSE(app, argc, argv);
  try {
    return asterion::protocol::run_task_worker(
        endpoint, host, port, tls, service, task,
        daily ? asterion::task::v1::DAILY_FACTOR : asterion::task::v1::FACTOR,
        [daily](const auto& input, auto stop, const auto& progress) {
          asterion::task::v1::TaskFinish result;
          if (daily)
            *result.mutable_daily_factor() =
                asterion::factor::run_daily(input.task().daily_factor(), stop, progress);
          else
            *result.mutable_factor() = asterion::factor::run(input.task().factor(), stop, progress);
          return result;
        },
        owner_pid);
  } catch (const std::exception& error) {
    std::cerr << "Factor analysis failed: " << error.what() << '\n';
    asterion::log_process_event("factor", asterion::LogLevel::error, "service.failed",
                                {{"message", error.what()}});
    return 1;
  }
}
