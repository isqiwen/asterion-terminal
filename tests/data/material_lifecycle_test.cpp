#include "material_store.hpp"
#include "material_providers.hpp"
#include "data_store.hpp"
#include "data_retentions.hpp"
#include "data_backup.hpp"
#include "domain_backup.hpp"
#include <asterion/system/native_plugin.hpp>
#include <asterion/system/durable_file.hpp>
#include <asterion/system/process/child.hpp>
#include <asterion/system/process/artifact.hpp>
#include <gtest/gtest.h>
#include <algorithm>
using namespace asterion;
namespace fs = std::filesystem;

namespace {

struct Folder
{
    fs::path path = fs::temp_directory_path() / ("ast-material-life-" + unique_process_id());

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
    resource::proto::Owner o;
    o.set_node_id("fixture-node");
    o.set_service_id("data-one");
    o.set_domain("data");
    return o;
}

data::proto::DataOwner source()
{
    data::proto::DataOwner o;
    o.set_node_id("fixture-node");
    o.set_service_id("data-one");
    return o;
}

data::proto::MaterialRequest request(const std::string& id)
{
    static const bool configured = [] {
        configure_native_plugins(MATERIAL_FIXTURE_DIRECTORY);
        return true;
    }();
    (void)configured;
    const auto sources = material_providers::sources();
    const auto s = std::find_if(sources.begin(), sources.end(), [](const auto& x) {
        return x.id == "fixture-material";
    });
    if (s == sources.end())
        throw std::runtime_error("material fixture missing");
    data::proto::MaterialRequest r;
    r.set_id(id);
    r.set_source_id(s->id);
    r.set_resource_id("announcement");
    r.set_use_scope("isolated research");
    r.set_provider_artifact(s->artifact);
    r.set_max_bytes(4096);
    r.set_max_requests(1);
    return r;
}

void seed(const fs::path& root)
{
    fs::create_directory(root);
    data::Store store(root, "data-one");
    data::Retentions retentions(root, source(), store);
    resource::proto::CatalogueQuery empty;
    empty.set_kind("material_acquisition");
    empty.set_limit(1);
    EXPECT_EQ(store.resources(owner(), empty, {}).items_size(), 0);
    EXPECT_FALSE(fs::exists(root / "materials.required.pb"));
    data::Materials materials(root, owner());
    const auto first = request("one");
    material_providers::preserve(first, root / "programs");
    materials.publish(materials.prepare(first, "原始材料\n", "text/plain", 100));
    auto second = request("two");
    second.set_resource_id("reprint");
    materials.publish(materials.prepare(second, "原始材料\n", "text/plain", 200));
    auto page = store.resources(owner(), empty, {});
    ASSERT_EQ(page.items_size(), 1);
    EXPECT_EQ(page.items(0).object().id(), "one");
    EXPECT_EQ(page.next_after(), "one");
    EXPECT_EQ(
        page.items(0).object().version(),
        materials.read("one").receipt.reference().version());
    empty.set_after(page.next_after());
    page = store.resources(owner(), empty, {});
    ASSERT_EQ(page.items_size(), 1);
    EXPECT_EQ(page.items(0).object().id(), "two");
    empty.clear_after();
    empty.set_kind("material_content");
    EXPECT_EQ(store.resources(owner(), empty, {}).items_size(), 1);
}

} // namespace

TEST(
    MaterialLifecycle,
    ExactAcquisitionsSharedOriginalAndProviderRestoreWithoutOriginalDirectoryOrExecution)
{
    Folder f;
    const auto root = f.path / "source";
    seed(root);
    const auto before = data::Materials(root, owner()).read("one");
    const auto saved = f.path / "backup";
    {
        auto cut = terminal_backup::cold_cut({{owner(), root}});
        const auto plan = cut.preview("material-backup");
        EXPECT_FALSE(plan.at("dependencies").empty());
        for (const auto& d : plan.at("dependencies"))
            EXPECT_TRUE(d.at("included").get<bool>());
        (void)cut.save("material-backup", saved, sha256_bytes(plan.dump()));
    }
    const auto restored = f.path / "restore";
    (void)backup_files::restore_files(saved, restored);
    fs::rename(root, f.path / "original-preserved");
    const auto copied = restored / "owners/0";
    auto verify = data::cold_backup(copied, source());
    EXPECT_EQ(
        data::Materials(copied, owner()).read("one").receipt.SerializeAsString(),
        before.receipt.SerializeAsString());
    EXPECT_EQ(data::Materials(copied, owner()).read("one").bytes, before.bytes);
    EXPECT_EQ(data::Materials(copied, owner()).receipts().size(), 2);
    EXPECT_FALSE(fs::exists(copied / "authorizations"));
    EXPECT_FALSE(fs::exists(copied / "process.json"));
    EXPECT_TRUE(std::ranges::any_of(verify.objects, [](const auto& o) {
        return o.kind() == "material_acquisition" && o.id() == "one";
    }));
}

TEST(MaterialLifecycle, PreservesUnconfirmedOriginalAndRefusesLossOrCorruptionOfPublishedEvidence)
{
    Folder f;
    const auto root = f.path / "source";
    seed(root);
    const auto orphan = sha256_bytes("unconfirmed original");
    replace_file_durably(root / "materials/objects" / orphan, "unconfirmed original");
    {
        auto backup = data::cold_backup(root, source());
        EXPECT_NE(
            std::ranges::find(
                backup.snapshot.limitations,
                "unreferenced_material_original_retained"),
            backup.snapshot.limitations.end());
        EXPECT_TRUE(std::ranges::any_of(backup.snapshot.files, [&](const auto& f) {
            return f.relative == fs::path("materials/objects") / orphan;
        }));
        EXPECT_FALSE(std::ranges::any_of(backup.objects, [&](const auto& o) {
            return o.id() == orphan;
        }));
    }
    const auto receipt = data::Materials(root, owner()).read("one").receipt;
    replace_file_durably(
        root / "materials/objects" / receipt.evidence().content().version(),
        "changed bytes");
    EXPECT_THROW(data::cold_backup(root, source()), std::invalid_argument);
    fs::remove(root / "materials.required.pb");
    EXPECT_THROW(data::cold_backup(root, source()), std::invalid_argument);
    EXPECT_TRUE(fs::exists(root / "materials/receipts/one.pb"));
    EXPECT_TRUE(fs::exists(root / "materials/objects" / orphan));
}
