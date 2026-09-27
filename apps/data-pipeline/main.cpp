#include "pipeline.hpp"
#include "calendar.hpp"
#include <CLI/CLI.hpp>
#include <asterion/kernel/process/owner.hpp>
#include <asterion/protocol/task_client.hpp>
#include <fstream>
#include <iostream>
#include <thread>
int main(int argc, char **argv) {
  CLI::App app{
      "Asterion validated CSV import and immutable dataset publication"};
  app.set_version_flag("--version",
                       "asterion-data-pipeline " ASTERION_PRODUCT_VERSION);
  std::string input_file, output_directory, endpoint, host, service, task;
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
  bool inspect = false, calendar = false;
  app.add_flag("--settlement-calendar", calendar, "Import or inspect a settlement calendar publication");
  app.add_option("--input", input_file, "Typed CsvImport Protobuf file")
      ->check(CLI::ExistingFile);
  app.add_option("--directory", output_directory,
                 "Dedicated existing publication directory")
      ->check(CLI::ExistingDirectory);
  app.add_flag("--inspect", inspect,
               "Verify and summarize an existing publication");
  argv = app.ensure_utf8(argv);
  CLI11_PARSE(app, argc, argv);
  if (argc == 1) {
    std::cerr << "asterion-data-pipeline: supply --input and --directory, or "
                 "--inspect and --directory.\n";
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
    if (!task.empty() || !service.empty() || !endpoint.empty() ||
        !host.empty() || port || !tls.ca_file.empty() ||
        !tls.certificate_file.empty() || !tls.private_key_file.empty()) {
      if (!input_file.empty() || !output_directory.empty() || inspect)
        throw std::invalid_argument("choose task worker OR standalone mode");
      return asterion::protocol::run_task_worker(
          endpoint, host, port, tls, service, task,
          calendar ? asterion::research::v1::CALENDAR_IMPORT : asterion::research::v1::DATA_IMPORT,
          [calendar](const auto &input, auto stop, const auto &progress) {
            asterion::research::v1::TaskFinish result;
            if(calendar) {
              progress(0,input.calendar().contents().size());
              *result.mutable_calendar_publication()=asterion::data_pipeline::import_calendar_snapshot(input.calendar(),stop);
              progress(input.calendar().contents().size(),input.calendar().contents().size());
            } else *result.mutable_publication() =
                asterion::data_pipeline::import_snapshot(input.data(), stop,
                                                         progress);
            return result;
          });
    }
    if (output_directory.empty() ||
        (inspect ? !input_file.empty() : input_file.empty()))
      throw std::invalid_argument(
          "choose CSV import OR publication inspection with a directory");
    const std::filesystem::path directory(
        std::u8string(output_directory.begin(), output_directory.end()));
    if (inspect && calendar) {
      std::cout << asterion::data_pipeline::calendar_summary(asterion::data_pipeline::read_calendar(directory)).dump() << '\n';
      return 0;
    }
    if (inspect) {
      std::cout << asterion::data_pipeline::summary(
                       asterion::data_pipeline::read(directory))
                       .dump()
                << '\n';
      return 0;
    }
    const std::filesystem::path path(
        std::u8string(input_file.begin(), input_file.end()));
    if (std::filesystem::is_symlink(path) ||
        !std::filesystem::is_regular_file(path) ||
        std::filesystem::file_size(path) > 65536)
      throw std::invalid_argument("invalid CSV import specification");
    std::ifstream file(path, std::ios::binary);
    std::string raw(65537, '\0');
    file.read(raw.data(), static_cast<std::streamsize>(raw.size()));
    raw.resize(static_cast<std::size_t>(file.gcount()));
    asterion::data::v1::CsvImport input;
    if (file.bad() || !file.eof() || raw.size() > 65536 ||
        !input.ParseFromString(raw))
      throw std::invalid_argument("invalid CsvImport Protobuf");
    if (calendar) {
      const auto publication=asterion::data_pipeline::import_calendar_snapshot(asterion::data_pipeline::capture_calendar_csv(input));
      const bool created=asterion::data_pipeline::publish_calendar(publication,directory);
      auto summary=asterion::data_pipeline::calendar_summary(publication);summary["created"]=created;
      std::cout << summary.dump() << '\n';return 0;
    }
    const auto publication = asterion::data_pipeline::import_csv(input);
    const bool created =
        asterion::data_pipeline::publish(publication, directory);
    auto summary = asterion::data_pipeline::summary(publication);
    summary["created"] = created;
    std::cout << summary.dump() << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "Data publication failed: " << error.what() << '\n';
    return 1;
  }
}
