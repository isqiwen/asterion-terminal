#pragma once
#include "store.hpp"

namespace asterion::history::warehouse {

// The sources of a table, first the one whose data counts: each later one only
// fills what those before it lack.
const std::vector<std::string>& sources_of(std::string_view table);

// The one dataset clients read, composed from the sources in the data directory
// in that order, table by table. Every partition comes whole from one source.
// Unchanged sources publish no new version.
Version compose_research(const std::filesystem::path& data, Store& research);

} // namespace asterion::history::warehouse
