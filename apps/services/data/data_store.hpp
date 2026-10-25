#pragma once
#include "bar_dataset_source.hpp"
#include "history_archive.hpp"
#include "table_update.hpp"
#include <asterion/kernel/process/file_lock.hpp>
#include <asterion/data_service.pb.h>
#include <optional>
#include <variant>

namespace asterion::data {

// One persistent warehouse, independent of task history and process lifetimes.
// The I/O owner orders each object through preparation and durable completion.
// File preparation returns sealed evidence; the Data writer confirms acquisition and publication.
class Store
{
public:
    Store(std::filesystem::path root, std::string instance);
    proto::DownloadBudgetPolicy verify_budget(const proto::DownloadBudgetConfiguration&) const;
    proto::StoredDownloadAuthorization download_authorization(const proto::DownloadIdentity&) const;

    const std::string& instance() const noexcept
    {
        return instance_;
    }

    const history_files::Archive& archive() const noexcept
    {
        return archive_;
    }

    void begin_minute_retirement(const proto::HistoryRecord&, const std::string& manifest);
    void clean_minute_retirement(const std::string&, std::stop_token);
    void finish_minute_retirement(const std::string&);

    proto::HistoryUsage history_usage(const std::string& dataset) const;
    proto::HistoryUsage snapshot_usage(const std::string&) const;
    resource::proto::Catalogue resources(
        const resource::proto::Owner&,
        const resource::proto::CatalogueQuery&,
        std::stop_token) const;
    void begin_table_retirement(const history_files::TableStore::Retirement&);
    void clean_table_retirement(const std::string&, std::stop_token);
    void finish_table_retirement(const std::string&);
    BarDatasetSources sources(const proto::BarDatasetRequest&) const;

    class VerifiedAuthorization
    {
        friend class Store;

        explicit VerifiedAuthorization(proto::StoredDownloadAuthorization value)
            : value_(std::move(value))
        {}

        proto::StoredDownloadAuthorization value_;
    };

    VerifiedAuthorization verify_authorization(
        const proto::DownloadAuthorizationRequest&,
        std::stop_token stop = {}) const;
    proto::DownloadAuthorization authorize(const VerifiedAuthorization&);
    proto::DownloadAuthorization authorization(const std::string& id) const;

    class VerifiedDataset
    {
        friend class Store;

        VerifiedDataset(proto::NamedDataset value, bool creates_entry)
            : value_(std::move(value)),
              creates_entry_(creates_entry)
        {}

        proto::NamedDataset value_;
        bool creates_entry_;

    public:
        bool creates_entry() const noexcept
        {
            return creates_entry_;
        }
    };

    VerifiedDataset verify_dataset(const proto::NamedDataset&) const;
    void save(const VerifiedDataset&);
    proto::DownloadCredentials credentials(const proto::DownloadIdentity&) const;

    class VerifiedAllocation
    {
        friend class Store;

        // A table update writes no files of its own: it has no contract and no directory.
        VerifiedAllocation(
            proto::DownloadAllocation value,
            std::optional<HistoryIdentity> contract,
            std::string source,
            unsigned interval,
            std::optional<std::string> directory)
            : value_(std::move(value)),
              contract_(std::move(contract)),
              source_(std::move(source)),
              interval_(interval),
              directory_(std::move(directory))
        {}

        proto::DownloadAllocation value_;
        std::optional<HistoryIdentity> contract_;
        std::string source_;
        unsigned interval_;
        std::optional<std::string> directory_;
    };

    VerifiedAllocation verify_allocation(const proto::DownloadAllocation&) const;
    proto::DownloadDirectory allocate(const VerifiedAllocation&);

    class VerifiedDownload
    {
        friend class Store;

        explicit VerifiedDownload(proto::DownloadPreparation value)
            : value_(std::move(value))
        {}

        proto::DownloadPreparation value_;
    };

    VerifiedDownload verify_download(const proto::DownloadPreparation&) const;
    proto::PreparedDownload prepare(const VerifiedDownload&);

    class VerifiedPublication
    {
        friend class Store;

        VerifiedPublication(proto::DownloadPublication decision, proto::HistoryRecord record)
            : decision_(std::move(decision)),
              record_(std::move(record))
        {}

        proto::DownloadPublication decision_;
        proto::HistoryRecord record_;

    public:
        const std::string& dataset_id() const
        {
            return record_.minute_result().manifest_sha256();
        }
    };

    VerifiedPublication verify_publication(const proto::DownloadPublication&) const;
    proto::PublishedDownload publish(const VerifiedPublication&);
    proto::PublishedDownload published(const proto::DownloadIdentity&) const;

    // Market-wide tables. A partition is published when it has been validated and
    // recorded; the task that delivered it only drives the reading.
    const history_files::TableStore& tables() const noexcept
    {
        return tables_;
    }

    std::vector<TableOffer> table_offers() const;
    proto::TableUpdatePlan table_plan(const proto::TablePlanQuery&, const std::string& today) const;

    class VerifiedPartition
    {
        friend class Store;
        TablePartition partition_;
        history_files::TableSeries series_;
        history_files::TableStore::Object object_;
        std::string provider_artifact_;
        std::optional<SourceResponse> response_;
        std::optional<history_files::TableObservationContext> observation_context_;
        std::optional<history_files::TableRefreshObservation> previous_observation_;
        std::string expected_snapshot_;
        bool missing_only_day_ = false;
    };

    // Types the delivered rows and writes their file. The receipt is final when
    // the rows are rejected or the partition already holds them; otherwise the
    // sealed partition still has to be recorded.
    std::variant<proto::TablePartitionReceipt, VerifiedPartition> verify_partition(
        const proto::TablePartitionRows&) const;
    proto::TablePartitionReceipt record_partition(const VerifiedPartition&, std::int64_t now_ns);

private:
    std::filesystem::path root_;
    std::string instance_;
    FileLock ownership_;
    history_files::Archive archive_;
    history_files::TableStore tables_;
    // The update a table attempt was authorized for, and what its plugin provides.
    std::pair<proto::TableUpdate, std::vector<TableOffer>> table_attempt(
        const proto::DownloadIdentity&) const;
    proto::StoredDownloadAuthorization read_authorization(const std::string& id) const;
    std::filesystem::path authorization_path(const std::string& id) const;
    std::filesystem::path operation(const proto::DownloadIdentity&) const;
};

} // namespace asterion::data
