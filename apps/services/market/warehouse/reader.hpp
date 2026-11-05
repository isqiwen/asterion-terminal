#pragma once
#include "store.hpp"
#include <asterion/market.pb.h>

namespace asterion::market_data::warehouse {

// Owned by Market's bounded history executor. No provider connections or writer.
class Reader
{
public:
    // A version's partitions come in pages of `page`.
    explicit Reader(std::filesystem::path directory, int page = 512)
        : directory_(std::move(directory)),
          page_(page)
    {}

    ::market::Datasets datasets() const;
    ::market::DatasetVersion version(const ::market::GetDatasetVersion&) const;
    ::market::DatasetFile file(const ::market::ReadDatasetFile&) const;
    ::market::DatasetSummary summary(const ::market::DatasetRef&) const;
    ::market::DatasetCoverage coverage(const ::market::DatasetRef&) const;
    ::market::DatasetVersions versions(const ::market::ListDatasetVersions&) const;
    ::market::DatasetChange change(const ::market::CompareDatasetVersions&) const;

private:
    std::filesystem::path directory_;
    int page_;
};

} // namespace asterion::market_data::warehouse
