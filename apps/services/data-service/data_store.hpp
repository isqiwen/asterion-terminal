#pragma once
#include "bar_dataset_source.hpp"
#include "history_archive.hpp"
#include <asterion/kernel/process/file_lock.hpp>
#include <asterion/v1/data_service.pb.h>
#include <optional>

namespace asterion::data {
// One persistent warehouse, independent of task history and process lifetimes.
// The I/O owner orders each object through preparation and durable completion.
// File preparation returns sealed evidence; the writer only commits that evidence. Only the bound
// same-node task coordinator may publish downloads.
class Store {
public:
  Store(std::filesystem::path root, std::string instance, std::string task_instance);
  v1::DownloadBudgetPolicy verify_budget(const v1::DownloadBudgetConfiguration&) const;
  v1::StoredDownloadAuthorization download_authorization(const v1::DownloadIdentity&) const;
  const std::string& instance() const noexcept { return instance_; }
  const history_files::Archive& archive() const noexcept { return archive_; }
  v1::HistoryUsage history_usage(const std::string& dataset) const;
  BarDatasetSources sources(const v1::BarDatasetRequest&) const;
  class VerifiedAuthorization {
    friend class Store;
    explicit VerifiedAuthorization(v1::StoredDownloadAuthorization value)
        : value_(std::move(value)) {}
    v1::StoredDownloadAuthorization value_;
  };
  VerifiedAuthorization verify_authorization(const v1::DownloadAuthorizationRequest&) const;
  v1::DownloadAuthorization authorize(const VerifiedAuthorization&);
  v1::DownloadAuthorization authorization(const std::string& id) const;
  class VerifiedDataset {
    friend class Store;
    VerifiedDataset(v1::NamedDataset value, bool creates_entry)
        : value_(std::move(value)), creates_entry_(creates_entry) {}
    v1::NamedDataset value_;
    bool creates_entry_;

  public:
    bool creates_entry() const noexcept { return creates_entry_; }
  };
  VerifiedDataset verify_dataset(const v1::NamedDataset&) const;
  void save(const VerifiedDataset&);
  v1::DownloadCredentials credentials(const v1::DownloadIdentity&) const;
  class VerifiedAllocation {
    friend class Store;
    VerifiedAllocation(v1::DownloadAllocation value, HistoryIdentity contract, std::string source,
                       unsigned interval, std::optional<std::string> directory)
        : value_(std::move(value)), contract_(std::move(contract)), source_(std::move(source)),
          interval_(interval), directory_(std::move(directory)) {}
    v1::DownloadAllocation value_;
    HistoryIdentity contract_;
    std::string source_;
    unsigned interval_;
    std::optional<std::string> directory_;
  };
  VerifiedAllocation verify_allocation(const v1::DownloadAllocation&) const;
  v1::DownloadDirectory allocate(const VerifiedAllocation&);
  class VerifiedDownload {
    friend class Store;
    explicit VerifiedDownload(v1::DownloadPreparation value) : value_(std::move(value)) {}
    v1::DownloadPreparation value_;
  };
  VerifiedDownload verify_download(const v1::DownloadPreparation&) const;
  v1::PreparedDownload prepare(const VerifiedDownload&);
  class VerifiedPublication {
    friend class Store;
    VerifiedPublication(v1::DownloadPublication decision, v1::HistoryRecord record)
        : decision_(std::move(decision)), record_(std::move(record)) {}
    v1::DownloadPublication decision_;
    v1::HistoryRecord record_;

  public:
    const std::string& dataset_id() const {
      return record_.has_minutes() ? record_.minute_result().manifest_sha256()
                                   : record_.daily_result().manifest_sha256();
    }
  };
  VerifiedPublication verify_publication(const v1::DownloadPublication&) const;
  v1::PublishedDownload publish(const VerifiedPublication&);
  v1::PublishedDownload published(const v1::DownloadIdentity&) const;

private:
  std::filesystem::path root_;
  std::string instance_, task_instance_;
  FileLock ownership_;
  history_files::Archive archive_;
  v1::StoredDownloadAuthorization read_authorization(const std::string& id) const;
  std::filesystem::path authorization_path(const std::string& id) const;
  std::filesystem::path operation(const v1::DownloadIdentity&) const;
};
} // namespace asterion::data
