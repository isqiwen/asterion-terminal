#include "provider_programs.hpp"
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/foundation/error.hpp>
#include <fstream>

namespace asterion::provider_programs {

void preserve(
    const std::string& digest,
    const std::filesystem::path& directory,
    const std::function<std::filesystem::path()>& source_path,
    std::stop_token stop)
{
    namespace fs = std::filesystem;
    if (digest.size() != 64 || digest.find_first_not_of("0123456789abcdef") != std::string::npos
        || !directory.is_absolute())
        throw std::invalid_argument("invalid provider program identity");
    const auto cancelled = [&] {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "provider program retention interrupted");
    };
    cancelled();
    create_directories_durably(directory);
    const auto suffix = current_platform().os == "macos" ? ".dylib" : ".so";
    const auto target = directory / (digest + suffix);
    if (fs::exists(target) || fs::is_symlink(target)) {
        if (fs::is_symlink(target) || !fs::is_regular_file(target)
            || fs::file_size(target) > max_artifact_bytes || sha256_file(target) != digest)
            throw std::invalid_argument("retained provider program is corrupt");
        sync_file_durably(target);
        return;
    }
    const auto source = source_path();
    if (fs::is_symlink(source) || !fs::is_regular_file(source)
        || fs::file_size(source) > max_artifact_bytes)
        throw std::invalid_argument("selected provider program cannot be retained");
    std::ifstream input(source, std::ios::binary);
    std::string bytes;
    char buffer[256 * 1024];
    while (input) {
        cancelled();
        input.read(buffer, sizeof(buffer));
        bytes.append(buffer, static_cast<std::size_t>(input.gcount()));
        if (bytes.size() > max_artifact_bytes)
            throw std::invalid_argument("selected provider program exceeds capacity");
    }
    if (input.bad() || sha256_bytes(bytes) != digest)
        throw std::invalid_argument("selected provider program changed before retention");
    const auto staging = directory / "staging";
    create_directories_durably(staging);
    const auto temporary = staging / unique_process_id();
    try {
        replace_file_durably(temporary, bytes);
        cancelled();
        publish_file_durably(temporary, target);
    } catch (...) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
}

} // namespace asterion::provider_programs
