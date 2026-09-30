#include <asterion/kernel/native_plugin.hpp>
#include "history_minutes.hpp"
#include "history_daily.hpp"
#include "history_providers.hpp"
#include <CLI/CLI.hpp>
#include <asterion/kernel/process/owner.hpp>
#include <asterion/protocol/task_client.hpp>
#include <algorithm>
#include <iostream>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
int main(int argc, char** argv) {
  CLI::App app{"Asterion historical data downloads from data-source plugins"};
  app.set_version_flag("--version", "asterion-data-pipeline " ASTERION_PRODUCT_VERSION);
  std::string endpoint, host, service, task;
  unsigned short port = 0;
  asterion::ipc::TlsIdentity tls;
  std::uint64_t owner_pid = 0;
  app.add_option("--owner-pid", owner_pid);
  app.add_option("--endpoint", endpoint);
  app.add_option("--host", host);
  app.add_option("--port", port)->check(CLI::Range(1, 65535));
  app.add_option("--tls-ca", tls.ca_file);
  app.add_option("--tls-cert", tls.certificate_file);
  app.add_option("--tls-key", tls.private_key_file);
  app.add_option("--session", service);
  app.add_option("--task", task);
  bool minute_worker = false, daily_worker = false;
  app.add_flag("--daily-download", daily_worker, "Run a managed daily-data download");
  app.add_flag("--minute-download", minute_worker, "Run a managed minute-data download");
  argv = app.ensure_utf8(argv);
  std::string plugin_directory;
  app.add_option("--plugin-directory", plugin_directory)->check(CLI::ExistingDirectory);
  CLI11_PARSE(app, argc, argv);
  try {
    if (!plugin_directory.empty())
      asterion::configure_native_plugins(plugin_directory);
    std::unique_ptr<asterion::ProcessOwner> owner;
    if (owner_pid)
      owner = std::make_unique<asterion::ProcessOwner>(owner_pid);
    std::jthread watch([&](std::stop_token stop) {
      while (owner && !stop.stop_requested()) {
        if (!owner->alive())
          std::_Exit(4);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    });
    if (minute_worker == daily_worker)
      throw std::invalid_argument("choose exactly one of --minute-download or --daily-download");
    {
      return asterion::protocol::run_task_worker(
          endpoint, host, port, tls, service, task,
          daily_worker ? asterion::research::v1::DAILY_DOWNLOAD
                       : asterion::research::v1::MINUTE_DOWNLOAD,
          [daily_worker](const auto& attempt, auto stop, const auto& progress) {
            const auto& input = attempt.task();
            const auto source = daily_worker ? input.daily().source() : input.minutes().source();
            if (input.provider_artifact() != asterion::history_providers::artifact(source))
              throw std::invalid_argument("download provider artifact does not match task");
            asterion::research::v1::TaskFinish result;
            {
              std::stop_source cancellation;
              std::stop_callback forward(stop, [&] { cancellation.request_stop(); });
              std::atomic<unsigned> completed = 0;
              std::exception_ptr failure;
              std::jthread heartbeat([&](std::stop_token done) {
                try {
                  while (!done.stop_requested()) {
                    progress(completed.load(), input.total());
                    std::mutex m;
                    std::condition_variable_any cv;
                    std::unique_lock lock(m);
                    cv.wait_for(lock, done, std::chrono::seconds(1), [] { return false; });
                  }
                } catch (...) {
                  failure = std::current_exception();
                  cancellation.request_stop();
                }
              });
              const auto directory = std::filesystem::path(std::u8string(
                  attempt.output_directory().begin(), attempt.output_directory().end()));
              try {
                if (daily_worker) {
                  auto provider = asterion::history_providers::daily(input.daily().source(),
                                                                     attempt.provider_token());
                  provider->start();
                  (void)asterion::history_files::download_daily(
                      *provider, asterion::history_files::daily_range(input.daily()), directory,
                      input.daily().requests_per_minute(), cancellation.get_token(),
                      [&](unsigned value, unsigned, std::uint64_t) { completed.store(value); });
                  *result.mutable_daily() = asterion::history_files::daily_result(directory);
                } else {
                  auto provider = asterion::history_providers::minutes(input.minutes().source(),
                                                                       attempt.provider_token());
                  provider->start();
                  (void)asterion::history_files::download_minutes(
                      *provider, asterion::history_files::minute_range(input.minutes()), directory,
                      input.minutes().requests_per_minute(), cancellation.get_token(),
                      [&](unsigned value, unsigned, std::uint64_t) { completed.store(value); });
                  *result.mutable_minutes() = asterion::history_files::minute_result(directory);
                }
              } catch (...) {
                heartbeat.request_stop();
                heartbeat.join();
                if (failure)
                  std::rethrow_exception(failure);
                throw;
              }
              heartbeat.request_stop();
              heartbeat.join();
              if (failure)
                std::rethrow_exception(failure);
              return result;
            }
          });
    }
  } catch (const std::exception& error) {
    std::cerr << "Data download failed: " << error.what() << '\n';
    return 1;
  }
}
