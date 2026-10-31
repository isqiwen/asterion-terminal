#include "backup_files.hpp"
#include <asterion/system/durable_file.hpp>
#include <asterion/system/process/artifact.hpp>
#include <asterion/system/process/child.hpp>
#include <gtest/gtest.h>
#include <fstream>

using namespace asterion;
namespace fs = std::filesystem;

namespace {

struct Folder
{
    fs::path root = fs::temp_directory_path() / ("asterion-backup-" + unique_process_id());

    Folder()
    {
        fs::create_directory(root);
        root = fs::canonical(root);
    }

    ~Folder()
    {
        std::error_code error;
        fs::remove_all(root, error);
    }
};

backup_files::Snapshot snapshot(const fs::path& source)
{
    backup_files::Snapshot value;
    value.directory = source;
    value.owner.set_node_id("stable-node");
    value.owner.set_service_id("research-one");
    value.owner.set_domain("research");
    value.ownership = std::make_unique<FileLock>(source, "manager.lock");
    value.files.push_back({source / "input.pb", "input.pb", "protobuf"});
    value.files.push_back({source / "metadata.json", "metadata.json", "json"});
    value.directories.push_back("required-empty");
    value.exclusions.push_back("credentials");
    return value;
}

backup_files::Cut cut(const fs::path& source)
{
    std::vector<backup_files::Snapshot> values;
    values.push_back(snapshot(source));
    return backup_files::Cut(std::move(values));
}

void seed(const fs::path& source)
{
    fs::create_directory(source);
    fs::create_directory(source / "required-empty");
    replace_file_durably(source / "input.pb", "synthetic immutable input");
    replace_file_durably(source / "metadata.json", "{\"name\":\"synthetic research\"}");
    replace_file_durably(source / "credentials.private", "synthetic-excluded-secret");
}

} // namespace

TEST(BackupFiles, HeldColdCutPublishesVerifiedIndependentFilesAndKeepsOriginals)
{
    Folder directory;
    const auto source = directory.root / "source";
    seed(source);
    const auto before = sha256_file(source / "input.pb");
    const auto secret = sha256_file(source / "credentials.private");
    auto current = cut(source);
    EXPECT_THROW(FileLock writer(source, "manager.lock"), std::runtime_error);
    const auto plan = current.preview("backup-one");
    EXPECT_EQ(plan.at("owners")[0].at("node"), "stable-node");
    EXPECT_EQ(plan.at("files").size(), 2U);
    EXPECT_EQ(plan.dump().find(source.string()), std::string::npos);
    EXPECT_EQ(plan.dump().find("synthetic-excluded-secret"), std::string::npos);
    const auto backup = directory.root / "backup";
    EXPECT_EQ(current.save("backup-one", backup, sha256_bytes(plan.dump())), plan);
    EXPECT_EQ(backup_files::inspect(backup), plan);
    EXPECT_FALSE(fs::exists(backup / "owners" / "0" / "credentials.private"));
    const auto restored = directory.root / "independent";
    EXPECT_EQ(backup_files::restore_files(backup, restored), plan);
    EXPECT_EQ(sha256_file(restored / "owners" / "0" / "input.pb"), before);
    EXPECT_TRUE(fs::is_directory(restored / "owners" / "0" / "required-empty"));
    EXPECT_EQ(sha256_file(source / "input.pb"), before);
    EXPECT_EQ(sha256_file(source / "credentials.private"), secret);
    EXPECT_THROW(backup_files::restore_files(backup, source), Error);
    EXPECT_THROW(backup_files::restore_files(backup, restored), Error);
}

TEST(BackupFiles, ExistingTargetsAndChangedPreviewsFailBeforeOverwritingAnything)
{
    Folder directory;
    const auto source = directory.root / "source";
    seed(source);
    auto current = cut(source);
    const auto plan = current.preview("backup-one");
    const auto existing = directory.root / "existing";
    fs::create_directory(existing);
    replace_file_durably(existing / "keep", "existing user artifact");
    const auto before = sha256_file(existing / "keep");
    EXPECT_THROW(current.save("backup-one", existing, sha256_bytes(plan.dump())), std::exception);
    EXPECT_EQ(sha256_file(existing / "keep"), before);
    EXPECT_FALSE(fs::exists(existing / "backup.lock"));
    replace_file_durably(source / "metadata.json", "{\"name\":\"changed by user\"}");
    const auto target = directory.root / "new";
    EXPECT_THROW(current.save("backup-one", target, sha256_bytes(plan.dump())), Error);
    EXPECT_FALSE(fs::exists(target));
    EXPECT_THROW(
        current.save(
            "backup-one",
            source / "nested",
            sha256_bytes(current.preview("backup-one").dump())),
        std::invalid_argument);
}

TEST(BackupFiles, PreparedOperationIsNotACompleteBackupAndSameIdentityCanResumeIdempotently)
{
    Folder directory;
    const auto source = directory.root / "source";
    seed(source);
    auto current = cut(source);
    const auto plan = current.preview("backup-one");
    const auto partial = directory.root / "partial";
    fs::create_directory(partial);
    {
        FileLock prepare(partial, "backup.lock");
    }
    replace_file_durably(partial / "plan.json", plan.dump()); // Crash after durable preparation.
    EXPECT_THROW(backup_files::inspect(partial), std::invalid_argument);
    EXPECT_EQ(backup_files::inspect_preparation(partial), plan);
    EXPECT_FALSE(fs::exists(partial / "complete.sha256"));
    const auto hash = sha256_bytes(plan.dump());
    EXPECT_EQ(current.save("backup-one", partial, hash), plan);
    EXPECT_EQ(current.save("backup-one", partial, hash), plan);
    EXPECT_THROW(
        current.save(
            "another-backup",
            partial,
            sha256_bytes(current.preview("another-backup").dump())),
        Error);
    EXPECT_EQ(backup_files::inspect(partial), plan);
}

TEST(BackupFiles, CorruptionOrUndeclaredFilesRejectRestoreBeforeCreatingTheTarget)
{
    Folder directory;
    const auto source = directory.root / "source";
    seed(source);
    auto current = cut(source);
    const auto plan = current.preview("backup-one");
    const auto backup = directory.root / "backup";
    (void)current.save("backup-one", backup, sha256_bytes(plan.dump()));
    replace_file_durably(backup / "owners" / "0" / "input.pb", "damaged");
    const auto target = directory.root / "not-created";
    EXPECT_THROW(backup_files::restore_files(backup, target), std::invalid_argument);
    EXPECT_FALSE(fs::exists(target));
    EXPECT_EQ(
        sha256_file(source / "input.pb"),
        plan.at("files")[0].at("sha256").get<std::string>());
    replace_file_durably(backup / "owners" / "0" / "input.pb", "synthetic immutable input");
    replace_file_durably(backup / "unexpected.private", "synthetic forbidden extra");
    EXPECT_THROW(backup_files::restore_files(backup, target), std::invalid_argument);
    EXPECT_FALSE(fs::exists(target));
}
