#pragma once
#include <string>
#include <string_view>

namespace asterion::warehouse::acquisition {

// Read one named member of an exchange's yearly ZIP into bounded memory.
// No archive paths are ever written to the filesystem.
std::string archive_member(std::string_view zip, std::string_view name);

} // namespace asterion::warehouse::acquisition
