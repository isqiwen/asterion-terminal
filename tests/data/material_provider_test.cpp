#include "material_providers.hpp"
#include "material_store.hpp"
#include <asterion/system/native_plugin.hpp>
#include <asterion/system/process/child.hpp>
#include <gtest/gtest.h>
#include <algorithm>
using namespace asterion;

namespace {

data::proto::MaterialRequest request(const std::string& resource = "announcement")
{
    static const bool configured = [] {
        configure_native_plugins(MATERIAL_FIXTURE_DIRECTORY);
        return true;
    }();
    (void)configured;
    const auto sources = material_providers::sources();
    const auto source = std::find_if(sources.begin(), sources.end(), [](const auto& s) {
        return s.id == "fixture-material";
    });
    if (source == sources.end())
        throw std::runtime_error("material test source missing");
    data::proto::MaterialRequest r;
    r.set_id("fixture-acquisition");
    r.set_source_id(source->id);
    r.set_resource_id(resource);
    r.set_use_scope("isolated synthetic research");
    r.set_provider_artifact(source->artifact);
    r.set_max_bytes(source->max_bytes);
    r.set_max_requests(1);
    return r;
}

} // namespace

TEST(MaterialProvider, ActualNativeResponsePersistsExactOriginalWithOwnerObservedAcquisitionTime)
{
    const auto r = request();
    unsigned calls = 0;
    const auto response = material_providers::fetch(r, [&](std::stop_token) {
        ++calls;
    });
    EXPECT_EQ(calls, 1);
    EXPECT_GT(response.acquired_at_ms, 0);
    EXPECT_FALSE(response.source_published_at_ms);
    const auto root =
        std::filesystem::temp_directory_path() / ("ast-material-provider-" + unique_process_id());
    std::filesystem::create_directory(root);

    struct Cleanup
    {
        std::filesystem::path root;

        ~Cleanup()
        {
            std::error_code e;
            std::filesystem::remove_all(root, e);
        }
    } cleanup{root};

    resource::proto::Owner owner;
    owner.set_node_id("fixture-node");
    owner.set_service_id("data-one");
    owner.set_domain("data");
    data::Materials materials(root, owner);
    const auto receipt = materials.publish(materials.prepare(
        r,
        response.bytes,
        response.media_type,
        response.acquired_at_ms,
        response.source_published_at_ms));
    EXPECT_EQ(receipt.evidence().request().provider_artifact(), r.provider_artifact());
    EXPECT_EQ(materials.read(r.id()).bytes, "工厂甲计划检修\n");
    EXPECT_EQ(materials.read(r.id()).text.state(), data::proto::MaterialText::READABLE);
}

TEST(MaterialProvider, AdmissionIdentityCostAccessAndCancellationBlockBeforeAnyExternalBudget)
{
    auto r = request();
    unsigned calls = 0;
    const auto budget = [&](std::stop_token) {
        ++calls;
    };
    auto mismatch = r;
    mismatch.set_provider_artifact(std::string(64, 'b'));
    EXPECT_THROW(material_providers::fetch(mismatch, budget), Error);
    for (const auto* source : {"fixture-paid", "fixture-auth"}) {
        auto blocked = r;
        blocked.set_source_id(source);
        EXPECT_THROW(material_providers::fetch(blocked, budget), Error);
    }
    std::stop_source stop;
    stop.request_stop();
    EXPECT_THROW(material_providers::fetch(r, budget, stop.get_token()), Error);
    EXPECT_EQ(calls, 0);
}

TEST(MaterialProvider, DeniedRequestsOrMissingOriginalCannotBeReportedAsAcquired)
{
    unsigned calls = 0;
    EXPECT_THROW(
        material_providers::fetch(
            request("over-budget"),
            [&](std::stop_token) {
                ++calls;
            }),
        Error);
    EXPECT_EQ(calls, 1);
    EXPECT_THROW(
        material_providers::fetch(request("no-original"), [](std::stop_token) {}),
        std::invalid_argument);
}
