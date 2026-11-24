#pragma once
#include "store.hpp"
#include <memory>

namespace asterion::warehouse {

// The sources of a table, first the one whose data counts: each later one only
// fills what those before it lack.
const std::vector<std::string>& sources_of(std::string_view table);

using SourceVersions = std::map<std::string, std::string>;
SourceVersions source_versions(const std::filesystem::path& data);

struct CachedSource
{
    std::unique_ptr<Store> store;
    VersionHead head;
    Version version;
};

using SourceCache = std::map<std::string, CachedSource>;

// Fixed inputs keep later acquisition batches out of this publication.
Version compose(const std::filesystem::path& data, Store& unified, const SourceVersions& versions);
Version compose(
    const std::filesystem::path& data,
    Store& unified,
    const SourceVersions& versions,
    SourceCache& cache);

// Publishes verified price-tick intervals into the `verified` source store
// when they differ from its current version, returning the current identity.
std::string
publish_verified_price_ticks(const std::filesystem::path& data, std::span<const PriceTick> ticks);

} // namespace asterion::warehouse
