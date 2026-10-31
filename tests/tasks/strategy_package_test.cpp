#include <asterion/protocol/research_strategy.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/base/error.hpp>
#include "data/bar_fixture.hpp"
#include <gtest/gtest.h>
using namespace asterion;

namespace {

backtest::proto::BacktestInput source_input()
{
    backtest::proto::BacktestInput value;
    std::vector<MarketBar> rows;
    for (int i = 0; i < 10; ++i)
        rows.push_back(
            test::flat(
                "2026-09-25",
                1790298000000000123LL + static_cast<std::int64_t>(i) * 60000000000LL,
                "100"));
    *value.mutable_paper()->add_contracts()->mutable_dataset() = test::dataset(rows);
    *value.add_strategies() = protocol::encode_strategy(
        {{"quantity", "1"}, {"sides", "both"}, {"rule", {{"kind", "momentum"}, {"lookback", 2}}}});
    return value;
}

research::proto::StrategyPackage package()
{
    research::proto::StrategyPackage value;
    value.set_id("candidate-one");
    auto& owner = *value.mutable_owner();
    owner.set_domain("research");
    owner.set_node_id("fixture-node");
    owner.set_service_id("research-one");
    *value.mutable_definition() =
        protocol::strategy_definition(source_input(), std::string(64, 'a'));
    value.set_digest(protocol::strategy_digest(value.definition()));
    value.set_name("Synthetic CTA");
    value.set_purpose("Fixed reference candidate");
    value.set_limitations("Synthetic reference evidence; predictive validity unproven");
    auto& source = *value.mutable_reference_result();
    *source.mutable_owner() = owner;
    source.set_kind("backtest_result");
    source.set_id("source-one");
    source.set_version(std::string(64, 'c'));
    value.set_source_input_sha256(std::string(64, 'd'));
    value.set_source_attempt(1);
    value.set_recorded_at_ms(1);
    return value;
}

} // namespace

TEST(StrategyPackages, CurrentDefinitionCopiesActualRuleAndUnitsWithoutMarketPayload)
{
    const auto input = source_input();
    const auto original = input.SerializeAsString();
    const auto value = protocol::strategy_definition(input, std::string(64, 'a'));
    EXPECT_EQ(value.strategy().SerializeAsString(), input.strategies(0).SerializeAsString());
    EXPECT_EQ(
        value.contract().SerializeAsString(),
        input.paper().contracts(0).dataset().contract().SerializeAsString());
    EXPECT_EQ(value.interval_minutes(), 1U);
    EXPECT_LT(value.ByteSizeLong(), input.ByteSizeLong());
    EXPECT_EQ(input.SerializeAsString(), original);
    auto search = input;
    *search.add_strategies() = input.strategies(0);
    EXPECT_THROW(
        protocol::strategy_definition(search, std::string(64, 'a')),
        std::invalid_argument);
    search = input;
    search.mutable_walk_forward()->set_training_days(10);
    EXPECT_THROW(
        protocol::strategy_definition(search, std::string(64, 'a')),
        std::invalid_argument);
    EXPECT_THROW(protocol::strategy_definition(input, ""), std::invalid_argument);
}

TEST(StrategyPackages, PortfolioDefinitionFixesEveryMemberWithoutAnIgnoredSingleContract)
{
    auto input = source_input();
    input.mutable_paper()->mutable_contracts(0)->mutable_dataset()->set_interval_minutes(0);
    auto* second = input.mutable_paper()->add_contracts();
    *second = input.paper().contracts(0);
    second->mutable_dataset()->mutable_contract()->set_symbol("hc2610");
    second->mutable_dataset()->mutable_contract()->set_product("hc");
    const Json weights = Json::array(
        {Json{{"venue", "SHFE"}, {"symbol", "rb2610"}, {"weight", "0.4"}},
            Json{{"venue", "SHFE"}, {"symbol", "hc2610"}, {"weight", "-0.2"}}});
    const Json policy{
        {"kind", "fixed_weights"},
        {"rebalance", 2},
        {"gross_weight_limit", "0.8"},
        {"net_weight_limit", "0.3"},
        {"members", weights}
    };
    *input.mutable_strategies(0) =
        protocol::encode_strategy(Json{{"sides", "both"}, {"rule", policy}});
    const auto original = input.SerializeAsString();
    const auto definition = protocol::strategy_definition(input, std::string(64, 'a'));
    ASSERT_TRUE(definition.has_portfolio());
    EXPECT_FALSE(definition.has_contract());
    EXPECT_FALSE(definition.has_strategy());
    ASSERT_EQ(definition.portfolio().contracts_size(), 2);
    EXPECT_EQ(
        definition.portfolio().contracts(1).SerializeAsString(),
        input.paper().contracts(1).dataset().contract().SerializeAsString());
    EXPECT_EQ(
        definition.portfolio().strategy().SerializeAsString(),
        input.strategies(0).SerializeAsString());
    EXPECT_LT(definition.ByteSizeLong(), input.ByteSizeLong());
    EXPECT_EQ(input.SerializeAsString(), original);
    auto candidate = package();
    *candidate.mutable_definition() = definition;
    candidate.set_digest(protocol::strategy_digest(definition));
    const auto shown = protocol::decode_strategy_package(candidate).at("definition");
    EXPECT_FALSE(shown.contains("contract"));
    EXPECT_FALSE(shown.contains("strategy"));
    ASSERT_EQ(shown.at("portfolio").at("contracts").size(), 2);
    EXPECT_EQ(shown.at("portfolio").at("strategy").at("rule").at("kind"), "fixed_weights");
    auto bad = definition;
    *bad.mutable_contract() = input.paper().contracts(0).dataset().contract();
    EXPECT_THROW(protocol::validate_strategy_definition(bad), std::invalid_argument);
    bad = definition;
    bad.mutable_portfolio()->mutable_contracts()->RemoveLast();
    EXPECT_THROW(protocol::validate_strategy_definition(bad), std::invalid_argument);
    bad = definition;
    bad.mutable_portfolio()->mutable_contracts(1)->set_symbol("other");
    EXPECT_THROW(protocol::validate_strategy_definition(bad), std::invalid_argument);
    auto wrong_source = input;
    wrong_source.mutable_paper()->mutable_contracts(1)->mutable_dataset()->set_interval_minutes(1);
    EXPECT_THROW(
        protocol::strategy_definition(wrong_source, std::string(64, 'a')),
        std::invalid_argument);
}

TEST(StrategyPackages, NameAndDeprecationCannotChangeSemanticIdentityOrTurnReferenceIntoValidation)
{
    const auto original = package();
    ASSERT_NO_THROW(protocol::validate_strategy_package(original));
    auto renamed = original;
    renamed.set_name("Another display name");
    renamed.set_retirement_reason("No longer selected for new runs");
    EXPECT_EQ(renamed.digest(), original.digest());
    EXPECT_NO_THROW(protocol::validate_strategy_package(renamed));
    const auto json = protocol::decode_strategy_package(renamed);
    EXPECT_EQ(json.at("evidence_relation"), "reference_only");
    EXPECT_EQ(json.at("retirement_reason"), renamed.retirement_reason());
    auto changed = original;
    changed.mutable_definition()->mutable_strategy()->mutable_momentum()->set_lookback(3);
    EXPECT_NE(protocol::strategy_digest(changed.definition()), original.digest());
    EXPECT_THROW(protocol::validate_strategy_package(changed), std::invalid_argument);
    auto wrong_owner = original;
    wrong_owner.mutable_reference_result()->mutable_owner()->set_service_id("other-research");
    EXPECT_THROW(protocol::validate_strategy_package(wrong_owner), std::invalid_argument);
}

TEST(StrategyPackages, DurableDeliveryFixesTargetAndOwnerButDoesNotPretendToAcceptATask)
{
    const auto fixed = package();
    research::proto::StrategyDelivery delivery;
    delivery.set_id("delivery-one");
    *delivery.mutable_owner() = fixed.owner();
    *delivery.mutable_package() = protocol::strategy_package_object(fixed);
    delivery.set_target_task_id("target-one");
    *delivery.mutable_reference_result() = fixed.reference_result();
    delivery.set_source_input_sha256(fixed.source_input_sha256());
    delivery.set_recorded_at_ms(1);
    delivery.set_digest(protocol::strategy_delivery_digest(delivery));
    ASSERT_NO_THROW(protocol::validate_strategy_delivery(delivery));
    auto later = delivery;
    later.set_recorded_at_ms(2);
    EXPECT_EQ(protocol::strategy_delivery_digest(later), delivery.digest());
    auto changed = delivery;
    changed.set_target_task_id("different-target");
    EXPECT_THROW(protocol::validate_strategy_delivery(changed), std::invalid_argument);
    research::proto::StrategyUse use;
    *use.mutable_package() = delivery.package();
    *use.mutable_delivery() = protocol::strategy_delivery_object(delivery);
    EXPECT_NO_THROW(protocol::validate_strategy_use(use));
    use.mutable_delivery()->mutable_owner()->set_node_id("other-node");
    EXPECT_THROW(protocol::validate_strategy_use(use), std::invalid_argument);
    EXPECT_FALSE(protocol::decode_strategy_delivery(delivery).contains("succeeded"));
}

TEST(StrategyPackages, ConsumerReplyBindsOwnerRequestAndOrderedPageWithoutInventingAdmission)
{
    const auto fixed = package();
    research::proto::StrategyRequest query;
    query.mutable_get()->set_id(fixed.id());
    research::proto::StrategyReply reply;
    *reply.mutable_package() = fixed;
    EXPECT_EQ(
        protocol::decode_strategy_reply(reply, query, fixed.owner())
            .at("package")
            .at("evidence_relation"),
        "reference_only");
    EXPECT_FALSE(
        protocol::decode_strategy_reply(reply, query, fixed.owner()).contains("target_accepted"));
    query.mutable_get()->set_id("different");
    EXPECT_THROW(
        protocol::decode_strategy_reply(reply, query, fixed.owner()),
        std::invalid_argument);
    query.mutable_get()->set_id(fixed.id());
    reply.set_target_accepted(true);
    EXPECT_THROW(
        protocol::decode_strategy_reply(reply, query, fixed.owner()),
        std::invalid_argument);
    reply.set_target_accepted(false);
    auto other = fixed.owner();
    other.set_node_id("other-node");
    EXPECT_THROW(protocol::decode_strategy_reply(reply, query, other), std::invalid_argument);
    query.Clear();
    query.mutable_list()->set_limit(2);
    reply.Clear();
    *reply.add_packages() = fixed;
    EXPECT_EQ(
        protocol::decode_strategy_reply(reply, query, fixed.owner()).at("packages").size(),
        1U);
    *reply.add_packages() = fixed;
    EXPECT_THROW(
        protocol::decode_strategy_reply(reply, query, fixed.owner()),
        std::invalid_argument);
}

TEST(StrategyPackages, FixedNewBacktestIntentSurvivesUseManifestRoundTrip)
{
    const auto candidate = package();
    research::proto::StrategyUse use;
    *use.mutable_package() = protocol::strategy_package_object(candidate);
    *use.mutable_delivery() = use.package();
    use.mutable_delivery()->set_kind("strategy_delivery");
    use.set_request_sha256(std::string(64, 'e'));
    const auto encoded = protocol::decode_strategy_use(use);
    EXPECT_EQ(protocol::encode_strategy_use(encoded).SerializeAsString(), use.SerializeAsString());
    auto invalid = encoded;
    invalid["request_sha256"] = "not-an-identity";
    EXPECT_THROW(protocol::encode_strategy_use(invalid), std::invalid_argument);
    invalid = encoded;
    invalid["unknown"] = true;
    EXPECT_THROW(protocol::encode_strategy_use(invalid), Error);
}
