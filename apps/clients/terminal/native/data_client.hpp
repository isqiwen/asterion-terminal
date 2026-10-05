#pragma once
#include "service_endpoint.hpp"
#include "service_io.hpp"
#include <asterion/protocol/data.hpp>
#include <asterion/v1/data_service.pb.h>
#include <asterion/protocol/backtest.hpp>
#include <memory>
#include <chrono>
namespace asterion::terminal {
class Application;
class DataClient {
public:
  [[nodiscard]] static std::future<std::shared_ptr<DataClient>> open(ServiceIo&, ServiceEndpoint);
  ~DataClient();
  DataClient(const DataClient&) = delete;
  DataClient& operator=(const DataClient&) = delete;
  [[nodiscard]] std::future<Json> status() const;
  [[nodiscard]] std::future<std::string>
  authorize_download(const data::v1::DownloadAuthorizationRequest&);
  [[nodiscard]] std::future<void>
  configure_download_budget(const data::v1::DownloadBudgetConfiguration&);
  ServiceEndpoint endpoint() const;
  [[nodiscard]] std::future<data::v1::HistorySource> source(const std::string& id) const;
  [[nodiscard]] std::future<std::vector<data::v1::HistorySource>>
  provider_sources(const std::string& plugin_id) const;
  [[nodiscard]] std::future<data::v1::HistoryConnectionVerification>
  verify_connection(const std::string& source, const std::string& credential);
  [[nodiscard]] std::future<data::v1::BarDataset> bar_dataset(const data::v1::BarDatasetRequest&);
  [[nodiscard]] std::future<backtest::v1::DominantSeriesPreview>
  dominant_series(const std::vector<data::v1::BarDatasetRequest>&);
  [[nodiscard]] std::future<data::v1::HistoryUpdatePlan>
  history_update_plan(const data::v1::HistoryUpdateQuery&);
  [[nodiscard]] std::future<Json> datasets(const data::v1::HistoryFilter&);
  [[nodiscard]] std::future<Json> coverage(const data::v1::HistoryFilter&);
  [[nodiscard]] std::future<std::vector<HistoryListing>> catalog(const std::string& source,
                                                                 const std::string& credential,
                                                                 const std::string& venue,
                                                                 const std::string& product);
  [[nodiscard]] std::future<Json> daily_page(const data::v1::DailyPageQuery&);
  [[nodiscard]] std::future<Json> minute_page(const data::v1::MinutePageQuery&);
  [[nodiscard]] std::future<Json> history_usage(const std::string&);
  [[nodiscard]] static std::future<Json>
  inspect_history_usage(ServiceIo&, const ServiceEndpoint&, const std::string&,
                        std::chrono::steady_clock::time_point deadline);
  [[nodiscard]] std::future<Json> saved_datasets();
  [[nodiscard]] std::future<data::v1::NamedDataset> saved_dataset(const std::string&);
  [[nodiscard]] std::future<void> save_dataset(const data::v1::NamedDataset&);

private:
  friend class Application;
  // Application collects selected service views in one I/O owner turn.
  Json owner_status() const;
  DataClient(ServiceIo&, ServiceEndpoint);
  struct Impl;
  std::shared_ptr<Impl> impl_;
};
} // namespace asterion::terminal
