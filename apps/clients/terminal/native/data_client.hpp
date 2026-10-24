#pragma once
#include "service_endpoint.hpp"
#include "service_io.hpp"
#include <asterion/protocol/data.hpp>
#include <asterion/data_service.pb.h>
#include <asterion/protocol/backtest.hpp>
#include <memory>
#include <chrono>

namespace asterion::terminal {

class Application;

class DataClient
{
public:
    [[nodiscard]] static std::future<std::shared_ptr<DataClient>> open(ServiceIo&, ServiceEndpoint);
    ~DataClient();
    DataClient(const DataClient&) = delete;
    DataClient& operator=(const DataClient&) = delete;
    [[nodiscard]] std::future<Json> status() const;
    [[nodiscard]] std::future<Json> resources(const resource::proto::CatalogueQuery&);
    [[nodiscard]] std::future<std::string> authorize_download(
        const data::proto::DownloadAuthorizationRequest&);
    [[nodiscard]] std::future<void> configure_download_budget(
        const data::proto::DownloadBudgetConfiguration&);
    [[nodiscard]] std::future<data::proto::AcquisitionTaskResponse> acquisition_task(
        data::proto::AcquisitionTaskRequest);
    [[nodiscard]] std::future<void> submit_download(const std::string&, const std::string&);
    [[nodiscard]] std::future<Json> material_sources();
    [[nodiscard]] std::future<Json> submit_material(const data::proto::MaterialRequest&);
    [[nodiscard]] std::future<Json> material_read(const std::string&);
    [[nodiscard]] std::future<void> page(unsigned before);
    [[nodiscard]] std::future<Json> retirement(const std::string&, bool execute);
    [[nodiscard]] std::future<Json> result(const std::string&);
    [[nodiscard]] std::future<Json> metadata(const std::string&);
    [[nodiscard]] std::future<void> edit_metadata(const task::proto::TaskMetadataEdit&);
    [[nodiscard]] std::future<void> action(const std::string&, const std::string&);
    ServiceEndpoint endpoint() const;
    [[nodiscard]] std::future<data::proto::HistorySource> source(const std::string& id) const;
    [[nodiscard]] std::future<std::vector<data::proto::HistorySource>> provider_sources(
        const std::string& plugin_id) const;
    [[nodiscard]] std::future<data::proto::HistoryConnectionVerification>
    verify_connection(const std::string& source, const std::string& credential);
    [[nodiscard]] std::future<data::proto::BarDataset> bar_dataset(
        const data::proto::BarDatasetRequest&);
    [[nodiscard]] std::future<data::proto::DominantSeriesPreview> dominant_series(
        const std::vector<data::proto::BarDatasetRequest>&);
    // A product's own months in the daily table, as its dominant series.
    [[nodiscard]] std::future<data::proto::DominantSeriesPreview> dominant_series(
        const data::proto::ProductBarsRequest&);
    [[nodiscard]] std::future<data::proto::HistoryUpdatePlan> history_update_plan(
        const data::proto::HistoryUpdateQuery&);
    [[nodiscard]] std::future<Json> datasets(const data::proto::HistoryFilter&);
    [[nodiscard]] std::future<Json> coverage(const data::proto::HistoryCoverageQuery&);
    [[nodiscard]] std::future<std::vector<HistoryListing>> table_contracts(
        const data::proto::TableContractsQuery&);
    [[nodiscard]] std::future<Json> minute_page(const data::proto::MinutePageQuery&);
    [[nodiscard]] std::future<Json> minute_range(const data::proto::MinuteRangeQuery&);
    [[nodiscard]] std::future<Json> minute_evidence(const data::proto::MinuteEvidenceQuery&);
    [[nodiscard]] std::future<Json> minute_retirement(const std::string&, bool execute);
    [[nodiscard]] std::future<Json>
    table_retirement(const std::string&, bool execute, const std::string& preview_digest = {});
    [[nodiscard]] std::future<Json> table_retirements(const std::string& after);
    [[nodiscard]] std::future<Json> minute_retirements(const std::string& after);
    [[nodiscard]] std::future<Json> history_usage(const std::string&);
    [[nodiscard]] std::future<Json> observation(const data::proto::ObservationQuery&);
    // Market-wide tables: what the store holds, and one page of a partition.
    [[nodiscard]] std::future<data::proto::TableRefreshPlan> table_refresh_plan(
        const data::proto::TableRefreshQuery&);
    [[nodiscard]] std::future<Json> historical_margins(const data::proto::HistoricalMarginQuery&);
    [[nodiscard]] std::future<Json> table_coverage();
    [[nodiscard]] std::future<Json> table_page(const data::proto::TablePageQuery&);
    [[nodiscard]] std::future<Json> table_products(const data::proto::TableProductsQuery&);
    [[nodiscard]] std::future<Json> table_listed_contracts(
        const data::proto::TableListedContractsQuery&);
    [[nodiscard]] std::future<Json> table_daily(const data::proto::TableDailyQuery&);
    [[nodiscard]] static std::future<Json> inspect_history_usage(
        ServiceIo&,
        const ServiceEndpoint&,
        const std::string&,
        std::chrono::steady_clock::time_point deadline);
    [[nodiscard]] std::future<Json> saved_datasets();
    [[nodiscard]] std::future<data::proto::NamedDataset> saved_dataset(const std::string&);
    [[nodiscard]] std::future<void> save_dataset(const data::proto::NamedDataset&);

private:
    friend class Application;
    // Application collects selected service views in one I/O owner turn.
    Json owner_status() const;
    DataClient(ServiceIo&, ServiceEndpoint);
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace asterion::terminal
