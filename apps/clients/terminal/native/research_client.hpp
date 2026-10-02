#pragma once
#include "service_endpoint.hpp"
#include <asterion/protocol/factor.hpp>
#include <asterion/protocol/data.hpp>
#include <asterion/protocol/research.hpp>
#include <memory>
#include <chrono>
namespace asterion::terminal {
class ResearchClient {
public:
  explicit ResearchClient(ServiceEndpoint endpoint);
  ~ResearchClient();
  Json status() const;
  // Task list read from the service now, not the last poll.
  Json tasks();
  data::v1::HistorySource source(const std::string& id) const;
  // The sources one plugin declares that take a credential.
  std::vector<data::v1::HistorySource> provider_sources(const std::string& plugin_id) const;
  data::v1::HistoryConnectionVerification verify_connection(const std::string& source,
                                                            const std::string& credential);
  void submit(const std::string& id, const research::v1::BacktestRequest& input);
  void submit(const std::string& id, const research::v1::FactorRequest& input);
  void submit(const std::string&, const research::v1::DailyFactorRequest&);
  // Bars resolved by the service from its completed downloads.
  data::v1::BarDataset bar_dataset(const data::v1::BarDatasetRequest&);
  void submit(const std::string&, const data::v1::MinuteDownload&, const std::string& token);
  void submit(const std::string&, const data::v1::DailyDownload&, const std::string& token);
  data::v1::HistoryUpdatePlan history_update_plan(const data::v1::HistoryUpdateQuery&);
  void submit_update(const std::string&, const data::v1::HistoryUpdateSubmit&,
                     const std::string& token);
  ServiceEndpoint endpoint() const;
  Json history_usage(const std::string&);
  // One-shot read only: no poller, retry, task listing or service mutation.
  static Json inspect_history_usage(const ServiceEndpoint&, const std::string&,
                                    std::chrono::steady_clock::time_point deadline);
  Json datasets(const data::v1::HistoryFilter&);
  Json saved_datasets();
  data::v1::ResearchDataset saved_dataset(const std::string&);
  void save_dataset(const data::v1::ResearchDataset&);
  // Per-contract trading-day coverage of the archive.
  Json coverage(const data::v1::HistoryFilter&);
  std::vector<HistoryListing> catalog(const std::string& source, const std::string& credential,
                                      const std::string& venue, const std::string& product);
  Json daily_page(const data::v1::DailyPageQuery&);
  void action(const std::string& id, const std::string& action);
  Json result(const std::string& id);
  Json minute_page(const data::v1::MinutePageQuery&);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace asterion::terminal
