#include "factor_engine.hpp"
#include "file_journal.hpp"
#include <CLI/CLI.hpp>
#include <asterion/kernel/process/owner.hpp>
#include <asterion/protocol/task_client.hpp>
#include <fstream>
#include <iostream>
#include <thread>
#include <stdexcept>
int main(int argc, char** argv) {
  CLI::App app{"Asterion bar-momentum factor evaluation and development-only "
               "window comparison"};
  app.set_version_flag("--version", "asterion-factor " ASTERION_PRODUCT_VERSION);
  std::string input_path, output_path, endpoint, host, service, task;
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
  app.add_option("--input", input_path, "Typed FactorInput Protobuf file")
      ->check(CLI::ExistingFile);
  app.add_option("--directory", output_path, "Existing empty result journal directory")
      ->check(CLI::ExistingDirectory);
  argv = app.ensure_utf8(argv);
  CLI11_PARSE(app, argc, argv);
  if (argc == 1) {
    std::cerr << "asterion-factor: supply --input and --directory to analyze "
                 "factors.\n";
    return 3;
  }
  try {
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
    if (!task.empty() || !service.empty() || !endpoint.empty() || !host.empty() || port ||
        !tls.ca_file.empty() || !tls.certificate_file.empty() || !tls.private_key_file.empty()) {
      if (!input_path.empty() || !output_path.empty())
        throw std::invalid_argument("choose task worker OR standalone file mode");
      return asterion::protocol::run_task_worker(
          endpoint, host, port, tls, service, task,
          daily ? asterion::research::v1::DAILY_FACTOR : asterion::research::v1::FACTOR,
          [daily](const auto& input, auto stop, const auto& progress) {
            asterion::research::v1::TaskFinish result;
            if (daily)
              *result.mutable_daily_factor() =
                  asterion::factor::run_daily(input.task().daily_factor(), stop, progress);
            else
              *result.mutable_factor() =
                  asterion::factor::run(input.task().factor(), stop, progress);
            return result;
          });
    }
    if (daily)
      throw std::invalid_argument("daily factor requires task worker mode");
    if (input_path.empty() || output_path.empty())
      throw std::invalid_argument("both --input and --directory are required");
    const auto path = std::filesystem::path(std::u8string(input_path.begin(), input_path.end()));
    const auto destination =
        std::filesystem::path(std::u8string(output_path.begin(), output_path.end()));
    if (std::filesystem::is_symlink(path) || !std::filesystem::is_regular_file(path) ||
        std::filesystem::file_size(path) > 16 * 1024 * 1024)
      throw std::invalid_argument("invalid factor input file");
    if (std::filesystem::is_symlink(destination))
      throw std::invalid_argument("result directory must not be a symbolic link");
    std::ifstream stream(path, std::ios::binary);
    const std::string raw{std::istreambuf_iterator<char>(stream), {}};
    asterion::research::v1::FactorInput input;
    if (stream.bad() || !input.ParseFromString(raw))
      throw std::invalid_argument("invalid factor Protobuf");
    asterion::factor::validate(input);
    asterion::FileJournal output(std::filesystem::absolute(destination));
    output.start();
    if (!output.read().empty())
      throw std::invalid_argument("result directory is not empty; existing "
                                  "results are never overwritten");
    const auto result = asterion::factor::run(input);
    const auto decoded = asterion::protocol::decode_factor_result(result);
    output.append({{"version", 1},
                   {"type", "factor.result"},
                   {"input", asterion::protocol::decode_factor(input)},
                   {"result", decoded}});
    std::cout << decoded.dump() << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Factor analysis failed: " << error.what() << '\n';
    return 1;
  }
}
