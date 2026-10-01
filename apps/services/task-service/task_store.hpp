#pragma once
#include <asterion/foundation/time.hpp>
#include "bar_dataset_source.hpp"
#include <asterion/protocol/research.hpp>
#include <filesystem>
#include <memory>
namespace asterion::tasks {
// Serialized application-level durable task state. Agent owns OS processes;
// attempt tokens fence late worker reports after retry or service restart.
class Store {
public:
  explicit Store(std::filesystem::path directory,
                 std::shared_ptr<const Clock> clock = std::make_shared<SystemClock>());
  ~Store();
  // Owns an immutable attempt/input/result snapshot, never a reference to Store.
  // prepare/finish require host serialization; verify runs outside that lock.
  class Completion {
  public:
    void verify();

  private:
    friend class Store;
    Completion(research::v1::Task task, research::v1::TaskFinish result,
               std::filesystem::path directory);
    research::v1::Task task_;
    std::filesystem::path directory_;
    research::v1::TaskFinish result_;
    bool verified_ = false;
  };
  // Capture and commit under the host lock; verify reads source files outside it.
  class DailyFactorSubmission {
  public:
    void verify();

  private:
    friend class Store;
    DailyFactorSubmission(std::string, research::v1::DailyFactorRequest, research::v1::Task,
                          data::v1::DailyDownloadResult);
    std::string id_;
    research::v1::DailyFactorRequest request_;
    research::v1::Task source_;
    data::v1::DailyDownloadResult result_;
    research::v1::DailyFactorInput input_;
    bool verified_ = false;
  };
  DailyFactorSubmission prepare_daily_factor(const std::string&,
                                             const research::v1::DailyFactorRequest&);
  research::v1::Task submit(DailyFactorSubmission);
  research::v1::DailyFactorResult daily_factor_result(const std::string&) const;
  Completion prepare_finish(const research::v1::TaskFinish& result);
  void finish(Completion completion);
  research::v1::Task submit(const std::string& id, const research::v1::BacktestInput& input);
  research::v1::Task submit(const std::string& id, const research::v1::FactorInput& input);
  // Captures completed download sources under the host lock; resolve their
  // files outside it with resolve_bar_dataset, then confirm_sources under it.
  BarDatasetSources prepare_dataset(const data::v1::BarDatasetRequest&) const;
  void confirm_sources(const BarDatasetSources&) const;
  research::v1::Task submit(const std::string&, const data::v1::MinuteDownload&,
                            const std::string& provider_token);
  research::v1::Task submit(const std::string&, const data::v1::DailyDownload&,
                            const std::string& provider_token);
  data::v1::DailyDownloadResult daily_result(const std::string&) const;
  void download_attempt(research::v1::TaskAttempt&) const;
  data::v1::MinuteDownloadResult minute_result(const std::string&) const;
  research::v1::Task get(const std::string& id) const;
  research::v1::TaskList list() const;
  research::v1::TaskLaunches dispatch(const research::v1::TaskDispatch& processes) const;
  std::string claim(const std::string& id);
  void progress(const std::string& id, const std::string& token, unsigned completed);
  research::v1::Task cancel(const std::string& id);
  research::v1::Task retry(const std::string& id);
  void finish(const std::string& id, const std::string& token,
              const research::v1::BacktestResult& result);
  void finish(const std::string& id, const std::string& token,
              const research::v1::FactorResult& result);
  void fail(const std::string& id, const std::string& token, const std::string& error);
  void interrupt(const std::string& id, const std::string& token, const std::string& error);
  void acknowledge_cancel(const std::string& id, const std::string& token);
  research::v1::BacktestResult result(const std::string& id) const;

  research::v1::FactorResult factor_result(const std::string& id) const;

private:
  research::v1::Task submit_task(research::v1::Task task);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace asterion::tasks
