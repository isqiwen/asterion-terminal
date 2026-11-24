#pragma once
#include "store.hpp"
#include <asterion/warehouse.pb.h>
#include <memory>

namespace asterion::warehouse {

// Owned by Warehouse's bounded history executor. No provider connections or writer.
class Reader
{
public:
    // A version's partitions come in pages of `page`.
    explicit Reader(std::filesystem::path directory, int page = 512);

    ::warehouse::DatasetVersion version(const ::warehouse::GetDatasetVersion&) const;
    ::warehouse::DatasetFile file(const ::warehouse::ReadDatasetFile&) const;
    ::warehouse::DatasetSummary summary(const ::warehouse::DatasetRef&) const;
    ::warehouse::DatasetCoverage coverage(const ::warehouse::DatasetRef&) const;
    ::warehouse::ProductContracts product_coverage(const ::warehouse::ProductRef&) const;
    ::warehouse::DatasetVersions versions(const ::warehouse::ListDatasetVersions&) const;
    ::warehouse::DatasetChange change(const ::warehouse::CompareDatasetVersions&) const;
    ::warehouse::TableRows rows(const ::warehouse::GetTableRows&) const;

private:
    struct Cache;

    std::filesystem::path directory_;
    int page_;
    std::shared_ptr<Cache> cache_;
};

} // namespace asterion::warehouse
