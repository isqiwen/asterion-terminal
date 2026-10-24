#include "program_files.hpp"
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/foundation/error.hpp>
#include <gtest/gtest.h>
#include <fstream>
using namespace asterion;
namespace fs = std::filesystem;

namespace {

struct Folder
{
    fs::path path = fs::temp_directory_path() / ("asterion-program-files-" + unique_process_id());

    Folder()
    {
        fs::create_directory(path);
    }

    ~Folder()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

resource::proto::Owner owner()
{
    resource::proto::Owner value;
    value.set_node_id("fixture-node");
    value.set_service_id("research-one");
    value.set_domain("research");
    return value;
}

} // namespace

TEST(ProgramFiles, ExactNativeProgramIsPreservedOnceAndReadOnlyListingDoesNotCreateAnArchive)
{
    Folder folder;
    programs::Files archive(folder.path / "programs", owner());
    EXPECT_TRUE(archive.catalogue().empty());
    EXPECT_FALSE(fs::exists(folder.path / "programs"));
    const auto source = current_executable();
    const auto expected = sha256_file(source);
    const auto first = archive.preserve(source, expected);
    EXPECT_EQ(first.digest, expected);
    EXPECT_EQ(first.bytes, fs::file_size(source));
    EXPECT_EQ(sha256_file(archive.path(expected)), expected);
    const auto before = fs::last_write_time(archive.path(expected));
    EXPECT_EQ(archive.preserve(source, expected).digest, expected);
    const auto entries = archive.catalogue();
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].digest, expected);
    EXPECT_EQ(fs::last_write_time(archive.path(expected)), before);
}

TEST(ProgramFiles, OwnerDriftMissingAuthorityAndCorruptBytesCannotBeReplacedOrRebound)
{
    Folder folder;
    const auto source = current_executable();
    const auto hash = sha256_file(source);
    programs::Files archive(folder.path / "programs", owner());
    archive.preserve(source, hash);
    auto another = owner();
    another.set_node_id("another-node");
    EXPECT_THROW(programs::Files(folder.path / "programs", another).verify(hash), Error);
    write_file_durably(archive.path(hash), "corrupt-program");
    EXPECT_THROW(archive.verify(hash), std::exception);
    EXPECT_THROW(archive.preserve(source, hash), std::exception);
    EXPECT_EQ(fs::file_size(archive.path(hash)), 15U);
    fs::remove(folder.path / "programs/owner.pb");
    EXPECT_THROW(archive.preserve(source, hash), std::invalid_argument);
    EXPECT_FALSE(fs::exists(folder.path / "programs/owner.pb"));
}

TEST(ProgramFiles, InterruptedNamespacePublicationCanResumeWithoutAdoptingAnUnknownProgram)
{
    Folder folder;
    const auto source = current_executable();
    const auto hash = sha256_file(source);
    programs::Files archive(folder.path / "programs", owner());
    fail_next_directory_syncs_for_testing(1);
    EXPECT_THROW(archive.preserve(source, hash), std::runtime_error);
    fail_next_directory_syncs_for_testing(0);
    EXPECT_EQ(archive.preserve(source, hash).digest, hash);
    EXPECT_EQ(archive.verify(hash).bytes, fs::file_size(source));
    Folder unknown;
    fs::create_directory(unknown.path / "programs");
    write_file_durably(unknown.path / "programs/unknown", "preserve-me");
    EXPECT_THROW(
        programs::Files(unknown.path / "programs", owner()).preserve(source, hash),
        std::invalid_argument);
    EXPECT_FALSE(fs::exists(unknown.path / "programs/owner.pb"));
}
