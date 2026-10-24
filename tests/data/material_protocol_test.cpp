#include <asterion/protocol/material.hpp>
#include <gtest/gtest.h>
using namespace asterion;

namespace {

resource::proto::Owner owner()
{
    resource::proto::Owner result;
    result.set_node_id("fixture-node");
    result.set_service_id("data-one");
    result.set_domain("data");
    return result;
}

data::proto::MaterialRequest request(const std::string& id = "acquisition-one")
{
    data::proto::MaterialRequest result;
    result.set_id(id);
    result.set_source_id("fixture-source");
    result.set_resource_id("announcement-one");
    result.set_use_scope("isolated research fixture");
    result.set_provider_artifact(std::string(64, 'a'));
    result.set_max_requests(1);
    result.set_max_bytes(protocol::max_material_bytes);
    return result;
}

} // namespace

TEST(Material, PreservesOriginalBytesAndAcquisitionTimesWithoutInventingPublication)
{
    const std::string bytes = "工厂甲计划检修\r\nNo actual shutdown confirmed.\n";
    const auto first = protocol::freeze_material(owner(), request(), bytes, "text/plain", 100);
    const auto second = protocol::freeze_material(
        owner(),
        request("acquisition-two"),
        bytes,
        "text/plain",
        200,
        90);
    EXPECT_EQ(
        first.evidence().content().SerializeAsString(),
        second.evidence().content().SerializeAsString());
    EXPECT_NE(first.reference().SerializeAsString(), second.reference().SerializeAsString());
    EXPECT_FALSE(first.evidence().has_source_published_at_ms());
    EXPECT_EQ(second.evidence().source_published_at_ms(), 90);
    const auto parsed = protocol::read_material_text(first, bytes);
    EXPECT_EQ(parsed.state(), data::proto::MaterialText::READABLE);
    EXPECT_EQ(protocol::material_excerpt(first, bytes, 0, 9), "工厂甲");
    EXPECT_EQ(protocol::material_excerpt(first, bytes, 0, bytes.size()), bytes);
    EXPECT_THROW(protocol::material_excerpt(first, bytes, 1, 9), std::invalid_argument);
    EXPECT_THROW(protocol::material_excerpt(first, bytes, 0, 8), std::invalid_argument);
    EXPECT_THROW(protocol::read_material_text(first, bytes + "changed"), std::invalid_argument);
}

TEST(Material, AcquisitionIsSeparateFromReadableCoverageAndSemanticPublication)
{
    for (const std::string bytes :
        {std::string("\xc0\xaf"), std::string("\xed\xa0\x80"), std::string("a\0b", 3)}) {
        const auto receipt =
            protocol::freeze_material(owner(), request(), bytes, "text/plain", 100);
        EXPECT_EQ(
            protocol::read_material_text(receipt, bytes).state(),
            data::proto::MaterialText::INVALID_UTF8);
        EXPECT_NO_THROW(protocol::validate_material_receipt(receipt));
    }
    const auto pdf =
        protocol::freeze_material(owner(), request(), "%PDF-fixture", "application/pdf", 100);
    EXPECT_EQ(
        protocol::read_material_text(pdf, "%PDF-fixture").state(),
        data::proto::MaterialText::UNSUPPORTED_MEDIA);
    auto bad = pdf;
    bad.mutable_reference()->mutable_owner()->set_service_id("other-data");
    EXPECT_THROW(protocol::validate_material_receipt(bad), std::invalid_argument);
    EXPECT_THROW(
        protocol::freeze_material(owner(), request(), "", "text/plain", 100),
        std::invalid_argument);
}
