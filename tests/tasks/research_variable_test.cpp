#include "research_tasks.hpp"
#include "variable_store.hpp"
#include "task_store.hpp"
#include <asterion/protocol/research_variable.hpp>
#include <asterion/foundation/id.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <gtest/gtest.h>
using namespace asterion;
namespace fs = std::filesystem;

namespace {

struct ResearchVariables : testing::Test
{
    fs::path root =
        fs::temp_directory_path() / ("asterion-variable-fixture-" + unique_process_id());
    resource::proto::Owner owner;
    std::unique_ptr<tasks::Store> tasks;

    void SetUp() override
    {
        fs::create_directory(root);
        root = fs::canonical(root);
        owner.set_domain("research");
        owner.set_node_id("fixture-node");
        owner.set_service_id("research-one");
        tasks = std::make_unique<tasks::Store>(
            asterion::research::task_domain(),
            root,
            tasks::Identity{"research-one"});
    }

    void TearDown() override
    {
        tasks.reset();
        std::error_code error;
        fs::remove_all(root, error);
    }

    research::proto::VariableMutation registration(
        std::string command = "register-one",
        std::string id = "feature-one",
        unsigned window = 2)
    {
        research::proto::VariableMutation request;
        request.set_id(command);
        auto& d = *request.mutable_registration();
        d.set_id(id);
        d.set_name("Declared return feature");
        d.set_purpose("Predictor after the current close is known; never a forward label");
        d.set_expression("close / lag(close, window) - 1");
        d.set_window(window);
        return request;
    }
};

} // namespace

TEST_F(ResearchVariables, EmptyReadOnlyDiscoveryDoesNotCreateVariableAuthority)
{
    const auto before = sha256_file(root / "tasks.sqlite");
    const auto valid = research::Variables::preview(registration().registration());
    EXPECT_TRUE(valid.valid());
    EXPECT_EQ(valid.required_history(), 2U);
    auto invalid = registration().registration();
    invalid.set_expression("forward_return");
    const auto checked = research::Variables::preview(invalid);
    EXPECT_FALSE(checked.valid());
    ASSERT_TRUE(checked.has_error_offset());
    EXPECT_EQ(checked.error_offset(), 0U);
    {
        research::Variables read(*tasks, owner);
        EXPECT_TRUE(read.list("", 10).empty());
        resource::proto::CatalogueQuery query;
        query.set_kind("research_variable");
        query.set_limit(10);
        EXPECT_TRUE(read.resources(query).items().empty());
        EXPECT_THROW(read.definition("absent"), Error);
        EXPECT_THROW(read.apply(registration()), Error);
    }
    EXPECT_EQ(sha256_file(root / "tasks.sqlite"), before);
    tasks.reset();
    sqlite::Database database(root / "tasks.sqlite");
    sqlite::Database::Statement tables(
        database,
        "SELECT count(*) FROM sqlite_master WHERE name LIKE 'research_variable_%'");
    ASSERT_TRUE(tables.step());
    EXPECT_EQ(tables.integer(0), 0);
}

TEST_F(ResearchVariables, SameNameNewDefinitionKeepsOriginalContentAndReceiptAcrossRestart)
{
    research::proto::VariableDefinition original, changed;
    {
        research::Variables write(*tasks, owner, sqlite::Database::Access::writer);
        original = write.apply(registration()).definition();
        changed = write.apply(registration("register-two", "feature-two", 3)).definition();
        EXPECT_EQ(original.name(), changed.name());
        EXPECT_NE(original.digest(), changed.digest());
        EXPECT_EQ(original.required_history(), 2U);
        EXPECT_EQ(original.price_power(), 0);
        EXPECT_EQ(
            write.apply(registration()).definition().SerializeAsString(),
            original.SerializeAsString());
        EXPECT_EQ(
            write.definition(original.id()).SerializeAsString(),
            original.SerializeAsString());
        EXPECT_THROW(write.apply(registration("another-command", "feature-one", 3)), Error);
        EXPECT_THROW(write.apply(registration("register-one", "different-feature", 2)), Error);
    }
    tasks.reset();
    tasks = std::make_unique<tasks::Store>(
        asterion::research::task_domain(),
        root,
        tasks::Identity{"research-one"});
    research::Variables read(*tasks, owner);
    EXPECT_EQ(read.definition(original.id()).SerializeAsString(), original.SerializeAsString());
    EXPECT_EQ(read.list("", 1).front().id(), "feature-one");
    EXPECT_EQ(read.list("feature-one", 1).front().id(), "feature-two");
    EXPECT_EQ(read.definition(changed.id()).window(), 3U);
    auto object = protocol::variable_object(original);
    read.verify(object);
    object.set_version(std::string(64, 'a'));
    EXPECT_THROW(read.verify(object), Error);
}

TEST_F(ResearchVariables, InvalidExpressionOrPurposeCannotRegisterAUsableDefinition)
{
    research::Variables write(*tasks, owner, sqlite::Database::Access::writer);
    auto future = registration();
    future.mutable_registration()->set_expression("forward_return");
    EXPECT_THROW(write.apply(future), std::invalid_argument);
    EXPECT_TRUE(write.list("", 10).empty());
    auto no_purpose = registration();
    no_purpose.mutable_registration()->set_purpose("   ");
    EXPECT_THROW(write.apply(no_purpose), std::invalid_argument);
    EXPECT_TRUE(write.list("", 10).empty());
    auto nested = registration();
    nested.mutable_registration()->set_expression("mean(return(close, 2), 3)");
    const auto saved = write.apply(nested).definition();
    EXPECT_EQ(saved.required_history(), 4U);
    EXPECT_EQ(saved.expression(), nested.registration().expression());
    const auto stable = write.definition(saved.id()).SerializeAsString();
    auto wrong = owner;
    wrong.set_node_id("other-node");
    EXPECT_THROW(research::Variables(*tasks, wrong), Error);
    EXPECT_EQ(write.definition(saved.id()).SerializeAsString(), stable);
}

TEST_F(ResearchVariables, StoppedReadOnlyInspectionChecksExactObjectsAndOriginalReceipts)
{
    research::proto::VariableDefinition saved;
    {
        research::Variables write(*tasks, owner, sqlite::Database::Access::writer);
        saved = write.apply(registration()).definition();
    }
    tasks.reset();
    const auto before = sha256_file(root / "tasks.sqlite");
    std::vector<resource::proto::Object> refs{protocol::variable_object(saved)};
    unsigned visited = 0;
    research::Variables::inspect_backup(root, owner, refs, [&](const auto& value) {
        ++visited;
        EXPECT_EQ(value.SerializeAsString(), saved.SerializeAsString());
    });
    EXPECT_EQ(visited, 1U);
    EXPECT_EQ(sha256_file(root / "tasks.sqlite"), before);
    refs[0].set_version(std::string(64, 'a'));
    EXPECT_THROW(research::Variables::inspect_backup(root, owner, refs, [](const auto&) {}), Error);
    EXPECT_EQ(sha256_file(root / "tasks.sqlite"), before);
}

TEST_F(ResearchVariables, PartialMetadataSchemaIsNotSilentlyRecreated)
{
    {
        research::Variables write(*tasks, owner, sqlite::Database::Access::writer);
        (void)write.apply(registration());
    }
    tasks.reset();
    {
        sqlite::Database database(root / "tasks.sqlite");
        database.execute("DROP TABLE research_variable_commands");
    }
    tasks = std::make_unique<tasks::Store>(
        asterion::research::task_domain(),
        root,
        tasks::Identity{"research-one"});
    EXPECT_THROW(
        research::Variables(*tasks, owner, sqlite::Database::Access::writer),
        std::invalid_argument);
    tasks.reset();
    sqlite::Database database(root / "tasks.sqlite");
    sqlite::Database::Statement rows(
        database,
        "SELECT count(*) FROM research_variable_definitions");
    ASSERT_TRUE(rows.step());
    EXPECT_EQ(rows.integer(0), 1);
    sqlite::Database::Statement tables(
        database,
        "SELECT count(*) FROM sqlite_master WHERE name='research_variable_commands'");
    ASSERT_TRUE(tables.step());
    EXPECT_EQ(tables.integer(0), 0);
}

TEST_F(ResearchVariables, NativeReplyKeepsPurposeAndRejectsAnotherOwnerOrDefinition)
{
    research::proto::VariableRequest request;
    *request.mutable_mutation() = registration();
    research::Variables write(*tasks, owner, sqlite::Database::Access::writer);
    const auto reply = write.apply(request.mutation());
    const auto decoded = protocol::decode_variable_reply(reply, request, owner);
    EXPECT_EQ(decoded.at("definition").at("purpose"), registration().registration().purpose());
    EXPECT_EQ(decoded.at("definition").at("required_history"), 2);
    EXPECT_EQ(decoded.at("definition").at("object").at("version"), reply.definition().digest());
    auto other = owner;
    other.set_node_id("wrong-node");
    EXPECT_THROW(protocol::decode_variable_reply(reply, request, other), Error);
    auto forged = request;
    forged.mutable_mutation()->mutable_registration()->set_window(3);
    EXPECT_THROW(protocol::decode_variable_reply(reply, forged, owner), Error);
}
