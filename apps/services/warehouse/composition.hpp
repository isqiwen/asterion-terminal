#pragma once
#include "store.hpp"

namespace asterion::warehouse {

// The sources of a table, first the one whose data counts: each later one only
// fills what those before it lack.
const std::vector<std::string>& sources_of(std::string_view table);

using SourceVersions = std::map<std::string, std::string>;
SourceVersions source_versions(const std::filesystem::path& data);

// Fixed inputs keep later acquisition batches out of this publication.
Version compose(const std::filesystem::path& data, Store& unified, const SourceVersions& versions);

} // namespace asterion::warehouse
