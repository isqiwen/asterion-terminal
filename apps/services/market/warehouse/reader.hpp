#pragma once
#include "store.hpp"
#include <asterion/dataset.pb.h>

namespace asterion::market_data::warehouse {

// Owned by Market's bounded history executor. No provider connections or writer.
class Reader
{
public:
    explicit Reader(std::filesystem::path directory)
        : directory_(std::move(directory))
    {}

    dataset::proto::Sources sources(const dataset::proto::SourcesQuery&) const;
    dataset::proto::Version version(const dataset::proto::VersionQuery&) const;
    dataset::proto::File file(const dataset::proto::FileQuery&) const;
    dataset::proto::Summary summary(const dataset::proto::SummaryQuery&) const;
    dataset::proto::Coverage coverage(const dataset::proto::CoverageQuery&) const;

private:
    std::filesystem::path directory_;
};

} // namespace asterion::market_data::warehouse
