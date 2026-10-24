#include "research_logic.hpp"
#include <asterion/protocol/research_logic.hpp>
#include <asterion/foundation/decimal.hpp>
#include <asterion/foundation/error.hpp>
#include <gtest/gtest.h>
using namespace asterion;

namespace {

research::proto::LogicTrialInput example()
{
    research::proto::LogicTrialInput input;
    input.mutable_owner()->set_node_id("fixture-node");
    input.mutable_owner()->set_service_id("research-one");
    input.mutable_owner()->set_domain("research");
    auto& definition = *input.mutable_definition();
    definition.set_id("logic");
    *definition.mutable_owner() = input.owner();
    definition.set_name("Synthetic means");
    definition.set_purpose("Fixed logic only, no market performance");
    definition.set_fast(1);
    definition.set_slow(3);
    definition.set_input(research::proto::LogicDefinition::COMPLETED_CLOSE);
    definition.set_output(research::proto::LogicDefinition::TARGET_SIGN);
    definition.set_recorded_at_ms(1);
    definition.set_digest(protocol::logic_digest(definition));
    auto& samples = *input.mutable_example();
    samples.set_id("example");
    *samples.mutable_owner() = input.owner();
    *samples.mutable_definition() = protocol::logic_object(definition);
    samples.set_name("Four fictional complete closes");
    samples.set_use_scope("Fictional logic expectations, no financial validation");
    samples.set_fictional(true);
    for (const auto* price : {"10", "10", "13", "7"}) {
        const auto index = samples.observations_size();
        auto& row = *samples.add_observations();
        row.set_closed_at_ns((index + 1) * 60000000000LL);
        row.set_available_at_ns(row.closed_at_ns());
        row.mutable_close()->set_units(Decimal::parse(price).raw());
        if (index < 2)
            row.set_warmup(true);
        else
            row.set_target_sign(index == 2 ? 1 : -1);
    }
    samples.set_recorded_at_ms(2);
    samples.set_digest(protocol::logic_example_digest(samples));
    return input;
}

void seal_example(research::proto::LogicTrialInput& input)
{
    input.mutable_example()->set_digest(protocol::logic_example_digest(input.example()));
}

} // namespace

TEST(ResearchLogic, OriginalDefinitionAndPriorExpectationsProduceActualRowsWithoutMarketData)
{
    const auto fixed = example();
    const auto before = fixed.SerializeAsString();
    unsigned completed = 0;
    const auto result = tasks::compute_logic_trial(fixed, {}, [&](unsigned done, unsigned total) {
        EXPECT_EQ(total, 4U);
        EXPECT_EQ(done, completed + 1);
        completed = done;
    });
    ASSERT_NO_THROW(protocol::validate_logic_trial_result(fixed, result));
    ASSERT_EQ(result.rows_size(), 4);
    EXPECT_EQ(completed, 4U);
    EXPECT_TRUE(result.all_match());
    EXPECT_EQ(result.matched(), 4U);
    EXPECT_EQ(result.decisions(), 2U);
    EXPECT_TRUE(result.rows(0).warmup());
    EXPECT_TRUE(result.rows(1).warmup());
    EXPECT_EQ(result.rows(2).target_sign(), 1);
    EXPECT_EQ(result.rows(3).target_sign(), -1);
    EXPECT_DOUBLE_EQ(result.rows(2).fast_mean(), 13);
    EXPECT_DOUBLE_EQ(result.rows(2).slow_mean(), 11);
    EXPECT_DOUBLE_EQ(result.rows(2).difference(), 2);
    EXPECT_EQ(fixed.SerializeAsString(), before);
    auto later = fixed;
    later.mutable_example()->mutable_observations(3)->mutable_close()->set_units(
        Decimal::parse("70").raw());
    seal_example(later);
    const auto changed = tasks::compute_logic_trial(later);
    for (int i = 0; i < 3; ++i)
        EXPECT_EQ(changed.rows(i).SerializeAsString(), result.rows(i).SerializeAsString());
    EXPECT_FALSE(changed.all_match());
    EXPECT_EQ(changed.matched(), 3U);
    EXPECT_EQ(changed.rows(3).target_sign(), 1);
}

TEST(
    ResearchLogic,
    ZeroAndNegativeInputsRemainExplicitAndIncorrectExpectationsAreNotExecutionFailure)
{
    auto fixed = example();
    for (auto& row : *fixed.mutable_example()->mutable_observations())
        row.mutable_close()->set_units(0);
    fixed.mutable_example()->mutable_observations(2)->set_target_sign(0);
    fixed.mutable_example()->mutable_observations(3)->set_target_sign(-1);
    seal_example(fixed);
    const auto result = tasks::compute_logic_trial(fixed);
    ASSERT_NO_THROW(protocol::validate_logic_trial_result(fixed, result));
    EXPECT_TRUE(result.rows(2).has_target_sign());
    EXPECT_EQ(result.rows(2).target_sign(), 0);
    EXPECT_FALSE(result.rows(2).has_warmup());
    EXPECT_FALSE(result.all_match());
    EXPECT_EQ(result.matched(), 3U);
    EXPECT_TRUE(protocol::decode_logic_trial_result(fixed, result).at("fictional"));
    fixed.mutable_example()->mutable_observations(3)->mutable_close()->set_units(
        Decimal::parse("-1").raw());
    seal_example(fixed);
    EXPECT_EQ(tasks::compute_logic_trial(fixed).rows(3).target_sign(), -1);
}

TEST(ResearchLogic, MissingUnknownAndFutureObservationsCannotBeAcceptedAsCompleteInputs)
{
    const auto original = example();
    auto invalid = original;
    invalid.mutable_example()->mutable_observations(2)->clear_close();
    seal_example(invalid);
    EXPECT_THROW(protocol::validate_logic_trial_input(invalid), std::invalid_argument);
    invalid = original;
    auto* delayed = invalid.mutable_example()->mutable_observations(2);
    delayed->set_available_at_ns(delayed->closed_at_ns() + 1);
    seal_example(invalid);
    EXPECT_THROW(protocol::validate_logic_trial_input(invalid), std::invalid_argument);
    invalid = original;
    invalid.mutable_example()->mutable_observations(2)->clear_expected();
    seal_example(invalid);
    EXPECT_THROW(protocol::validate_logic_trial_input(invalid), std::invalid_argument);
    invalid = original;
    invalid.mutable_example()->mutable_observations(2)->set_target_sign(2);
    seal_example(invalid);
    EXPECT_THROW(protocol::validate_logic_trial_input(invalid), std::invalid_argument);
    invalid = original;
    invalid.mutable_definition()->set_output(research::proto::LogicDefinition::OUTPUT_UNSPECIFIED);
    invalid.mutable_definition()->set_digest(protocol::logic_digest(invalid.definition()));
    EXPECT_THROW(protocol::validate_logic_trial_input(invalid), std::invalid_argument);
    invalid = original;
    while (invalid.example().observations_size() <= 512)
        *invalid.mutable_example()->add_observations() = original.example().observations(0);
    seal_example(invalid);
    EXPECT_THROW(protocol::validate_logic_trial_input(invalid), std::invalid_argument);
}

TEST(ResearchLogic, ContentIdentitySeparatesMeaningFromLabelsAndPreservesFixedCaseReferences)
{
    const auto original = example();
    auto renamed = original.definition();
    renamed.set_name("Display change");
    renamed.set_retirement_reason("No new trials");
    EXPECT_EQ(protocol::logic_digest(renamed), original.definition().digest());
    EXPECT_NO_THROW(protocol::validate_logic_definition(renamed));
    auto changed = original;
    changed.mutable_definition()->set_fast(2);
    changed.mutable_definition()->set_digest(protocol::logic_digest(changed.definition()));
    EXPECT_NE(changed.definition().digest(), original.definition().digest());
    EXPECT_THROW(protocol::validate_logic_trial_input(changed), std::invalid_argument);
    auto tampered = tasks::compute_logic_trial(original);
    tampered.mutable_rows(2)->set_matches(false);
    EXPECT_THROW(protocol::validate_logic_trial_result(original, tampered), std::invalid_argument);
    tampered = tasks::compute_logic_trial(original);
    tampered.mutable_rows(2)->set_target_sign(2);
    EXPECT_THROW(protocol::validate_logic_trial_result(original, tampered), std::invalid_argument);
}

TEST(ResearchLogic, CooperativeCancellationCannotTurnAPartialCalculationIntoACompleteReport)
{
    std::stop_source stop;
    unsigned observed = 0;
    EXPECT_THROW(
        tasks::compute_logic_trial(
            example(),
            stop.get_token(),
            [&](unsigned completed, unsigned) {
                observed = completed;
                if (completed == 2)
                    stop.request_stop();
            }),
        Error);
    EXPECT_EQ(observed, 2U);
}

TEST(ResearchLogic, ReplyProjectionRejectsWrongOwnerIdentityAndPagination)
{
    const auto fixed = example();
    research::proto::LogicRequest request;
    request.mutable_get()->set_id(fixed.definition().id());
    research::proto::LogicReply reply;
    *reply.mutable_definition() = fixed.definition();
    EXPECT_EQ(
        protocol::decode_logic_reply(reply, request, fixed.owner())
            .at("definition")
            .at("object")
            .at("id"),
        fixed.definition().id());
    auto wrong = reply;
    wrong.mutable_definition()->mutable_owner()->set_service_id("other-research");
    EXPECT_THROW(
        protocol::decode_logic_reply(wrong, request, fixed.owner()),
        std::invalid_argument);
    wrong = reply;
    wrong.mutable_definition()->set_id("other-definition");
    EXPECT_THROW(
        protocol::decode_logic_reply(wrong, request, fixed.owner()),
        std::invalid_argument);
    request.mutable_examples()->set_definition_id(fixed.definition().id());
    request.mutable_examples()->set_limit(1);
    reply.Clear();
    *reply.add_examples() = fixed.example();
    reply.set_next_after(fixed.example().id());
    EXPECT_EQ(
        protocol::decode_logic_reply(reply, request, fixed.owner()).at("examples").size(),
        1U);
    reply.set_next_after("unrelated");
    EXPECT_THROW(
        protocol::decode_logic_reply(reply, request, fixed.owner()),
        std::invalid_argument);
    reply.clear_next_after();
    request.mutable_examples()->set_definition_id("other-definition");
    EXPECT_THROW(
        protocol::decode_logic_reply(reply, request, fixed.owner()),
        std::invalid_argument);
}
