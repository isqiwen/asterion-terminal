#include <asterion/kernel/logger.hpp>
#include <asterion/kernel/native_plugin.hpp>
#include "history_minutes.hpp"
#include "history_daily.hpp"
#include "history_providers.hpp"
#include <CLI/CLI.hpp>
#include <asterion/protocol/task_client.hpp>
#include <asterion/protocol/data_client.hpp>
#include <iostream>
#include <condition_variable>
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
    if (minute_worker == daily_worker)
      throw std::invalid_argument("choose exactly one of --minute-download or --daily-download");
    {
      return asterion::protocol::run_task_worker(
          endpoint, host, port, tls, service, task,
          daily_worker ? asterion::task::v1::DAILY_DOWNLOAD : asterion::task::v1::MINUTE_DOWNLOAD,
          [daily_worker, &service](const auto& attempt, auto stop, const auto& progress) {
            const auto& input = attempt.task();
            const auto source = daily_worker ? input.daily().source() : input.minutes().source();
            if (input.provider_artifact() != asterion::history_providers::artifact(source))
              throw std::invalid_argument("download provider artifact does not match task");
            asterion::protocol::DataClient data(attempt.data_endpoint(), attempt.data_instance());
            asterion::data::v1::DataRequest query;
            auto* identity = query.mutable_download_credentials();
            identity->set_data_instance(attempt.data_instance());
            identity->set_task_instance(service);
            identity->set_task_id(input.id());
            identity->set_attempt(input.attempt());
            const auto response = data.call(query);
            const auto& credentials = response.download_credentials();
            if (credentials.identity().SerializeAsString() != identity->SerializeAsString() ||
                credentials.authorization_id() != input.download_authorization())
              throw std::invalid_argument("download credentials do not match attempt");
            asterion::data::v1::DataRequest permit_request;
            *permit_request.mutable_acquire_download_permit() = *identity;
            const auto rpm = daily_worker ? input.daily().requests_per_minute()
                                          : input.minutes().requests_per_minute();
            auto next_request = std::chrono::steady_clock::now();
            auto budget = [&](std::stop_token cancelled) {
              std::mutex mutex;
              std::condition_variable_any changed;
              std::unique_lock lock(mutex);
              changed.wait_until(lock, cancelled, next_request, [] { return false; });
              while (!cancelled.stop_requested()) {
                const auto permit =
                    data.call(permit_request, std::chrono::seconds(5)).download_permit();
                if (permit.granted()) {
                  next_request = std::chrono::steady_clock::now() +
                                 std::chrono::milliseconds((60000 + rpm - 1) / rpm);
                  return;
                }
                if (!permit.retry_after_ms() || permit.retry_after_ms() > 60000)
                  throw std::invalid_argument("invalid download request permit");
                changed.wait_for(lock, cancelled,
                                 std::chrono::milliseconds(permit.retry_after_ms()),
                                 [] { return false; });
              }
              throw asterion::Error(asterion::ErrorCode::cancelled,
                                    "Historical download cancelled");
            };
            asterion::task::v1::TaskFinish result;
            const auto directory = std::filesystem::path(std::u8string(
                attempt.output_directory().begin(), attempt.output_directory().end()));
            if (daily_worker) {
              auto provider = asterion::history_providers::daily(input.daily().source(),
                                                                 credentials.credential(), budget);
              (void)asterion::history_files::download_daily(
                  *provider, asterion::history_files::daily_range(input.daily()), directory, stop,
                  [&](unsigned value, unsigned total, std::uint64_t) { progress(value, total); });
              *result.mutable_daily() = asterion::history_files::daily_result(directory);
            } else {
              auto provider = asterion::history_providers::minutes(
                  input.minutes().source(), credentials.credential(), budget);
              (void)asterion::history_files::download_minutes(
                  *provider, asterion::history_files::minute_range(input.minutes()), directory,
                  stop,
                  [&](unsigned value, unsigned total, std::uint64_t) { progress(value, total); });
              *result.mutable_minutes() = asterion::history_files::minute_result(directory);
            }
            return result;
          },
          owner_pid);
    }
  } catch (const std::exception& error) {
    std::cerr << "Data download failed: " << error.what() << '\n';
    asterion::log_process_event("data-pipeline", asterion::LogLevel::error, "service.failed",
                                {{"message", error.what()}});
    return 1;
  }
}
