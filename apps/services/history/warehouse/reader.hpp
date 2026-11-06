#pragma once
#include "store.hpp"
#include <asterion/history.pb.h>

namespace asterion::history::warehouse {

// Owned by History's bounded history executor. No provider connections or writer.
class Reader
{
public:
    // A version's partitions come in pages of `page`.
    explicit Reader(std::filesystem::path directory, int page = 512)
        : directory_(std::move(directory)),
          page_(page)
    {}

    ::history::DatasetVersion version(const ::history::GetDatasetVersion&) const;
    ::history::DatasetFile file(const ::history::ReadDatasetFile&) const;
    ::history::DatasetSummary summary(const ::history::DatasetRef&) const;
    ::history::DatasetCoverage coverage(const ::history::DatasetRef&) const;
    ::history::DatasetVersions versions(const ::history::ListDatasetVersions&) const;
    ::history::DatasetChange change(const ::history::CompareDatasetVersions&) const;
    ::history::TableRows rows(const ::history::GetTableRows&) const;

private:
    std::filesystem::path directory_;
    int page_;
};

} // namespace asterion::history::warehouse
