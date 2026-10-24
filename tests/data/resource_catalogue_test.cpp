#include "data_store.hpp"
#include "data_retentions.hpp"
#include "data/history_fixture.hpp"
#include <asterion/protocol/resource.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <gtest/gtest.h>
#include <set>
using namespace asterion;
namespace fs = std::filesystem;

namespace {

struct Folder
{
    fs::path path = fs::temp_directory_path() / ("asterion-resource-" + unique_process_id());

    Folder()
    {
        fs::create_directory(path);
    }

    ~Folder()
    {
        std::error_code e;
        fs::remove_all(path, e);
    }
};

resource::proto::Owner owner()
{
    resource::proto::Owner result;
    result.set_node_id("fixture-node");
    result.set_service_id("historical-data");
    result.set_domain("data");
    return result;
}

} // namespace

TEST(DataResourceCatalogue, PagesStableIdentityAndLifecycleWithoutModifyingTheLedger)
{
    Folder folder;
    const auto seeded = test::seed_history(folder.path, {100, 101}, "catalogue");
    data::Store store(folder.path, "historical-data");
    const auto before = sha256_file(folder.path / "tables/ledger.sqlite");
    resource::proto::CatalogueQuery query;
    query.set_kind("table_snapshot");
    query.set_limit(1);
    std::set<std::string> ids;
    std::string selected;
    for (unsigned page = 0; page < 20; ++page) {
        const auto entries = store.resources(owner(), query, {});
        ASSERT_LE(entries.items_size(), 1);
        EXPECT_NO_THROW(protocol::decode_resource_catalogue(entries, query, owner()));
        for (const auto& entry : entries.items()) {
            EXPECT_TRUE(ids.insert(entry.object().id()).second);
            EXPECT_EQ(entry.object().version(), entry.object().id());
            EXPECT_FALSE(entry.has_logical_bytes());
            EXPECT_EQ(entry.relative_location(), "tables");
            if (entry.object().id() != store.tables().head())
                selected = entry.object().id();
        }
        if (entries.next_after().empty())
            break;
        query.set_after(entries.next_after());
    }
    ASSERT_GE(ids.size(), 3U);
    ASSERT_FALSE(selected.empty());
    query.set_after("");
    query.set_limit(128);
    const auto decision = store.tables().preview_retirement(selected);
    store.begin_table_retirement(decision);
    auto list = store.resources(owner(), query, {});
    const auto state = [&](const auto& page) {
        for (const auto& entry : page.items())
            if (entry.object().id() == selected)
                return entry.state();
        throw std::runtime_error("catalogue entry missing");
    };
    EXPECT_EQ(state(list), resource::proto::CatalogueEntry::SEALED);
    store.clean_table_retirement(selected, {});
    store.finish_table_retirement(selected);
    EXPECT_EQ(state(store.resources(owner(), query, {})), resource::proto::CatalogueEntry::RETIRED);
    query.set_kind("minute_version");
    const auto minutes = store.resources(owner(), query, {});
    ASSERT_EQ(minutes.items_size(), 1);
    EXPECT_EQ(
        minutes.items(0).object().id(),
        seeded.at("source_dataset_ids")[0].get<std::string>());
    EXPECT_TRUE(minutes.items(0).relative_location().starts_with("history/"));
    EXPECT_EQ(sha256_file(folder.path / "tables/ledger.sqlite"), before);
    query.set_kind("provider_module");
    const auto programs = store.resources(owner(), query, {});
    ASSERT_GE(programs.items_size(), 1);
    for (const auto& entry : programs.items()) {
        EXPECT_TRUE(entry.has_logical_bytes());
        const auto path = folder.path / entry.relative_location();
        EXPECT_EQ(sha256_file(path), entry.object().version());
        EXPECT_EQ(entry.logical_bytes(), fs::file_size(path));
    }
    EXPECT_NO_THROW(protocol::decode_resource_catalogue(programs, query, owner()));
    query.set_kind("unimplemented_model");
    EXPECT_THROW(store.resources(owner(), query, {}), Error);
    query.set_kind("table_snapshot");
    query.set_limit(129);
    EXPECT_THROW(store.resources(owner(), query, {}), std::invalid_argument);
    fs::rename(
        folder.path / "tables/retirements.sqlite",
        folder.path / "preserved-retirements.sqlite");
    query.set_limit(64);
    EXPECT_THROW(store.resources(owner(), query, {}), std::runtime_error);
}

TEST(ResourceCatalogueProtocol, RejectsWrongOwnerEscapingLocationAndInvalidContinuation)
{
    resource::proto::CatalogueQuery query;
    query.set_kind("table_snapshot");
    query.set_limit(1);
    resource::proto::Catalogue page;
    *page.mutable_owner() = owner();
    page.set_kind(query.kind());
    auto* entry = page.add_items();
    *entry->mutable_object()->mutable_owner() = owner();
    entry->mutable_object()->set_kind(query.kind());
    entry->mutable_object()->set_id(std::string(64, 'a'));
    entry->mutable_object()->set_version(std::string(64, 'a'));
    entry->set_state(resource::proto::CatalogueEntry::REGISTERED);
    entry->set_relative_location("tables");
    const auto decoded = protocol::decode_resource_catalogue(page, query, owner());
    EXPECT_TRUE(decoded.at("items")[0].at("logical_bytes").is_null());
    auto wrong = owner();
    wrong.set_node_id("another-node");
    EXPECT_THROW(protocol::decode_resource_catalogue(page, query, wrong), Error);
    entry->set_relative_location("../private");
    EXPECT_THROW(protocol::decode_resource_catalogue(page, query, owner()), Error);
    entry->set_relative_location("tables");
    page.set_next_after(std::string(64, 'b'));
    EXPECT_THROW(protocol::decode_resource_catalogue(page, query, owner()), Error);
}
