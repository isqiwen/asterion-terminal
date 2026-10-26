#include "research_tasks.hpp"
#include "project_store.hpp"
#include <asterion/protocol/research_project.hpp>
#include "task_store.hpp"
#include <asterion/foundation/id.hpp>
#include <asterion/kernel/process/child.hpp>
#include <gtest/gtest.h>

using namespace asterion;
namespace fs = std::filesystem;

namespace {

struct ResearchProjects : testing::Test
{
    fs::path root = fs::temp_directory_path() / ("asterion-project-fixture-" + unique_process_id());
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

    research::proto::ProjectMutation save(
        std::string command = "save-one",
        std::string expected = "",
        std::string project = "question-one")
    {
        research::proto::ProjectMutation request;
        request.set_id(command);
        auto& save = *request.mutable_save();
        save.set_project_id(project);
        save.set_expected_digest(expected);
        auto& d = *save.mutable_definition();
        d.set_title("Fixture hypothesis");
        d.set_question("Does the chosen factor predict a later return?");
        d.set_hypothesis("A declared relation, not an established fact");
        d.set_support("Declared direction and sufficient independent observations");
        d.set_refutation("Opposite direction or inadequate evidence");
        d.set_notes("# Synthetic research note");
        return request;
    }

    research::proto::ProjectMutation fix(
        const research::proto::ProjectDraft& draft,
        std::string id = "plan-one",
        std::string parent = "",
        std::string reason = "")
    {
        research::proto::ProjectMutation request;
        request.set_id("fix-" + id);
        auto& f = *request.mutable_fix();
        f.set_plan_id(id);
        f.set_project_id(draft.id());
        f.set_expected_draft_digest(draft.digest());
        f.set_parent_plan_id(parent);
        f.set_revision_reason(reason);
        return request;
    }
};

} // namespace

TEST_F(ResearchProjects, ReadOnlyEmptyDiscoveryDoesNotInitializeProjectTables)
{
    {
        research::Projects read(*tasks, owner);
        EXPECT_TRUE(read.drafts("", 10).empty());
        for (const auto kind : {"research_project", "research_plan", "research_conclusion"}) {
            resource::proto::CatalogueQuery query;
            query.set_kind(kind);
            query.set_limit(1);
            const auto result = read.resources(query);
            EXPECT_TRUE(result.items().empty());
            EXPECT_EQ(result.owner().SerializeAsString(), owner.SerializeAsString());
        }
        EXPECT_FALSE(read.draft("question-one"));
        EXPECT_THROW(read.apply(save()), Error);
        EXPECT_THROW(read.drafts("", 0), std::invalid_argument);
    }
    tasks.reset();
    sqlite::Database db(root / "tasks.sqlite", sqlite::Database::Access::read_only);
    sqlite::Database::Statement count(
        db,
        "SELECT count(*) FROM sqlite_master WHERE name LIKE 'research_project_%'");
    ASSERT_TRUE(count.step());
    EXPECT_EQ(count.integer(0), 0);
}

TEST_F(ResearchProjects, ImmutablePlansKeepOriginalStandardsAndRevisionReasonsAcrossReopen)
{
    research::proto::HypothesisPlan first, revised;
    {
        research::Projects write(*tasks, owner, sqlite::Database::Access::writer);
        const auto draft = write.apply(save()).draft();
        first = write.apply(fix(draft)).plan();
        EXPECT_FALSE(first.exploratory());
        auto edit = save("save-two", draft.digest());
        edit.mutable_save()->mutable_definition()->set_support(
            "A changed criterion, for later experiments only");
        const auto changed = write.apply(edit).draft();
        EXPECT_THROW(write.apply(fix(changed, "plan-two", first.id())), std::invalid_argument);
        revised =
            write.apply(fix(changed, "plan-two", first.id(), "Record why this criterion changed"))
                .plan();
        EXPECT_EQ(write.plan(first.id()).SerializeAsString(), first.SerializeAsString());
        EXPECT_NE(revised.definition_digest(), first.definition_digest());
    }
    tasks.reset();
    tasks = std::make_unique<tasks::Store>(
        asterion::research::task_domain(),
        root,
        tasks::Identity{"research-one"});
    research::Projects reopened(*tasks, owner);
    EXPECT_EQ(reopened.plan(first.id()).SerializeAsString(), first.SerializeAsString());
    EXPECT_EQ(reopened.plan(revised.id()).parent_plan_id(), first.id());
    EXPECT_EQ(reopened.plans("question-one", "", 10).size(), 2U);
    EXPECT_EQ(reopened.drafts("", 1).size(), 1U);
}

TEST_F(ResearchProjects, OldDraftsCannotOverwriteConcurrentEditsAndReceiptsReplayOriginalReplies)
{
    research::Projects write(*tasks, owner, sqlite::Database::Access::writer);
    const auto request = save();
    const auto first = write.apply(request);
    auto edit = save("save-two", first.draft().digest());
    edit.mutable_save()->mutable_definition()->set_notes("Later edit");
    const auto second = write.apply(edit);
    EXPECT_EQ(write.apply(request).SerializeAsString(), first.SerializeAsString());
    EXPECT_EQ(write.draft("question-one")->SerializeAsString(), second.draft().SerializeAsString());
    auto stale = save("save-stale", first.draft().digest());
    EXPECT_THROW(write.apply(stale), Error);
    auto reused = request;
    reused.mutable_save()->mutable_definition()->set_title("Different payload");
    EXPECT_THROW(write.apply(reused), Error);
    EXPECT_EQ(write.draft("question-one")->digest(), second.draft().digest());
}

TEST_F(ResearchProjects, CommitFailureRollsBackBothDefinitionAndIdempotencyReceipt)
{
    const auto request = save();
    {
        research::Projects write(*tasks, owner, sqlite::Database::Access::writer);
        sqlite::fail_next_commits_for_testing(1);
        EXPECT_THROW(write.apply(request), std::runtime_error);
        EXPECT_FALSE(write.draft("question-one"));
    }
    tasks.reset();
    tasks = std::make_unique<tasks::Store>(
        asterion::research::task_domain(),
        root,
        tasks::Identity{"research-one"});
    research::Projects recovered(*tasks, owner, sqlite::Database::Access::writer);
    const auto result = recovered.apply(request);
    EXPECT_EQ(recovered.apply(request).SerializeAsString(), result.SerializeAsString());
    EXPECT_EQ(recovered.drafts("", 200).size(), 1U);
}

TEST_F(ResearchProjects, PendingStandardsAreExploratoryAndEmptyQuestionCannotBecomeFixedPlan)
{
    research::Projects write(*tasks, owner, sqlite::Database::Access::writer);
    auto request = save();
    request.mutable_save()->mutable_definition()->clear_support();
    auto draft = write.apply(request).draft();
    EXPECT_TRUE(write.apply(fix(draft)).plan().exploratory());
    request = save("save-empty", draft.digest());
    request.mutable_save()->mutable_definition()->clear_question();
    draft = write.apply(request).draft();
    EXPECT_THROW(write.apply(fix(draft, "not-a-plan")), std::invalid_argument);
    EXPECT_TRUE(write.draft(draft.id())->definition().question().empty());
}

TEST_F(ResearchProjects, ExactPlanBindingsRejectOtherOwnerProjectAndDefinition)
{
    research::Projects write(*tasks, owner, sqlite::Database::Access::writer);
    const auto fixed = write.apply(fix(write.apply(save()).draft())).plan();
    research::proto::ProjectBinding binding;
    *binding.mutable_owner() = owner;
    binding.set_project_id(fixed.project_id());
    binding.set_plan_id(fixed.id());
    binding.set_definition_digest(fixed.definition_digest());
    EXPECT_NO_THROW(write.verify_binding(binding));
    auto bad = binding;
    bad.set_definition_digest(std::string(64, 'f'));
    EXPECT_THROW(write.verify_binding(bad), Error);
    bad = binding;
    bad.set_project_id("other-question");
    EXPECT_THROW(write.verify_binding(bad), Error);
    bad = binding;
    bad.mutable_owner()->set_node_id("other-node");
    EXPECT_THROW(write.verify_binding(bad), Error);
    auto other = owner;
    other.set_service_id("other-research");
    EXPECT_THROW(research::Projects(*tasks, other), Error);
}

TEST_F(ResearchProjects, PartialSchemaIsRejectedWithoutRecreatingMissingAuthority)
{
    {
        research::Projects write(*tasks, owner, sqlite::Database::Access::writer);
        (void)write.apply(save());
    }
    tasks.reset();
    {
        sqlite::Database db(root / "tasks.sqlite");
        db.execute("DROP TABLE research_project_commands");
    }
    tasks = std::make_unique<tasks::Store>(
        asterion::research::task_domain(),
        root,
        tasks::Identity{"research-one"});
    EXPECT_THROW(
        research::Projects(*tasks, owner, sqlite::Database::Access::writer),
        std::invalid_argument);
    tasks.reset();
    sqlite::Database db(root / "tasks.sqlite", sqlite::Database::Access::read_only);
    sqlite::Database::Statement missing(
        db,
        "SELECT count(*) FROM sqlite_master WHERE name='research_project_commands'");
    ASSERT_TRUE(missing.step());
    EXPECT_EQ(missing.integer(0), 0);
}

TEST_F(ResearchProjects, OversizedRevisionCannotCommitAnUnreadablePlanOrReceipt)
{
    research::Projects write(*tasks, owner, sqlite::Database::Access::writer);
    auto request = save();
    request.mutable_save()->mutable_definition()->set_notes(std::string(60000, 'n'));
    const auto draft = write.apply(request).draft();
    const auto first = write.apply(fix(draft)).plan();
    auto revise = fix(draft, "plan-two", first.id(), std::string(80000, 'r'));
    EXPECT_THROW(write.apply(revise), std::invalid_argument);
    EXPECT_EQ(write.plans(draft.id(), "", 200).size(), 1U);
    EXPECT_EQ(write.plan(first.id()).SerializeAsString(), first.SerializeAsString());
    revise.mutable_fix()->set_revision_reason("Bounded reason preserving the original plan");
    const auto accepted = write.apply(revise);
    EXPECT_EQ(write.apply(revise).SerializeAsString(), accepted.SerializeAsString());
    EXPECT_EQ(write.plans("", "", 200).size(), 2U);
}

TEST_F(
    ResearchProjects,
    NativeProjectBoundaryKeepsOriginalDefinitionsAndRejectsWrongOwnerSubjectAndCursor)
{
    research::Projects write(*tasks, owner, sqlite::Database::Access::writer);
    const auto command = save();
    const auto reply = write.apply(command);
    research::proto::ProjectRequest request;
    *request.mutable_mutation() = command;
    const auto decoded = protocol::decode_project_reply(reply, request, owner);
    EXPECT_EQ(
        protocol::encode_project_definition(decoded.at("draft").at("definition"))
            .SerializeAsString(),
        command.save().definition().SerializeAsString());
    EXPECT_EQ(decoded.at("draft").at("digest"), reply.draft().digest());
    auto other = owner;
    other.set_node_id("other-node");
    EXPECT_THROW(protocol::decode_project_reply(reply, request, other), Error);
    research::proto::ProjectRequest wrong;
    wrong.mutable_draft()->set_id("different-project");
    EXPECT_THROW(protocol::decode_project_reply(reply, wrong, owner), Error);
    research::proto::ProjectRequest query;
    query.mutable_drafts()->set_limit(1);
    research::proto::ProjectReply page;
    *page.add_drafts() = reply.draft();
    EXPECT_EQ(protocol::decode_project_reply(page, query, owner).at("drafts").size(), 1U);
    page.set_next_after("unrelated-cursor");
    EXPECT_THROW(protocol::decode_project_reply(page, query, owner), Error);
}
