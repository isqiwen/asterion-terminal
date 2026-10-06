#include "artifact_uploads.hpp"
#include "managed_paths.hpp"
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <fstream>
#include <stdexcept>
namespace asterion::agent {
namespace fs = std::filesystem;
ArtifactUploads::ArtifactUploads(fs::path root, BlockingWork blocking)
    : root_(std::move(root)), blocking_(std::move(blocking)) {}
PolledTask<void> ArtifactUploads::begin(const node::v1::Upload& u) {
  validate_artifact_digest(u.sha256());
  const auto platform = current_platform();
  if (u.os() != platform.os || u.arch() != platform.arch || !u.size() ||
      u.size() > max_artifact_bytes)
    throw std::invalid_argument("artifact platform or size mismatch");
  const auto path = root_ / "uploads" / u.sha256();
  co_await blocking_([&] {
    require_managed_path(path);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
      throw std::runtime_error("cannot create upload");
  });
  uploads_[u.sha256()] = {u.size(), 0};
}
PolledTask<void> ArtifactUploads::append(const node::v1::Chunk& c) {
  validate_artifact_digest(c.sha256());
  auto& u = uploads_.at(c.sha256());
  if (c.offset() != u.offset || c.data().empty() || c.data().size() > 1024 * 1024 ||
      c.data().size() > u.size - u.offset)
    throw std::invalid_argument("invalid upload chunk");
  const auto path = root_ / "uploads" / c.sha256();
  co_await blocking_([&] {
    require_managed_path(path);
    std::ofstream out(path, std::ios::binary | std::ios::app);
    out.write(c.data().data(), static_cast<std::streamsize>(c.data().size()));
    out.flush();
    if (!out)
      throw std::runtime_error("upload write failed");
  });
  u.offset += c.data().size();
}
PolledTask<void> ArtifactUploads::finish(const node::v1::Finish& finished) {
  const auto hash = finished.sha256();
  validate_artifact_digest(hash);
  const auto u = uploads_.at(hash);
  const auto path = root_ / "uploads" / hash;
  const auto target = root_ / "artifacts" / (hash + ".bin");
  co_await blocking_([&] {
    require_managed_path(path);
    require_managed_path(target);
    // A failed directory sync may leave the renamed target in place.
    // Revalidate and durably acknowledge it on an explicit finish retry.
    const auto source = fs::exists(path) ? path : target;
    if (u.offset != u.size || sha256_file(source) != hash)
      throw std::invalid_argument("artifact size or checksum mismatch");
    const auto actual = artifact_platform(source);
    const auto platform = current_platform();
    if (actual.os != platform.os || actual.arch != platform.arch)
      throw std::invalid_argument("uploaded executable platform mismatch");
    if (fs::exists(target)) {
      if (sha256_file(target) != hash)
        throw std::runtime_error("existing artifact corrupted");
      sync_directory(target.parent_path());
      if (fs::exists(path))
        fs::remove(path);
    } else {
      fs::permissions(path, fs::perms::owner_all);
      publish_file_durably(path, target);
    }
    sync_directory(path.parent_path());
  });
  uploads_.erase(hash);
}
} // namespace asterion::agent
