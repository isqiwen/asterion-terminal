#pragma once
#include "agent_work.hpp"
#include <asterion/node.pb.h>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>

namespace asterion::agent {

// Programs an administrator uploads in chunks. A finished upload is verified
// against its digest and platform, then published as an immutable artifact.
class ArtifactUploads
{
public:
    ArtifactUploads(std::filesystem::path root, BlockingWork blocking);
    PolledTask<void> begin(const node::proto::Upload& upload);
    PolledTask<void> append(const node::proto::Chunk& chunk);
    PolledTask<void> finish(const node::proto::Finish& finished);

private:
    struct Upload
    {
        std::uint64_t size, offset = 0;
    };

    std::filesystem::path root_;
    BlockingWork blocking_;
    std::map<std::string, Upload> uploads_;
};

} // namespace asterion::agent
