#include "pipeline.hpp"
#include "minutes.hpp"
#include "daily.hpp"
#include "tushare.hpp"
#include "calendar.hpp"
#include "calendar_generate.hpp"
#include <asterion/kernel/durable_file.hpp>
#include <CLI/CLI.hpp>
#include <asterion/kernel/process/owner.hpp>
#include <asterion/protocol/task_client.hpp>
#include <fstream>
#include <algorithm>
#include <iostream>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
int main(int argc, char** argv) {
  CLI::App app{"Asterion validated CSV import and immutable dataset publication"};
  app.set_version_flag("--version", "asterion-data-pipeline " ASTERION_PRODUCT_VERSION);
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
  bool inspect = false, calendar = false, minute_worker = false, daily_worker = false;
  app.add_flag("--daily-download", daily_worker, "Run a managed daily-data download");
  app.add_flag("--minute-download", minute_worker, "Run a managed minute-data download");
  app.add_flag("--settlement-calendar", calendar,
               "Import or inspect a settlement calendar publication");
  app.add_option("--input", input_file, "Typed CsvImport Protobuf file")->check(CLI::ExistingFile);
  app.add_option("--directory", output_directory, "Dedicated existing publication directory")
      ->check(CLI::ExistingDirectory);
  app.add_flag("--inspect", inspect, "Verify and summarize an existing publication");
  std::string sessions_file, venue, product, settlements_file, previous_day, generated_file;
  auto* generate = app.add_subcommand(
      "calendar-generate",
      "Render a settlement calendar CSV from trading days, settlement prices and a session "
      "template");
  generate->add_option("--sessions", sessions_file, "Session templates (futures-sessions.json)")
      ->required()
      ->check(CLI::ExistingFile);
  generate->add_option("--venue", venue)->required();
  generate->add_option("--product", product)->required();
  generate
      ->add_option("--settlements", settlements_file,
                   "CSV: trading_day,settlement_price,settlement_source")
      ->required()
      ->check(CLI::ExistingFile);
  generate->add_option("--previous-trading-day", previous_day,
                       "Trading day before the first row; decides its night session");
  generate->add_option("--output", generated_file, "New calendar CSV; must not exist")->required();
  std::string minute_code, minute_directory;
  unsigned minute_frequency = 1, minute_rate = 60;
  bool token_stdin = false, minute_inspect = false;
  auto* minutes = app.add_subcommand(
      "tushare-minutes",
      "Download or resume a dated futures OHLCV dataset; never converts bars into trades");
  minutes->add_option("--ts-code", minute_code);
  minutes->add_option("--minutes", minute_frequency)->check(CLI::IsMember({1, 5, 15, 30, 60}));
  minutes->add_option("--requests-per-minute", minute_rate)->check(CLI::Range(1, 500));
  minutes->add_option("--directory", minute_directory)->required()->check(CLI::ExistingDirectory);
  minutes->add_flag("--token-stdin", token_stdin,
                    "Read token from standard input, never from command-line arguments");
  minutes->add_flag("--inspect", minute_inspect,
                    "Verify local dataset without requesting credentials or network access");
  argv = app.ensure_utf8(argv);
  CLI11_PARSE(app, argc, argv);
  const auto path_of = [](const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
  };
  const auto read_small = [&](const std::string& text) {
    const auto path = path_of(text);
    if (std::filesystem::is_symlink(path) || !std::filesystem::is_regular_file(path) ||
        std::filesystem::file_size(path) > 1024 * 1024)
      throw std::invalid_argument("input must be a regular file of at most 1 MiB: " + text);
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), {});
  };
  if (*minutes) {
    try {
      if (!input_file.empty() || !output_directory.empty() || !endpoint.empty() || !host.empty() ||
          !task.empty() || !service.empty() || port || inspect || calendar || minute_worker ||
          daily_worker || owner_pid || !tls.ca_file.empty() || !tls.certificate_file.empty() ||
          !tls.private_key_file.empty())
        throw std::invalid_argument("choose minute download OR another pipeline mode");
      const auto directory = path_of(minute_directory);
      if (minute_inspect) {
        if (token_stdin || !minute_code.empty())
          throw std::invalid_argument("inspection accepts only a dataset directory");
        std::cout << asterion::data_pipeline::inspect_minutes(directory).dump() << '\n';
        return 0;
      }
      if (!token_stdin)
        throw std::invalid_argument("Tushare download requires --token-stdin");
      const auto selected = asterion::tushare::instrument(minute_code);
      char secret[258]{};
      std::cin.getline(secret, sizeof(secret));
      if (std::cin.fail())
        throw std::invalid_argument("invalid Tushare token input");
      asterion::tushare::Minutes provider(secret);
      const auto catalog = asterion::tushare::contracts(
          secret, selected.venue, minute_code.substr(0, minute_code.find_first_of("0123456789")));
      std::fill(std::begin(secret), std::end(secret), '\0');
      const auto found =
          std::ranges::find(catalog, minute_code, &asterion::tushare::FuturesListing::ts_code);
      if (found == catalog.end())
        throw std::invalid_argument("load and select a dated futures contract first");
      const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
      auto range = asterion::tushare::contract_range(*found, minute_frequency, now);
      if (std::filesystem::exists(directory / "minutes.json")) {
        const auto manifest = asterion::data_pipeline::inspect_minutes(directory);
        auto spec = manifest.at("request");
        spec["version"] = 1;
        spec["requests_per_minute"] = minute_rate;
        const auto previous =
            asterion::data_pipeline::minute_range(asterion::data_pipeline::minute_request(spec));
        if (previous.instrument != range.instrument ||
            previous.interval_minutes != range.interval_minutes ||
            previous.begin_ns != range.begin_ns || previous.end_ns > range.end_ns)
          throw std::invalid_argument("minute dataset does not match whole contract request");
        range = previous; // Resume the recorded cutoff; never extend an existing dataset silently.
      }
      provider.start();
      auto result = asterion::data_pipeline::download_minutes(
          provider, range, directory, minute_rate, {},
          [](unsigned completed, unsigned total, std::uint64_t rows) {
            std::cout << asterion::Json{{"event", "download.progress"},
                                        {"completed", completed},
                                        {"total", total},
                                        {"rows", rows}}
                             .dump()
                      << std::endl;
          });
      provider.stop();
      std::cout << result.dump() << '\n';
      return 0;
    } catch (const std::exception& error) {
      std::cerr << "Minute download failed: " << error.what() << '\n';
      return 1;
    }
  }
  if (*generate) {
    try {
      const auto output = path_of(generated_file);
      if (std::filesystem::exists(std::filesystem::symlink_status(output)))
        throw std::invalid_argument("output already exists: " + generated_file);
      const auto catalog = asterion::sessions::SessionCatalog::parse(
          asterion::Json::parse(read_small(sessions_file)));
      const auto csv = asterion::data_pipeline::generate_calendar_csv(
          catalog, venue, product, read_small(settlements_file),
          previous_day.empty() ? std::nullopt : std::optional<std::string>(previous_day));
      asterion::write_file_durably(output, csv, false);
      std::cout << asterion::Json{{"output", generated_file}, {"bytes", csv.size()}}.dump() << '\n';
      return 0;
    } catch (const std::exception& error) {
      std::cerr << "Calendar generation failed: " << error.what() << '\n';
      return 1;
    }
  }
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
    if (!task.empty() || !service.empty() || !endpoint.empty() || !host.empty() || port ||
        !tls.ca_file.empty() || !tls.certificate_file.empty() || !tls.private_key_file.empty()) {
      if (!input_file.empty() || !output_directory.empty() || inspect ||
          (static_cast<unsigned>(calendar) + minute_worker + daily_worker > 1))
        throw std::invalid_argument("choose task worker OR standalone mode");
      return asterion::protocol::run_task_worker(
          endpoint, host, port, tls, service, task,
          daily_worker    ? asterion::research::v1::DAILY_DOWNLOAD
          : minute_worker ? asterion::research::v1::MINUTE_DOWNLOAD
          : calendar      ? asterion::research::v1::CALENDAR_IMPORT
                          : asterion::research::v1::DATA_IMPORT,
          [calendar, minute_worker, daily_worker](const auto& attempt, auto stop,
                                                  const auto& progress) {
            const auto& input = attempt.task();
            asterion::research::v1::TaskFinish result;
            if (minute_worker || daily_worker) {
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
                  asterion::tushare::Daily provider(attempt.provider_token());
                  provider.start();
                  (void)asterion::data_pipeline::download_daily(
                      provider, asterion::data_pipeline::daily_range(input.daily()), directory,
                      input.daily().requests_per_minute(), cancellation.get_token(),
                      [&](unsigned value, unsigned, std::uint64_t) { completed.store(value); });
                  *result.mutable_daily() = asterion::data_pipeline::daily_result(directory);
                } else {
                  asterion::tushare::Minutes provider(attempt.provider_token());
                  provider.start();
                  (void)asterion::data_pipeline::download_minutes(
                      provider, asterion::data_pipeline::minute_range(input.minutes()), directory,
                      input.minutes().requests_per_minute(), cancellation.get_token(),
                      [&](unsigned value, unsigned, std::uint64_t) { completed.store(value); });
                  *result.mutable_minutes() = asterion::data_pipeline::minute_result(directory);
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
            if (calendar) {
              progress(0, input.calendar().contents().size());
              *result.mutable_calendar_publication() =
                  asterion::data_pipeline::import_calendar_snapshot(input.calendar(), stop);
              progress(input.calendar().contents().size(), input.calendar().contents().size());
            } else
              *result.mutable_publication() =
                  asterion::data_pipeline::import_snapshot(input.data(), stop, progress);
            return result;
          });
    }
    if (daily_worker)
      throw std::invalid_argument("daily download worker requires a managed task");
    if (minute_worker)
      throw std::invalid_argument("minute download worker requires a managed task");
    if (output_directory.empty() || (inspect ? !input_file.empty() : input_file.empty()))
      throw std::invalid_argument("choose CSV import OR publication inspection with a directory");
    const std::filesystem::path directory(
        std::u8string(output_directory.begin(), output_directory.end()));
    if (inspect && calendar) {
      std::cout << asterion::data_pipeline::calendar_summary(
                       asterion::data_pipeline::read_calendar(directory))
                       .dump()
                << '\n';
      return 0;
    }
    if (inspect) {
      std::cout << asterion::data_pipeline::summary(asterion::data_pipeline::read(directory)).dump()
                << '\n';
      return 0;
    }
    const std::filesystem::path path(std::u8string(input_file.begin(), input_file.end()));
    if (std::filesystem::is_symlink(path) || !std::filesystem::is_regular_file(path) ||
        std::filesystem::file_size(path) > 65536)
      throw std::invalid_argument("invalid CSV import specification");
    std::ifstream file(path, std::ios::binary);
    std::string raw(65537, '\0');
    file.read(raw.data(), static_cast<std::streamsize>(raw.size()));
    raw.resize(static_cast<std::size_t>(file.gcount()));
    asterion::data::v1::CsvImport input;
    if (file.bad() || !file.eof() || raw.size() > 65536 || !input.ParseFromString(raw))
      throw std::invalid_argument("invalid CsvImport Protobuf");
    if (calendar) {
      const auto publication = asterion::data_pipeline::import_calendar_snapshot(
          asterion::data_pipeline::capture_calendar_csv(input));
      const bool created = asterion::data_pipeline::publish_calendar(publication, directory);
      auto summary = asterion::data_pipeline::calendar_summary(publication);
      summary["created"] = created;
      std::cout << summary.dump() << '\n';
      return 0;
    }
    const auto publication = asterion::data_pipeline::import_csv(input);
    const bool created = asterion::data_pipeline::publish(publication, directory);
    auto summary = asterion::data_pipeline::summary(publication);
    summary["created"] = created;
    std::cout << summary.dump() << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Data publication failed: " << error.what() << '\n';
    return 1;
  }
}
