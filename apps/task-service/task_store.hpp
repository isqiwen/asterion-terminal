#pragma once
#include <asterion/foundation/time.hpp>
#include <asterion/protocol/research.hpp>
#include <filesystem>
#include <memory>
namespace asterion::tasks {
// Serialized application-level durable task state. Agent owns OS processes;
// attempt tokens fence late worker reports after retry or service restart.
class Store {
public:
  explicit Store(
      std::filesystem::path directory,
      std::shared_ptr<const Clock> clock = std::make_shared<SystemClock>());
  ~Store();
  research::v1::Task submit(const std::string &id,
                            const research::v1::BacktestInput &input);
  research::v1::Task submit(const std::string &id,
                            const research::v1::FactorInput &input);
  research::v1::Task submit(const std::string &id,
                            const data::v1::CsvSnapshot &input);
  void finish(const std::string &id, const std::string &token,
              const data::v1::DatasetPublication &result);
  data::v1::DatasetPublication publication(const std::string &id) const;
  research::v1::Task submit(const std::string &, const data::v1::CalendarCsvSnapshot &);
  void finish(const std::string &, const std::string &, const data::v1::CalendarPublication &);
  data::v1::CalendarPublication calendar_publication(const std::string &) const;
  research::v1::Task get(const std::string &id) const;
  research::v1::TaskList list() const;
  std::string claim(const std::string &id);
  void progress(const std::string &id, const std::string &token,
                unsigned completed);
  research::v1::Task cancel(const std::string &id);
  research::v1::Task retry(const std::string &id);
  void finish(const std::string &id, const std::string &token,
              const research::v1::BacktestResult &result);
  void finish(const std::string &id, const std::string &token,
              const research::v1::FactorResult &result);
  void fail(const std::string &id, const std::string &token,
            const std::string &error);
  void interrupt(const std::string &id, const std::string &token,
                 const std::string &error);
  void acknowledge_cancel(const std::string &id, const std::string &token);
  research::v1::BacktestResult result(const std::string &id) const;

  research::v1::FactorResult factor_result(const std::string &id) const;

private:
  research::v1::Task submit_task(research::v1::Task task);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace asterion::tasks
