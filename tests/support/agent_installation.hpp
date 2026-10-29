#pragma once
#include <filesystem>

namespace asterion::testing_support {

// The build's Agent placed in <root>/agent/bin through a link, as an
// installation runs it: it keeps its state in <root>/agent/data and installs
// the services under <root>. Returns the link to start.
inline std::filesystem::path install_agent(const std::filesystem::path& root)
{
    const auto link = root / "agent" / "bin" / "asterion_node_agent";
    std::filesystem::create_directories(link.parent_path());
    std::filesystem::create_directories(root / "agent" / "data");
    if (!std::filesystem::is_symlink(link))
        std::filesystem::create_symlink(ASTERION_AGENT_PATH, link);
    return link;
}

} // namespace asterion::testing_support
