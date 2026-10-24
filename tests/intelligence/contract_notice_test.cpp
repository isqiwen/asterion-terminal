#include "contract_notice.hpp"
#include <asterion/protocol/intelligence.hpp>
#include <algorithm>
#include <asterion/foundation/error.hpp>
#include <gtest/gtest.h>
using namespace asterion;
using namespace asterion::intelligence;

namespace {

const std::string original = "SHFE/rb/2026-10 计划于2026-10-24将保证金率调整为12%。";
const std::vector<std::string> contracts{"SHFE/rb/2026-10", "SHFE/cu/2026-10"};

EvidenceSpan quote(std::string_view text)
{
    const auto begin = original.find(text);
    if (begin == std::string::npos)
        throw std::runtime_error("missing fixture quote");
    return {begin, begin + text.size(), std::string(text)};
}

ContractNoticeEvidence evidence()
{
    ContractNoticeEvidence e;
    e.original = original;
    e.contract = contracts[0];
    e.list_date = "2025-10-01";
    e.delist_date = "2026-10-31";
    e.dictionary = std::span<const std::string>(contracts);
    e.subject = quote(e.contract);
    e.parameter = quote("保证金率");
    e.effective_day = quote("2026-10-24");
    e.percentage = quote("12%");
    e.modality = quote("计划");
    e.rule_id = "contract_margin_announcement";
    e.use_scope = "research";
    e.source_use_scope = "research";
    e.use = NoticeUse::post_hoc_research;
    e.acquired_at_ms = 1000;
    e.source_published_at_ms = 900;
    return e;
}

const NoticeCheck& field(const NoticeAssessment& result, const std::string& name)
{
    for (const auto& check : result.checks)
        if (check.field == name)
            return check;
    throw std::runtime_error("missing assessment field");
}

} // namespace

TEST(NoticeVerification, ExactEvidenceProposesDayAndRatioButAlwaysRequiresHumanContextReview)
{
    const auto result = assess_contract_notice(evidence());
    EXPECT_TRUE(result.ready_for_human_review);
    ASSERT_TRUE(result.margin_ratio);
    EXPECT_EQ(*result.margin_ratio, Decimal::parse("0.12"));
    EXPECT_EQ(result.effective_day, "2026-10-24");
    EXPECT_TRUE(result.planned_marker_present);
    EXPECT_NE(
        std::ranges::find(result.limitations, "notice_semantic_context_requires_human_review"),
        result.limitations.end());
}

TEST(NoticeVerification, MissingOrAmbiguousDictionaryNeverChoosesFirstContract)
{
    auto e = evidence();
    e.dictionary.reset();
    auto result = assess_contract_notice(e);
    EXPECT_FALSE(result.ready_for_human_review);
    EXPECT_EQ(field(result, "subject_mapping").state, CheckState::unknown);
    const std::vector<std::string> ambiguous{e.contract, e.contract};
    e.dictionary = std::span<const std::string>(ambiguous);
    result = assess_contract_notice(e);
    EXPECT_FALSE(result.ready_for_human_review);
    EXPECT_EQ(field(result, "subject_mapping").reason, "notice_contract_mapping_ambiguous");
}

TEST(NoticeVerification, LiteralEvidenceAndSourceUseCannotBeRepairedByClickingApproval)
{
    auto e = evidence();
    ++e.parameter.begin;
    auto result = assess_contract_notice(e);
    EXPECT_FALSE(result.ready_for_human_review);
    EXPECT_EQ(field(result, "parameter_evidence").state, CheckState::blocked);
    e = evidence();
    e.use_scope = "trading";
    result = assess_contract_notice(e);
    EXPECT_FALSE(result.ready_for_human_review);
    EXPECT_EQ(field(result, "use_scope").state, CheckState::blocked);
    std::string relative = original;
    relative.replace(relative.find("2026-10-24"), 10, "next month");
    e = evidence();
    e.original = relative;
    e.effective_day.text = "next month";
    result = assess_contract_notice(e);
    EXPECT_FALSE(result.ready_for_human_review);
    EXPECT_EQ(field(result, "effective_day").reason, "notice_effective_day_not_literal");
    e = evidence();
    e.delist_date = "2026-10-15";
    EXPECT_FALSE(assess_contract_notice(e).ready_for_human_review);
    e = evidence();
    e.percentage.end = original.size() + 1;
    result = assess_contract_notice(e);
    EXPECT_FALSE(result.ready_for_human_review);
    EXPECT_EQ(field(result, "percentage_evidence").state, CheckState::blocked);
}

TEST(NoticeVerification, UnknownPublicationTimeCannotEnterStrictHistoryAndCancellationIsBounded)
{
    auto e = evidence();
    e.source_published_at_ms.reset();
    e.use = NoticeUse::strict_history;
    auto strict = assess_contract_notice(e);
    EXPECT_FALSE(strict.ready_for_human_review);
    EXPECT_EQ(
        field(strict, "availability_mode").reason,
        "notice_strict_history_requires_source_time");
    e.use = NoticeUse::post_hoc_research;
    const auto post = assess_contract_notice(e);
    EXPECT_TRUE(post.ready_for_human_review);
    EXPECT_NE(
        std::ranges::find(post.limitations, "notice_source_publication_time_unknown"),
        post.limitations.end());
    e.use = NoticeUse::strict_history;
    e.source_published_at_ms = 2000;
    EXPECT_FALSE(assess_contract_notice(e).ready_for_human_review);
    std::stop_source stop;
    stop.request_stop();
    EXPECT_THROW(assess_contract_notice(e, stop.get_token()), Error);
}

TEST(NoticeVerification, CandidateShapeKeepsIncompleteDraftsButRejectsDifferentSourceObjectKinds)
{
    proto::NoticeCandidate c;
    c.set_id("draft-one");
    auto& ref = *c.mutable_material();
    ref.mutable_owner()->set_node_id("fixture-node");
    ref.mutable_owner()->set_service_id("data-one");
    ref.mutable_owner()->set_domain("data");
    ref.set_kind("material_acquisition");
    ref.set_id("material-one");
    ref.set_version(std::string(64, 'a'));
    EXPECT_NO_THROW(protocol::validate_notice_candidate(c));
    c.mutable_material()->set_kind("material_content");
    EXPECT_THROW(protocol::validate_notice_candidate(c), std::invalid_argument);
}
