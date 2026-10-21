#include "node_program.hpp"
#include <asterion/kernel/durable_file.hpp>
#include <asterion/foundation/serialization.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/file_lock.hpp>
#include <fstream>
#include <stdexcept>

namespace asterion::terminal {
namespace fs = std::filesystem;

namespace {

std::string text(const fs::path& p)
{
    const auto value = p.u8string();
    return {value.begin(), value.end()};
}

void plain(const fs::path& p)
{
    if (fs::is_symlink(p))
        throw std::runtime_error("Agent update path cannot be a symlink");
}

} // namespace

void install_node_program(const fs::path& source, const fs::path& installed, const fs::path& root)
{
    if (!source.is_absolute() || !installed.is_absolute() || !root.is_absolute()
        || installed.parent_path() != root / "bin" || source == installed)
        throw std::invalid_argument("invalid Agent installation paths");
    for (const auto& path : {root, root / "bin", source, installed})
        plain(path);
    if (!fs::is_regular_file(source))
        throw std::runtime_error("bundled Agent program is missing");
    const auto platform = artifact_platform(source), host = current_platform();
    if (platform.os != host.os || platform.arch != host.arch)
        throw std::runtime_error("bundled Agent platform mismatch");
    const auto target = sha256_file(source);
    create_directories_durably(root / "bin");
    if (fs::exists(installed)) {
        if (!fs::is_regular_file(installed))
            throw std::runtime_error("installed Agent is not a regular file");
        if (sha256_file(installed) != target)
            throw std::runtime_error(
                "local Agent version differs; upgrade the system service explicitly");
        // The matching executable can be the result of a failed directory sync.
        // The directory creation check above finishes publication before registration.
        return;
    }
    const auto temporary = root / "bin" / ("install-" + unique_process_id());
    if (!fs::create_directory(temporary))
        throw std::runtime_error("cannot create Agent staging directory");

    struct Cleanup
    {
        fs::path path;

        ~Cleanup()
        {
            std::error_code error;
            fs::remove_all(path, error);
        }
    } cleanup{temporary};

    const auto staged = temporary / installed.filename();
    fs::copy_file(source, staged);
    if (sha256_file(staged) != target)
        throw std::runtime_error("Agent source changed during staging");
    publish_file_durably(staged, installed);
    fs::remove(temporary);
    sync_directory(installed.parent_path());
}

void replace_node_program(
    const fs::path& source,
    const fs::path& installed,
    const fs::path& root,
    const std::string& expected_digest)
{
    if (!source.is_absolute() || !installed.is_absolute() || !root.is_absolute()
        || installed.parent_path() != root / "bin" || source == installed
        || expected_digest.size() != 64
        || expected_digest.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::invalid_argument("invalid Agent update identity");
    for (const auto& path : {source, installed, root, root / "bin"})
        plain(path);
    if (!fs::is_regular_file(source) || !fs::is_regular_file(installed))
        throw std::runtime_error("Agent programs must be regular files");
    FileLock stopped(root, "agent.lock");
    const auto platform = artifact_platform(source), host = current_platform();
    if (platform.os != host.os || platform.arch != host.arch)
        throw std::invalid_argument("Agent update platform mismatch");
    const auto target = sha256_file(source);
    const auto observed = sha256_file(installed);
    const auto journal = root / "agent-upgrade.json";
    const auto pending = root / "agent-upgrade.pending";
    const auto candidate = root / "bin" / "agent-upgrade.staged";
    for (const auto& path : {journal, pending, candidate})
        plain(path);
    if (fs::exists(pending))
        throw std::runtime_error("unfinished Agent update record requires inspection");
    const Json transaction =
        {{"installed", text(installed)}, {"before", expected_digest}, {"after", target}};
    if (fs::exists(journal)) {
        if (fs::file_size(journal) > 4096)
            throw std::runtime_error("Agent update record is too large");
        std::ifstream in(journal, std::ios::binary);
        std::string bytes{std::istreambuf_iterator<char>(in), {}};
        const auto saved = parse_json(bytes);
        require_fields(saved, {"installed", "before", "after"});
        if (saved != transaction)
            throw std::runtime_error("Agent update transaction differs");
    } else {
        if (observed != expected_digest)
            throw std::runtime_error("installed Agent changed; inspect before updating");
        if (fs::exists(candidate))
            throw std::runtime_error("unrecorded staged Agent requires inspection");
        if (target == expected_digest)
            return;
        write_file_durably(pending, transaction.dump());
        publish_file_durably(pending, journal);
    }
    if (observed != expected_digest && observed != target)
        throw std::runtime_error("Agent program does not match recorded update");
    if (observed == expected_digest) {
        if (fs::exists(candidate)) {
            if (!fs::is_regular_file(candidate) || sha256_file(candidate) != target)
                throw std::runtime_error("staged Agent differs; inspection required");
        } else {
            fs::copy_file(source, candidate);
            if (sha256_file(candidate) != target)
                throw std::runtime_error("Agent source changed during staging");
        }
        const auto staged = artifact_platform(candidate);
        if (staged.os != host.os || staged.arch != host.arch)
            throw std::runtime_error("staged Agent platform mismatch");
        publish_file_durably(candidate, installed);
    } else if (fs::exists(candidate)) {
        throw std::runtime_error("unexpected staged Agent after publication");
    }
    // A failed rename-directory sync leaves the new executable visible. Retry
    // that barrier even when recovery did not need another rename.
    sync_directory(installed.parent_path());
    // Only transaction metadata is removed; all identity, service and ledger
    // data remain intact. Restart/health confirmation is the caller's next step.
    fs::remove(journal);
    sync_directory(root);
}

} // namespace asterion::terminal
