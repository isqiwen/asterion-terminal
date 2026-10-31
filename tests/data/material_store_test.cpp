#include "material_store.hpp"
#include <asterion/system/durable_file.hpp>
#include <asterion/system/process/child.hpp>
#include <gtest/gtest.h>
using namespace asterion;
namespace fs = std::filesystem;

namespace {

struct Folder
{
    fs::path path = fs::temp_directory_path() / ("ast-material-" + unique_process_id());

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
    result.set_service_id("data-one");
    result.set_domain("data");
    return result;
}

data::proto::MaterialRequest
request(const std::string& id, const std::string& source = "source-one")
{
    data::proto::MaterialRequest result;
    result.set_id(id);
    result.set_source_id(source);
    result.set_resource_id("announcement-one");
    result.set_use_scope("isolated research fixture");
    result.set_provider_artifact(std::string(64, 'a'));
    result.set_max_requests(1);
    result.set_max_bytes(protocol::max_material_bytes);
    return result;
}

} // namespace

TEST(MaterialStore, ReusesOriginalButPreservesEachAcquisitionAndRestartIdentity)
{
    Folder f;
    const auto guard = f.path / "materials.required.pb";
    const std::string body = "工厂甲计划检修\n";
    data::Materials store(f.path, owner());
    EXPECT_TRUE(store.receipts().empty());
    EXPECT_FALSE(fs::exists(guard));
    const auto first = store.publish(store.prepare(request("one"), body, "text/plain", 100));
    const auto second =
        store.publish(store.prepare(request("two", "source-two"), body, "text/plain", 200, 90));
    EXPECT_EQ(
        std::distance(
            fs::directory_iterator(f.path / "materials/objects"),
            fs::directory_iterator()),
        1);
    EXPECT_EQ(store.receipts().size(), 2);
    EXPECT_EQ(first.evidence().content().version(), second.evidence().content().version());
    EXPECT_NE(first.evidence().request().source_id(), second.evidence().request().source_id());
    data::Materials reopened(f.path, owner());
    const auto retry = reopened.publish(reopened.prepare(request("one"), body, "text/plain", 300));
    EXPECT_EQ(retry.SerializeAsString(), first.SerializeAsString());
    EXPECT_EQ(reopened.read("one").bytes, body);
    EXPECT_FALSE(reopened.read("one").receipt.evidence().has_source_published_at_ms());
    EXPECT_THROW(reopened.prepare(request("one"), "changed", "text/plain", 400), Error);
    EXPECT_THROW(reopened.prepare(request("one", "other-source"), body, "text/plain", 400), Error);
}

TEST(MaterialStore, PublicationFailureDoesNotAcknowledgeAndRetainsExactOriginal)
{
    Folder f;
    data::Materials store(f.path, owner());
    const auto prepared = store.prepare(request("one"), "original", "text/plain", 100);
    (void)store.publish(prepared);
    fs::remove(
        f.path
        / "materials/receipts/one.pb"); // Simulate crash after original publication, before receipt.
    EXPECT_FALSE(store.find("one"));
    EXPECT_EQ(store.publish(prepared).SerializeAsString(), prepared.receipt().SerializeAsString());
    const auto old = store.find("one")->SerializeAsString();
    replace_file_durably(
        f.path / "materials/objects" / prepared.receipt().evidence().content().version(),
        "modified");
    EXPECT_THROW(store.read("one"), std::invalid_argument);
    EXPECT_THROW(
        store.publish(store.prepare(request("two"), "original", "text/plain", 200)),
        std::invalid_argument);
    EXPECT_EQ(store.find("one")->SerializeAsString(), old);
    auto wrong = owner();
    wrong.set_service_id("other");
    EXPECT_THROW((data::Materials(f.path, wrong)), Error);
}

TEST(MaterialStore, MissingAuthorityAndUnsupportedReadingNeverBecomeSemanticSuccess)
{
    Folder f;
    data::Materials store(f.path, owner());
    store.publish(store.prepare(request("pdf"), "%PDF-fixture", "application/pdf", 100));
    const auto read = store.read("pdf");
    EXPECT_EQ(read.text.state(), data::proto::MaterialText::UNSUPPORTED_MEDIA);
    EXPECT_EQ(store.receipts().size(), 1);
    fs::remove(f.path / "materials.required.pb");
    EXPECT_THROW(store.read("pdf"), std::invalid_argument);
    EXPECT_THROW(
        store.publish(store.prepare(request("next"), "text", "text/plain", 200)),
        std::invalid_argument);
    EXPECT_TRUE(fs::exists(f.path / "materials/receipts/pdf.pb"));
    EXPECT_FALSE(fs::exists(f.path / "materials.required.pb"));
}
