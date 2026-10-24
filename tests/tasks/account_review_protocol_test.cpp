#include <asterion/protocol/account_review.hpp>
#include <asterion/protocol/trading_facts.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <gtest/gtest.h>
using namespace asterion;

namespace {

protocol::proto::TradingFactsStatus evidence()
{
    protocol::proto::TradingFactsStatus status;
    auto& capture = *status.mutable_capture();
    auto& f = *capture.mutable_facts();
    f.mutable_source()->set_node_id("fixture-node");
    f.mutable_source()->set_service_id("trading-one");
    f.mutable_source()->set_domain("trading");
    f.set_account_id("account-one");
    f.set_environment(protocol::proto::BROKER_SIMULATION);
    f.set_instance("original-process");
    f.set_id("fixed-one");
    f.set_captured_at_ms(1790000000000);
    f.set_policy_revision("policy");
    f.set_risk_artifact(std::string(64, 'b'));
    f.set_phase("disconnected");
    *capture.mutable_reference()->mutable_owner() = f.source();
    capture.mutable_reference()->set_kind("trading_facts");
    capture.mutable_reference()->set_id(f.id());
    capture.mutable_reference()->set_version(sha256_bytes(f.SerializeAsString()));
    *status.mutable_operation()->mutable_reference() = capture.reference();
    status.mutable_operation()->set_state(protocol::proto::TradingFactsReceipt::PUBLISHED);
    status.mutable_operation()->set_admitted_at_ms(1790000000000);
    status.mutable_operation()->set_published_at_ms(1790000000001);
    return status;
}

resource::proto::Owner owner()
{
    resource::proto::Owner value;
    value.set_node_id("fixture-node");
    value.set_service_id("research-one");
    value.set_domain("research");
    return value;
}

research::proto::AccountReviewRequest request(const protocol::proto::TradingFactsStatus& value)
{
    research::proto::AccountReviewRequest q;
    *q.mutable_source() = value.operation().reference();
    q.set_account_id("account-one");
    q.set_environment(protocol::proto::BROKER_SIMULATION);
    return q;
}

research::proto::AccountReviewResult result(const research::proto::AccountReviewInput& input)
{
    research::proto::AccountReviewResult output;
    *output.mutable_owner() = input.owner();
    *output.mutable_source() = input.evidence().operation().reference();
    output.set_account_id("account-one");
    output.set_environment(protocol::proto::BROKER_SIMULATION);
    output.set_program_artifact(input.program_artifact());
    output.set_conclusion(research::proto::AccountReviewResult::EVIDENCE_INSUFFICIENT);
    for (const auto* missing :
        {"complete_broker_history",
            "cashflow_history",
            "settlement_evidence",
            "valuation_history",
            "component_business_times",
            "funds_currency"})
        output.add_limitations(missing);
    return output;
}

} // namespace

TEST(AccountReviewProtocol, OnlyTheExactPublishedOwnerVersionAccountAndEnvironmentBecomeFixedInput)
{
    const auto source = evidence();
    auto q = request(source);
    const auto input =
        protocol::make_account_review_input(owner(), q, source, std::string(64, 'a'));
    EXPECT_EQ(input.evidence().SerializeAsString(), source.SerializeAsString());
    EXPECT_EQ(
        protocol::account_review_execution(input).request().SerializeAsString(),
        q.SerializeAsString());
    q.set_environment(protocol::proto::REAL);
    EXPECT_THROW(
        protocol::make_account_review_input(owner(), q, source, std::string(64, 'a')),
        std::invalid_argument);
    q = request(source);
    q.set_account_id("another-account");
    EXPECT_THROW(
        protocol::make_account_review_input(owner(), q, source, std::string(64, 'a')),
        std::invalid_argument);
    q = request(source);
    q.mutable_source()->set_version(std::string(64, 'c'));
    EXPECT_THROW(
        protocol::make_account_review_input(owner(), q, source, std::string(64, 'a')),
        std::invalid_argument);
    auto pending = source;
    pending.clear_capture();
    pending.mutable_operation()->set_state(protocol::proto::TradingFactsReceipt::ADMITTED);
    pending.mutable_operation()->set_published_at_ms(0);
    EXPECT_THROW(
        protocol::make_account_review_input(
            owner(),
            request(source),
            pending,
            std::string(64, 'a')),
        std::invalid_argument);
}

TEST(AccountReviewProtocol, FixedInputCannotClaimTheWrongResearchOwnerOrAnUnknownProgram)
{
    const auto source = evidence();
    auto wrong = owner();
    wrong.set_domain("data");
    EXPECT_THROW(
        protocol::make_account_review_input(wrong, request(source), source, std::string(64, 'a')),
        std::invalid_argument);
    EXPECT_THROW(
        protocol::make_account_review_input(owner(), request(source), source, "unfixed"),
        std::invalid_argument);
    auto corrupted = source;
    corrupted.mutable_capture()->mutable_facts()->set_account_id("changed-without-new-version");
    EXPECT_THROW(
        protocol::make_account_review_input(
            owner(),
            request(source),
            corrupted,
            std::string(64, 'a')),
        std::invalid_argument);
}

TEST(AccountReviewProtocol, ResultCannotInventFundsCountsOrHideHistoricalEvidenceGaps)
{
    const auto source = evidence();
    const auto input =
        protocol::make_account_review_input(owner(), request(source), source, std::string(64, 'a'));
    const auto valid = result(input);
    EXPECT_NO_THROW(protocol::validate_account_review_result(input, valid));
    const auto decoded = protocol::decode_account_review_result(input, valid);
    EXPECT_EQ(decoded.at("conclusion"), "evidence_insufficient");
    EXPECT_FALSE(decoded.at("funds_present").get<bool>());
    auto invalid = valid;
    invalid.set_funds_present(true);
    EXPECT_THROW(protocol::validate_account_review_result(input, invalid), std::invalid_argument);
    invalid = valid;
    invalid.set_trade_records(1);
    EXPECT_THROW(protocol::validate_account_review_result(input, invalid), std::invalid_argument);
    invalid = valid;
    invalid.clear_limitations();
    EXPECT_THROW(protocol::validate_account_review_result(input, invalid), std::invalid_argument);
    invalid = valid;
    invalid.set_program_artifact(std::string(64, 'c'));
    EXPECT_THROW(protocol::validate_account_review_result(input, invalid), std::invalid_argument);
}
