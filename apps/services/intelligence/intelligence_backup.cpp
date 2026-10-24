#include "intelligence_backup.hpp"
#include "notice_store.hpp"
#include "program_files.hpp"
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/foundation/error.hpp>
#include <set>
#include <algorithm>

namespace asterion::intelligence {
namespace fs = std::filesystem;

ColdBackup
cold_backup(const fs::path& directory, const resource::proto::Owner& owner, std::stop_token stop)
{
    ColdBackup result;
    auto& snapshot = result.snapshot;
    snapshot.owner = owner;
    snapshot.directory = directory;
    std::set<std::string> required_programs, dependencies;
    const auto depend = [&](const resource::proto::Object& object) {
        if (dependencies.insert(object.SerializeAsString()).second)
            snapshot.dependencies.push_back({object, false});
    };
    const auto programme = [&](const std::string& digest) {
        required_programs.insert(digest);
        resource::proto::Object object;
        *object.mutable_owner() = owner;
        object.set_kind("intelligence_program");
        object.set_id(digest);
        object.set_version(digest);
        depend(object);
    };
    snapshot.ownership = Store::inspect_backup(
        directory,
        owner,
        [&](const auto& record) {
            result.objects.push_back(record.reference());
            depend(record.input().material().reference());
            depend(record.input().material().evidence().content());
            if (!record.input().dictionary_reference().version().empty()) {
                depend(record.input().dictionary_reference());
                data::proto::DataRetention retention;
                const auto& source = record.input().dictionary_reference().owner();
                retention.mutable_source()->set_node_id(source.node_id());
                retention.mutable_source()->set_service_id(source.service_id());
                *retention.mutable_consumer() = record.reference();
                auto& version = *retention.add_versions();
                version.set_kind(data::proto::DataVersion::TABLE_SNAPSHOT);
                version.set_version(record.input().dictionary_reference().version());
                result.references.push_back(
                    {retention.source(),
                        record.reference(),
                        sha256_bytes(retention.SerializeAsString()),
                        record.state() == proto::PREPARING || record.state() == proto::ABANDONED});
            }
            programme(record.input().program_artifact());
            if (record.has_verification())
                programme(record.verification().program_artifact());
            if (record.has_intent())
                result.objects.push_back(record.intent().reference());
            if (record.has_published()) {
                result.objects.push_back(record.published().reference());
                depend(record.published().publication().storage().content());
                if (record.event().initial_publication().SerializeAsString()
                    == record.published().reference().SerializeAsString())
                    result.objects.push_back(record.event().reference());
                else
                    depend(record.event().reference());
            }
        },
        stop,
        [&](const proto::NoticeConsumerRecord& record) {
            const auto& binding = record.binding();
            const bool released = record.phase() == proto::NoticeConsumerRecord::RELEASED;
            result.notice_references.push_back(
                {binding.source(),
                    binding.consumer(),
                    sha256_bytes(binding.SerializeAsString()),
                    released});
            if (!released)
                depend(binding.consumer());
        },
        [&](const proto::NoticeChangeRecord& record) {
            const auto& input = record.intent().payload().input();
            const auto& request = input.request();
            result.objects.push_back(record.intent().reference());
            depend(request.event());
            depend(request.expected_head());
            depend(record.intent().payload().affected_publication());
            depend(input.material().reference());
            depend(input.material().evidence().content());
            programme(input.program_artifact());
            if (request.has_replacement())
                depend(request.replacement());
            if (record.has_published()) {
                result.objects.push_back(record.published().reference());
                depend(record.published().storage().content());
            }
        },
        [&](const proto::NoticeSetRecord& record) {
            const auto& payload = record.intent().payload();
            result.objects.push_back(record.intent().reference());
            programme(payload.request().program_artifact());
            for (const auto& member : payload.members()) {
                depend(member.event().event().reference());
                depend(member.event().head());
                depend(member.publication().reference());
                depend(member.publication().publication().storage().content());
                if (member.event().has_latest_change())
                    depend(member.event().latest_change().published().storage().content());
                for (const auto& restriction : member.limitations())
                    depend(restriction.source());
            }
            for (const auto& excluded : payload.exclusions()) {
                depend(excluded.event());
                for (const auto& reason : excluded.reasons())
                    depend(reason.source());
            }
            if (record.has_published()) {
                result.objects.push_back(record.published().reference());
                depend(record.published().storage().content());
            }
        },
        [&](const resource::proto::Object& object) {
            result.objects.push_back(object);
        });
    const auto add = [&](const fs::path& relative, const std::string& format) {
        const auto path = directory / relative;
        if (fs::is_symlink(path) || !fs::is_regular_file(path)
            || (format == "coordination" && fs::file_size(path)))
            throw std::invalid_argument("Intelligence cold evidence missing or invalid");
        snapshot.files.push_back({path, relative, format});
    };
    add("owner.pb", "protobuf");
    add("ledger.sqlite", "sqlite");
    add("intelligence.lock", "coordination");
    programs::Files programmes(directory / "programs", owner);
    std::set<std::string> preserved;
    std::string cursor;
    for (;;) {
        const auto page = programmes.catalogue(cursor, 200);
        if (page.empty())
            break;
        for (const auto& entry : page) {
            (void)programmes.verify(entry.digest, stop);
            preserved.insert(entry.digest);
            add(fs::path("programs") / entry.digest, "binary");
            resource::proto::Object object;
            *object.mutable_owner() = owner;
            object.set_kind("intelligence_program");
            object.set_id(entry.digest);
            object.set_version(entry.digest);
            result.objects.push_back(std::move(object));
        }
        cursor = page.back().digest;
    }
    for (const auto& digest : required_programs)
        if (!preserved.contains(digest))
            throw std::invalid_argument("Intelligence original verification program missing");
    if (fs::exists(directory / "programs")) {
        add("programs/owner.pb", "protobuf");
        snapshot.directories.push_back("programs");
        snapshot.directories.push_back("programs/staging");
    }
    std::set<fs::path> declared;
    for (const auto& file : snapshot.files)
        declared.insert(file.relative);
    for (const auto& entry : fs::recursive_directory_iterator(directory)) {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "Intelligence cold backup interrupted");
        if (entry.is_symlink())
            throw std::invalid_argument("Intelligence backup rejects symbolic links");
        if (!entry.is_regular_file())
            continue;
        const auto relative = entry.path().lexically_relative(directory);
        const auto name = relative.filename().string();
        if (relative.parent_path() == "programs/staging"
            || (relative.parent_path() == "programs" && name.starts_with("owner.pb.tmp.")))
            continue;
        if (name == "ledger.sqlite-shm")
            continue;
        if ((name == "ledger.sqlite-wal" || name == "ledger.sqlite-journal")
            && !fs::file_size(entry.path()))
            continue;
        if (!declared.contains(relative))
            throw std::invalid_argument(
                "undeclared Intelligence evidence; owner inspection required");
    }
    snapshot.exclusions = {"unpublished_program_preparations", "runtime_process_state"};
    return result;
}

} // namespace asterion::intelligence
