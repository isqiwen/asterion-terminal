#include "data_host.hpp"
#include <CLI/CLI.hpp>
#include <asterion/kernel/logger.hpp>
#include <asterion/kernel/native_plugin.hpp>
#include <iostream>
using namespace std::chrono_literals;
int main(int argc, char** argv) {
  CLI::App app{"Asterion historical data service"};
  app.set_version_flag("--version", "asterion-data-service " ASTERION_PRODUCT_VERSION);
  std::string directory, service, task_instance, health_endpoint, worker_endpoint, plugins;
  std::uint64_t owner_pid = 0;
  unsigned file_workers = 2;
  asterion::service::Transport transport;
  app.add_option("--directory", directory)->required()->check(CLI::ExistingDirectory);
  app.add_option("--session", service)->required();
  app.add_option("--task-instance", task_instance)->required();
  app.add_option("--endpoint", transport.endpoint);
  app.add_option("--bind", transport.bind);
  app.add_option("--port", transport.port)->check(CLI::Range(1, 65535));
  app.add_option("--tls-ca", transport.tls.ca_file);
  app.add_option("--tls-cert", transport.tls.certificate_file);
  app.add_option("--tls-key", transport.tls.private_key_file);
  app.add_option("--worker-endpoint", worker_endpoint)->required();
  app.add_option("--health-endpoint", health_endpoint);
  app.add_option("--owner-pid", owner_pid);
  app.add_option("--file-workers", file_workers)->check(CLI::Range(1, 2));
  app.add_option("--plugin-directory", plugins)->check(CLI::ExistingDirectory);
  argv = app.ensure_utf8(argv);
  CLI11_PARSE(app, argc, argv);
  try {
    if (!plugins.empty())
      asterion::configure_native_plugins(plugins);
    asterion::validate_id(service);
    transport.validate();
    asterion::service::install_stop_signals();
    using Host = asterion::service::RpcHost;
    asterion::data::DataHost data({directory, service, task_instance, file_workers});
    Host::Options options;
    options.owner_pid = owner_pid;
    options.connections = 12;
    options.receive = 10s;
    options.send = 5s;
    options.local_endpoints.push_back(
        {worker_endpoint,
         [&](const Host::Peer&, std::string frame) { return data.accept(std::move(frame), true); },
         6, false, 1024 * 1024, 128 * 1024 * 1024});
    if (!health_endpoint.empty())
      options.local_endpoints.push_back(
          {health_endpoint,
           [&](const Host::Peer&, std::string frame) { return data.health(frame); }, 4, true});
    options.advance = [&](Host::Stage stage) { return data.advance(stage); };
    asterion::Progress io_progress;
    Host host(
        transport,
        [&](const Host::Peer&, std::string frame) { return data.accept(std::move(frame), false); },
        std::move(options), io_progress);
    data.open();
    if (!host.run())
      std::_Exit(0);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Data service failed: " << error.what() << '\n';
    asterion::log_process_event("data-service", asterion::LogLevel::error, "service.failed",
                                {{"message", error.what()}});
    return 1;
  }
}
