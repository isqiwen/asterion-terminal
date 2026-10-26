#include "strategy_store.hpp"
#include "research_submission.hpp"
#include "data_submission.hpp"
#include "task_store.hpp"
#include "tasks/task_store_support.hpp"
#include "tasks/paper_input.hpp"
#include "program_files.hpp"
#include "engine.hpp"
#include <asterion/protocol/research_strategy.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/protocol/data.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <gtest/gtest.h>
#include <array>
using namespace asterion;
namespace fs = std::filesystem;

namespace {

struct StrategyStore : testing::Test
{
    fs::path root = fs::temp_directory_path() / ("asterion-strategy-store-" + unique_process_id());
    resource::proto::Owner owner;
    std::unique_ptr<tasks::Store> store;
    backtest::proto::BacktestInput input;
    task::proto::TaskResponse reference;

    void SetUp() override
    {
        ASSERT_TRUE(fs::create_directory(root));
        root = fs::canonical(root);
        owner.set_node_id("fixture-node");
        owner.set_service_id("research-one");
        owner.set_domain("research");
        store = std::make_unique<tasks::Store>(
            asterion::research::task_domain(),
            root,
            tasks::Identity{"research-one"});
    }

    void TearDown() override
    {
        store.reset();
        std::error_code error;
        fs::remove_all(root, error);
    }

    void seed(bool portfolio = false)
    {
        const std::array<const char*, 3> prices = {"100", "101", "102"};
        std::vector<MarketBar> bars;
        for (int i = 0; i < 10; ++i)
            bars.push_back(
                test::flat(
                    "2026-09-25",
                    1790298000000000123LL + static_cast<std::int64_t>(i) * 60000000000LL,
                    prices[static_cast<std::size_t>(i) % prices.size()]));
        input = testing_support::backtest_input(test::dataset(bars));
        if (portfolio) {
            const std::array<const char*, 6> days = {
                "2026-09-21",
                "2026-09-22",
                "2026-09-23",
                "2026-09-24",
                "2026-09-25",
                "2026-09-28"
            };
            std::vector<MarketBar> daily;
            for (std::size_t i = 0; i < days.size(); ++i)
                daily.push_back(
                    test::flat(
                        days[i],
                        1790000000000000000LL + static_cast<std::int64_t>(i) * 86400000000000LL,
                        "1000",
                        "100"));
            input = testing_support::backtest_input(test::dataset(daily, {}, test::contract(), 0));
            input.mutable_paper()->mutable_deposit()->set_units(Decimal::parse("100000").raw());
            auto* second = input.mutable_paper()->add_contracts();
            *second = input.paper().contracts(0);
            *second->mutable_dataset()->mutable_contract() =
                test::contract("SHFE", "hc2610", "hc", "2026-10");
            second->mutable_dataset()->set_revision(
                protocol::bar_dataset_revision(second->dataset()));
            input.set_dataset_revision(protocol::dataset_revision(input.paper()));
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
        }
        input.mutable_data_owner()->set_node_id(owner.node_id());
        input.mutable_data_owner()->set_service_id("fixture-data");
        const auto artifact = sha256_file(current_executable());
        (void)programs::Files(root / "programs", owner).preserve(current_executable(), artifact);
        auto submission = task_submission(*store, "reference-backtest", input);
        asterion::tasks::set_program_artifact(submission, artifact);
        tasks::submit(*store, std::move(submission));
        const auto token = store->commit(store->claim("reference-backtest")).token();
        store->commit(tasks::finish(*store, "reference-backtest", token, backtest::run(input)));
        reference = tasks::verified_result(*store, "reference-backtest");
    }

    research::proto::StrategyMutation freeze() const
    {
        research::proto::StrategyMutation req;
        req.set_id("freeze-request");
        auto& r = *req.mutable_freeze();
        r.set_id("candidate-one");
        r.set_name("Synthetic CTA candidate");
        r.set_purpose("Fixed research candidate for reviewed backtest delivery");
        r.set_limitations(
            "Synthetic evidence only; no claim of predictive validity or trading permission");
        auto& source = *r.mutable_source();
        *source.mutable_owner() = owner;
        source.set_kind("backtest_result");
        source.set_id("reference-backtest");
        source.set_version(reference.result_task().result_digest());
        return req;
    }

    research::proto::StrategyPackage capture() const
    {
        return research::capture_strategy_package(
            owner,
            freeze().freeze(),
            reference,
            sha256_file(root / "reference-backtest/input.pb"));
    }

    research::proto::StrategyMutation deliver(const research::proto::StrategyPackage& pkg) const
    {
        research::proto::StrategyMutation req;
        req.set_id("deliver-request");
        auto& r = *req.mutable_deliver();
        r.set_id("delivery-one");
        r.set_target_task_id("candidate-backtest");
        *r.mutable_package() = protocol::strategy_package_object(pkg);
        return req;
    }
};

} // namespace

TEST_F(StrategyStore, ReadOnlyDiscoveryDoesNotInitializeStrategyTablesOrPermitWrites)
{
    {
        research::Strategies read(*store, owner);
        EXPECT_TRUE(read.packages("", 10).empty());
        EXPECT_TRUE(read.deliveries("", "", 10).empty());
        EXPECT_TRUE(read.source_consumers("reference-backtest").empty());
        research::proto::StrategyMutation req;
        req.set_id("unrequested-write");
        EXPECT_THROW(read.apply(req), Error);
    }
    store.reset();
    sqlite::Database db(root / "tasks.sqlite", sqlite::Database::Access::read_only);
    sqlite::Database::Statement tables(
        db,
        "SELECT count(*) FROM sqlite_master WHERE name LIKE 'research_strategy_%'");
    ASSERT_TRUE(tables.step());
    EXPECT_EQ(tables.integer(0), 0);
}

TEST_F(StrategyStore, PortfolioPackageAndDeliveryRetainAllMembersAcrossRestartWithoutAdmittingATask)
{
    seed(true);
    const auto request = freeze();
    const auto captured = capture();
    research::proto::StrategyPackage frozen;
    research::proto::StrategyDelivery draft;
    {
        research::Strategies strategies(*store, owner, sqlite::Database::Access::writer);
        frozen = strategies.apply(request, &captured).package();
        ASSERT_TRUE(frozen.definition().has_portfolio());
        EXPECT_FALSE(frozen.definition().has_contract());
        ASSERT_EQ(frozen.definition().portfolio().contracts_size(), 2);
        EXPECT_EQ(frozen.definition().portfolio().contracts(1).symbol(), "hc2610");
        EXPECT_EQ(
            frozen.definition().portfolio().strategy().SerializeAsString(),
            input.strategies(0).SerializeAsString());
        EXPECT_EQ(
            strategies.apply(request).package().SerializeAsString(),
            frozen.SerializeAsString());
        draft = strategies.apply(deliver(frozen)).delivery();
        EXPECT_FALSE(strategies.delivery_status(draft.id()).target_accepted());
        EXPECT_FALSE(store->find_task(draft.target_task_id()));
        auto conflict = request;
        conflict.mutable_freeze()->set_name("Conflicting same operation");
        EXPECT_THROW(strategies.apply(conflict, &captured), Error);
        EXPECT_EQ(strategies.package(frozen.id()).SerializeAsString(), frozen.SerializeAsString());
    }
    store.reset();
    store = std::make_unique<tasks::Store>(
        asterion::research::task_domain(),
        root,
        tasks::Identity{"research-one"});
    {
        research::Strategies strategies(*store, owner);
        EXPECT_EQ(strategies.package(frozen.id()).SerializeAsString(), frozen.SerializeAsString());
        EXPECT_EQ(strategies.delivery(draft.id()).SerializeAsString(), draft.SerializeAsString());
        ASSERT_TRUE(strategies.receipt(request));
        EXPECT_EQ(
            strategies.receipt(request)->package().SerializeAsString(),
            frozen.SerializeAsString());
        EXPECT_FALSE(strategies.delivery_status(draft.id()).target_accepted());
        EXPECT_FALSE(store->find_task(draft.target_task_id()));
    }
}

TEST_F(StrategyStore, FrozenCandidateKeepsReferenceOnlyEvidenceAndDurableReceiptAcrossRestart)
{
    seed();
    const auto req = freeze();
    const auto captured = capture();
    research::proto::StrategyReply original;
    {
        research::Strategies write(*store, owner, sqlite::Database::Access::writer);
        original = write.apply(req, &captured);
        EXPECT_LT(original.package().ByteSizeLong(), 32768U);
        EXPECT_EQ(write.apply(req).SerializeAsString(), original.SerializeAsString());
        EXPECT_EQ(
            write.source_consumers("reference-backtest"),
            std::vector<std::string>{"strategy_package/candidate-one"});
        auto changed = req;
        changed.mutable_freeze()->set_name("Different content under one operation");
        EXPECT_THROW(write.apply(changed, &captured), Error);
        auto wrong = captured;
        wrong.mutable_reference_result()->set_version(std::string(64, 'e'));
        auto other = req;
        other.set_id("wrong-source-command");
        *other.mutable_freeze()->mutable_source() = wrong.reference_result();
        EXPECT_THROW(write.apply(other, &wrong), Error);
        EXPECT_EQ(
            protocol::decode_strategy_package(write.package("candidate-one"))
                .at("evidence_relation"),
            "reference_only");
    }
    store.reset();
    store = std::make_unique<tasks::Store>(
        asterion::research::task_domain(),
        root,
        tasks::Identity{"research-one"});
    research::Strategies recovered(*store, owner);
    ASSERT_TRUE(recovered.receipt(req));
    EXPECT_EQ(recovered.receipt(req)->SerializeAsString(), original.SerializeAsString());
    EXPECT_EQ(
        recovered.package("candidate-one").SerializeAsString(),
        original.package().SerializeAsString());
}

TEST_F(StrategyStore, DeliveryIsDurableDraftAndAConflictingTargetNeverBecomesAccepted)
{
    seed();
    research::Strategies write(*store, owner, sqlite::Database::Access::writer);
    const auto captured = capture();
    const auto pkg = write.apply(freeze(), &captured).package();
    const auto req = deliver(pkg);
    const auto draft = write.apply(req);
    EXPECT_FALSE(draft.target_accepted());
    EXPECT_EQ(store->list().tasks_size(), 1);
    EXPECT_FALSE(write.delivery_status("delivery-one").target_accepted());
    EXPECT_EQ(write.apply(req).SerializeAsString(), draft.SerializeAsString());
    research::proto::StrategyUse use;
    *use.mutable_package() = draft.delivery().package();
    *use.mutable_delivery() = protocol::strategy_delivery_object(draft.delivery());
    EXPECT_NO_THROW(write.verify_use(use, "candidate-backtest", true));
    EXPECT_THROW(write.verify_use(use, "different-target", true), Error);
    auto duplicate = req;
    duplicate.set_id("another-delivery-command");
    duplicate.mutable_deliver()->set_id("different-delivery");
    EXPECT_THROW(write.apply(duplicate), Error);
    EXPECT_EQ(write.deliveries("candidate-one", "", 10).size(), 1U);
    auto unrelated = task_submission(*store, "candidate-backtest", input);
    asterion::tasks::set_program_artifact(unrelated, reference.result_task().program_artifact());
    tasks::submit(*store, std::move(unrelated));
    EXPECT_THROW(write.delivery_status("delivery-one"), Error);
}

TEST_F(StrategyStore, DeprecationReleasesCandidateHoldButPreservesDefinitionDraftAndOriginalReceipt)
{
    seed();
    research::Strategies write(*store, owner, sqlite::Database::Access::writer);
    const auto captured = capture();
    const auto create = freeze();
    const auto original = write.apply(create, &captured);
    const auto draft = write.apply(deliver(original.package()));
    research::proto::StrategyMutation retire;
    retire.set_id("deprecate-request");
    *retire.mutable_retire()->mutable_package() =
        protocol::strategy_package_object(original.package());
    retire.mutable_retire()->set_reason("No longer used for new research submissions");
    const auto retired = write.apply(retire);
    EXPECT_EQ(retired.package().digest(), original.package().digest());
    EXPECT_TRUE(write.source_consumers("reference-backtest").empty());
    EXPECT_EQ(write.apply(create).SerializeAsString(), original.SerializeAsString());
    EXPECT_FALSE(write.package("candidate-one").retirement_reason().empty());
    auto later = deliver(original.package());
    later.set_id("later-delivery-request");
    later.mutable_deliver()->set_id("later-draft");
    later.mutable_deliver()->set_target_task_id("later-run");
    EXPECT_THROW(write.apply(later), Error);
    research::proto::StrategyUse use;
    *use.mutable_package() = draft.delivery().package();
    *use.mutable_delivery() = protocol::strategy_delivery_object(draft.delivery());
    EXPECT_THROW(write.verify_use(use, "candidate-backtest", true), Error);
    EXPECT_NO_THROW(write.verify_use(use, "candidate-backtest", false));
    EXPECT_FALSE(write.delivery_status("delivery-one").target_accepted());
}

TEST_F(StrategyStore, PartialSchemaAndWrongOwnerRemainRejectedWithoutRepairingExistingData)
{
    {
        research::Strategies write(*store, owner, sqlite::Database::Access::writer);
    }
    auto other = owner;
    other.set_node_id("other-node");
    EXPECT_THROW(research::Strategies(*store, other), Error);
    store.reset();
    {
        sqlite::Database db(root / "tasks.sqlite");
        db.execute("DROP TABLE research_strategy_deliveries");
    }
    store = std::make_unique<tasks::Store>(
        asterion::research::task_domain(),
        root,
        tasks::Identity{"research-one"});
    EXPECT_THROW(
        research::Strategies(*store, owner, sqlite::Database::Access::writer),
        std::invalid_argument);
    store.reset();
    sqlite::Database db(root / "tasks.sqlite", sqlite::Database::Access::read_only);
    sqlite::Database::Statement missing(
        db,
        "SELECT count(*) FROM sqlite_master WHERE name='research_strategy_deliveries'");
    ASSERT_TRUE(missing.step());
    EXPECT_EQ(missing.integer(0), 0);
}

TEST_F(StrategyStore, AdmissionBindingAndCompletedUseSurviveDeprecationAndColdMetadataChecks)
{
    seed();
    research::proto::StrategyUse use;
    {
        research::Strategies write(*store, owner, sqlite::Database::Access::writer);
        const auto captured = capture();
        const auto pkg = write.apply(freeze(), &captured).package();
        const auto draft = write.apply(deliver(pkg)).delivery();
        *use.mutable_package() = draft.package();
        *use.mutable_delivery() = protocol::strategy_delivery_object(draft);
        auto wrong_input = input;
        wrong_input.mutable_strategies(0)->mutable_moving_average()->set_slow(4);
        auto wrong = task_submission(*store, "wrong-definition", wrong_input);
        asterion::tasks::set_program_artifact(wrong, pkg.definition().program_artifact());
        EXPECT_THROW(
            asterion::tasks::set_strategy_use(*store, wrong, use, pkg.definition()),
            Error);
        EXPECT_FALSE(fs::exists(root / "wrong-definition"));
        auto submitted = task_submission(*store, draft.target_task_id(), input);
        asterion::tasks::set_program_artifact(submitted, pkg.definition().program_artifact());
        asterion::tasks::set_strategy_use(*store, submitted, use, pkg.definition());
        task::proto::TaskDerivation origin;
        origin.set_source_id(pkg.reference_result().id());
        origin.set_source_result_digest(pkg.reference_result().version());
        origin.set_data_choice("fixed");
        submitted.set_derivation(origin);
        store->admit_submission(submitted);
        submitted.prepare_files();
        const auto prepared = store->prepared_submission_record(draft.target_task_id());
        EXPECT_EQ(
            prepared.record().task().strategy_use().SerializeAsString(),
            use.SerializeAsString());
        EXPECT_EQ(prepared.execution().strategy_use().SerializeAsString(), use.SerializeAsString());
        store->commit(store->register_submission(submitted));
        EXPECT_TRUE(write.delivery_status(draft.id()).target_accepted());
        EXPECT_EQ(store->describe(draft.target_task_id()).state(), task::proto::QUEUED);
        research::proto::StrategyMutation retire;
        retire.set_id("deprecate-after-admission");
        *retire.mutable_retire()->mutable_package() = use.package();
        retire.mutable_retire()->set_reason(
            "No new submissions; the admitted run retains its original definition");
        write.apply(retire);
        EXPECT_NO_THROW(write.verify_use(use, draft.target_task_id(), false));
        EXPECT_THROW(write.verify_use(use, draft.target_task_id(), true), Error);
        const auto token = store->commit(store->claim(draft.target_task_id())).token();
        store->commit(tasks::finish(*store, draft.target_task_id(), token, backtest::run(input)));
        EXPECT_EQ(
            tasks::verified_result(*store, draft.target_task_id())
                .result_task()
                .strategy_use()
                .SerializeAsString(),
            use.SerializeAsString());
        EXPECT_TRUE(write.delivery_status(draft.id()).target_accepted());
        auto changed_use = use;
        changed_use.mutable_delivery()->set_version(std::string(64, 'e'));
        auto replay = task_submission(*store, draft.target_task_id(), input);
        asterion::tasks::set_program_artifact(replay, pkg.definition().program_artifact());
        asterion::tasks::set_strategy_use(*store, replay, changed_use, pkg.definition());
        replay.set_derivation(origin);
        EXPECT_THROW(store->admit_submission(replay), Error);
    }
    store.reset();
    const std::vector<std::pair<std::string, research::proto::StrategyUse>> required = {
        {"candidate-backtest", use}
    };
    std::size_t candidates = 0, drafts = 0;
    research::Strategies::inspect_backup(
        root,
        owner,
        required,
        [&](const auto&) {
            ++candidates;
        },
        [&](const auto&) {
            ++drafts;
        });
    EXPECT_EQ(candidates, 1U);
    EXPECT_EQ(drafts, 1U);
    store = std::make_unique<tasks::Store>(
        asterion::research::task_domain(),
        root,
        tasks::Identity{"research-one"});
    {
        research::Strategies restored(*store, owner);
        EXPECT_TRUE(restored.delivery_status("delivery-one").target_accepted());
        EXPECT_EQ(
            store->describe("candidate-backtest").strategy_use().SerializeAsString(),
            use.SerializeAsString());
    }
    store.reset();
    {
        sqlite::Database db(root / "tasks.sqlite");
        db.execute("DELETE FROM research_strategy_deliveries WHERE id='delivery-one'");
    }
    EXPECT_THROW(
        research::Strategies::inspect_backup(
            root,
            owner,
            required,
            [](const auto&) {},
            [](const auto&) {}),
        Error);
}
