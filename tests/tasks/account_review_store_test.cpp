#include "task_store.hpp"
#include "research_submission.hpp"
#include "data_submission.hpp"
#include "research_execution.hpp"
#include "domain_backup.hpp"
#include "research_backup.hpp"
#include "program_files.hpp"
#include "support/local_listener.hpp"
#include "support/task_wait.hpp"
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/research.pb.h>
#include <atomic>
#include <thread>
#include "tasks/task_store_support.hpp"
#include "coverage.hpp"
#include <asterion/protocol/account_review.hpp>
#include <asterion/protocol/task_execution.hpp>
#include <asterion/protocol/trading_facts.hpp>
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <gtest/gtest.h>
using namespace asterion;
namespace fs = std::filesystem;

namespace {

struct Folder
{
    fs::path path = fs::temp_directory_path() / ("asterion-account-review-" + unique_process_id());

    Folder()
    {
        fs::create_directory(path);
    }

    ~Folder()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

research::proto::AccountReviewInput input()
{
    research::proto::AccountReviewInput value;
    value.mutable_owner()->set_node_id("fixture-node");
    value.mutable_owner()->set_service_id("research-one");
    value.mutable_owner()->set_domain("research");
    value.set_program_artifact(std::string(64, 'a'));
    auto& status = *value.mutable_evidence();
    auto& capture = *status.mutable_capture();
    auto& facts = *capture.mutable_facts();
    facts.mutable_source()->set_node_id("fixture-node");
    facts.mutable_source()->set_service_id("trading-one");
    facts.mutable_source()->set_domain("trading");
    facts.set_account_id("account-one");
    facts.set_environment(protocol::proto::BROKER_SIMULATION);
    facts.set_instance("source-instance");
    facts.set_id("fixed-source");
    facts.set_captured_at_ms(1790000000000);
    facts.set_policy_revision("policy");
    facts.set_risk_artifact(std::string(64, 'b'));
    facts.set_phase("disconnected");
    auto& ref = *capture.mutable_reference();
    *ref.mutable_owner() = facts.source();
    ref.set_id(facts.id());
    ref.set_kind("trading_facts");
    ref.set_version(sha256_bytes(facts.SerializeAsString()));
    auto& receipt = *status.mutable_operation();
    *receipt.mutable_reference() = ref;
    receipt.set_state(protocol::proto::TradingFactsReceipt::PUBLISHED);
    receipt.set_admitted_at_ms(1790000000000);
    receipt.set_published_at_ms(1790000000001);
    return value;
}

research::proto::AccountReviewResult review(const research::proto::AccountReviewInput& fixed)
{
    const auto& f = fixed.evidence().capture().facts();
    AccountObservationCoverage coverage;
    coverage.position_records = static_cast<unsigned>(f.positions_size());
    coverage.order_records = static_cast<unsigned>(f.orders_size());
    coverage.trade_records = static_cast<unsigned>(f.trades_size());
    coverage.unconfirmed_orders = static_cast<unsigned>(f.unconfirmed_size());
    coverage.broker_ready = f.phase() == "ready";
    coverage.positions_reconciled = f.positions_reconciled();
    coverage.funds_present = f.has_funds();
    coverage.recovery_required = f.recovery_required();
    const auto analyzed = review_account_observation(coverage);
    research::proto::AccountReviewResult output;
    *output.mutable_owner() = fixed.owner();
    *output.mutable_source() = fixed.evidence().operation().reference();
    output.set_account_id(f.account_id());
    output.set_environment(f.environment());
    output.set_program_artifact(fixed.program_artifact());
    output.set_conclusion(research::proto::AccountReviewResult::EVIDENCE_INSUFFICIENT);
    output.set_position_records(coverage.position_records);
    output.set_order_records(coverage.order_records);
    output.set_trade_records(coverage.trade_records);
    output.set_unconfirmed_orders(coverage.unconfirmed_orders);
    output.set_broker_ready(coverage.broker_ready);
    output.set_positions_reconciled(coverage.positions_reconciled);
    output.set_funds_present(coverage.funds_present);
    output.set_recovery_required(coverage.recovery_required);
    for (const auto& gap : analyzed.limitations)
        output.add_limitations(gap);
    return output;
}

std::string claim(tasks::Store& store, const std::string& id)
{
    auto read = store.prepare_input(id);
    read.load_for_claim();
    const auto token = store.commit(store.claim(read)).token();
    const auto attempt = store.confirm_attempt(std::move(read));
    EXPECT_TRUE(attempt.task().has_account_review());
    EXPECT_TRUE(attempt.execution().has_account_review());
    EXPECT_FALSE(attempt.execution().has_data_owner());
    EXPECT_TRUE(attempt.data_endpoint().empty());
    EXPECT_EQ(attempt.task().account_review().SerializeAsString(), input().SerializeAsString());
    return token;
}

void finish(tasks::Store& store, const std::string& id, const std::string& token)
{
    task::proto::TaskFinish result;
    result.set_id(id);
    result.set_token(token);
    *result.mutable_account_review() = review(input());
    store.commit(tasks::finish(store, std::move(result)));
}

} // namespace

TEST(AccountReviewStore, FixedInputIsReusedAndResultsRemainReadableWithoutAnyBusinessSource)
{
    Folder folder;
    std::string result_digest;
    {
        tasks::Store store(folder.path, {"research-one"});
        const auto first = tasks::submit(store, "review-one", input());
        EXPECT_EQ(first.kind(), task::proto::ACCOUNT_REVIEW);
        EXPECT_EQ(first.total(), 1U);
        EXPECT_EQ(first.account_review().SerializeAsString(), input().SerializeAsString());
        tasks::submit(store, "review-two", input());
        EXPECT_TRUE(
            fs::equivalent(
                folder.path / "review-one/input.pb",
                folder.path / "review-two/input.pb"));
        EXPECT_FALSE(store.describe("review-two").metadata().has_derivation())
            << "byte reuse does not invent a business derivation";
        const auto token = claim(store, "review-one");
        finish(store, "review-one", token);
        const auto response = tasks::verified_result(store, "review-one");
        ASSERT_TRUE(response.has_account_review());
        const auto decoded = protocol::decode_task_result(response, "review-one");
        EXPECT_EQ(decoded.at("kind"), "account_review");
        EXPECT_EQ(decoded.at("result").at("conclusion"), "evidence_insufficient");
        EXPECT_EQ(
            decoded.at("experiment").at("evidence").at("reference").at("version"),
            input().evidence().operation().reference().version());
        result_digest = store.describe("review-one").result_digest();
        EXPECT_FALSE(store.describe("review-one").has_account_review())
            << "task lists retain bounded metadata";
        resource::proto::CatalogueQuery query;
        query.set_kind("account_review_result");
        query.set_limit(20);
        const auto resources = store.resources("fixture-node", query);
        ASSERT_EQ(resources.items_size(), 1);
        EXPECT_EQ(resources.items(0).object().version(), result_digest);
    }
    {
        tasks::Store recovered(folder.path, {"research-one"});
        const auto response = tasks::verified_result(recovered, "review-one");
        EXPECT_EQ(response.result_task().result_digest(), result_digest);
        EXPECT_EQ(
            response.result_task().account_review().SerializeAsString(),
            input().SerializeAsString());
        EXPECT_EQ(
            response.account_review().source().SerializeAsString(),
            input().evidence().operation().reference().SerializeAsString());
    }
}

TEST(AccountReviewStore, CancellationWinsAndInterruptedAttemptsRetainTheirExactOwnedInput)
{
    Folder folder;
    std::string original_token;
    {
        tasks::Store store(folder.path, {"research-one"});
        tasks::submit(store, "cancelled", input());
        const auto token = claim(store, "cancelled");
        store.commit(store.cancel("cancelled"));
        finish(store, "cancelled", token);
        EXPECT_EQ(store.describe("cancelled").state(), task::proto::CANCELLED);
        EXPECT_TRUE(store.describe("cancelled").result_digest().empty());
        EXPECT_THROW(store.prepare_result("cancelled"), std::invalid_argument);
        tasks::submit(store, "interrupted", input());
        original_token = claim(store, "interrupted");
    }
    {
        tasks::Store recovered(folder.path, {"research-one"});
        EXPECT_EQ(recovered.describe("interrupted").state(), task::proto::INTERRUPTED);
        EXPECT_EQ(
            recovered.get("interrupted").account_review().SerializeAsString(),
            input().SerializeAsString());
        recovered.commit(recovered.retry("interrupted"));
        const auto token = claim(recovered, "interrupted");
        EXPECT_NE(token, original_token);
        finish(recovered, "interrupted", token);
        EXPECT_EQ(recovered.describe("interrupted").state(), task::proto::SUCCEEDED);
    }
}

TEST(AccountReviewStore, OwnedPreparationResumesAndAChangedScopeCannotOverwriteAnAcceptedIdentity)
{
    Folder folder;
    {
        tasks::Store store(folder.path, {"research-one"});
        auto preparation = task_submission(store, "prepared", input());
        store.admit_submission(preparation);
        preparation.prepare_files();
        EXPECT_TRUE(store.abandon_submission(preparation));
    }
    {
        tasks::Store recovered(folder.path, {"research-one"});
        const auto marker = recovered.prepared_submission_record("prepared");
        EXPECT_TRUE(marker.execution().has_account_review());
        EXPECT_FALSE(marker.execution().has_data_owner());
        auto resumed = recovered.resume_submission("prepared");
        recovered.admit_submission(resumed);
        recovered.commit(recovered.register_submission(resumed));
        EXPECT_EQ(
            recovered.get("prepared").account_review().SerializeAsString(),
            input().SerializeAsString());
        auto changed = input();
        changed.set_program_artifact(std::string(64, 'c'));
        EXPECT_THROW(tasks::submit(recovered, "prepared", changed), std::invalid_argument);
        EXPECT_EQ(
            recovered.get("prepared").account_review().SerializeAsString(),
            input().SerializeAsString());
        recovered.commit(recovered.cancel("prepared"));
        auto preview = recovered.retirement("prepared", "fixture-node");
        EXPECT_EQ(preview.object().kind(), "account_review");
        recovered.commit(recovered.retire(preview.object()));
        EXPECT_TRUE(recovered.describe("prepared").has_retirement());
        EXPECT_TRUE(fs::exists(folder.path / "prepared/input.pb"));
        EXPECT_THROW(tasks::submit(recovered, "prepared", input()), Error);
    }
}

TEST(AccountReviewExecution, OwnedEvidenceRunsWithoutSourcesAndRefusesAnotherProgram)
{
    Folder folder;
    tasks::Store store(folder.path, {"research-one"});
    tasks::submit(store, "review-one", input());
    auto read = store.prepare_input("review-one");
    read.load_for_claim();
    const auto token = store.commit(store.claim(read)).token();
    auto attempt = store.confirm_attempt(std::move(read));
    attempt.set_token(token);
    unsigned completed = 0;
    auto result = tasks::execute(
        attempt,
        folder.path,
        {},
        [&](auto done, auto total) {
            EXPECT_EQ(total, 1U);
            completed = done;
        },
        input().program_artifact());
    EXPECT_EQ(completed, 1U);
    protocol::validate_account_review_result(input(), result.account_review());
    EXPECT_EQ(
        result.account_review().conclusion(),
        research::proto::AccountReviewResult::EVIDENCE_INSUFFICIENT);
    EXPECT_THROW(
        (void)tasks::execute(
            attempt,
            folder.path,
            {},
            [](auto, auto) {},
            std::string(64, 'c')),
        Error);
    auto changed = attempt;
    changed.mutable_task()
        ->mutable_account_review()
        ->mutable_evidence()
        ->mutable_capture()
        ->mutable_facts()
        ->set_phase("ready");
    EXPECT_THROW(
        (void)tasks::execute(
            changed,
            folder.path,
            {},
            [](auto, auto) {},
            input().program_artifact()),
        std::invalid_argument);
    std::stop_source cancelled;
    cancelled.request_stop();
    EXPECT_THROW(
        (void)tasks::execute(
            attempt,
            folder.path,
            cancelled.get_token(),
            [](auto, auto) {},
            input().program_artifact()),
        Error);
    store.commit(tasks::finish(store, std::move(result)));
    EXPECT_TRUE(tasks::verified_result(store, "review-one").has_account_review());
}

TEST(AccountReviewProcess, DirectFixedEvidenceRunsWithoutDataAndReplaysAfterSourceLossAndRestart)
{
    using namespace std::chrono_literals;
    Folder folder;
    const auto root = folder.path / "research";
    fs::create_directory(root);
    Folder sockets;
    const auto endpoint = (sockets.path / "r.sock").string();
    const auto discovery_endpoint = (sockets.path / "d.sock").string();
    const auto source_endpoint = (sockets.path / "t.sock").string();
    const auto config = sockets.path / "research.json";
    replace_file_durably(config, R"({"task_concurrency":1})");
    const auto artifact = sha256_file(ASTERION_RESEARCH_PATH);
    const auto evidence = input().evidence();
    std::atomic<std::uint64_t> pid{0};
    std::atomic<unsigned> source_reads{0};
    std::atomic<bool> online{true};
    testing_support::LocalListener discovery(discovery_endpoint, 8), trading(source_endpoint, 8);
    std::jthread discovery_worker([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            try {
                auto peer = discovery.accept(100ms);
                node::proto::Request request;
                if (!request.ParseFromString(peer.receive(200ms)))
                    continue;
                node::proto::Response reply;
                reply.set_version(request.version());
                reply.set_correlation_id(request.correlation_id());
                auto& status = *reply.mutable_status();
                status.set_phase(node::proto::Status::READY);
                status.set_node_id("fixture-node");
                status.set_instance_id("fixture-agent");
                auto* research = status.add_services();
                research->set_id("research-one");
                research->set_kind(node::proto::RESEARCH);
                research->set_state("running");
                research->set_pid(pid.load());
                research->set_artifact(artifact);
                auto* source = status.add_services();
                source->set_id("trading-one");
                source->set_kind(node::proto::TRADING);
                source->set_state(online ? "running" : "stopped");
                source->set_endpoint(source_endpoint);
                peer.send(reply.SerializeAsString(), 200ms);
            } catch (const Error&) {
                // Bounded fixture socket waits, no production retry policy.
            }
        }
    });
    std::jthread source_worker([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            try {
                auto peer = trading.accept(100ms);
                protocol::proto::Request request;
                if (!request.ParseFromString(peer.receive(200ms)))
                    continue;
                ++source_reads;
                protocol::proto::Response reply;
                reply.set_version(request.version());
                reply.set_session_id(request.session_id());
                reply.set_correlation_id(request.correlation_id());
                if (!online || !request.has_facts() || !request.facts().has_read()
                    || request.facts().read().SerializeAsString()
                        != evidence.operation().reference().SerializeAsString()) {
                    reply.mutable_error()->set_code("not_found");
                    reply.mutable_error()->set_message("fixed fixture evidence unavailable");
                } else {
                    *reply.mutable_facts() = evidence;
                }
                peer.send(reply.SerializeAsString(), 200ms);
            } catch (const Error&) {
            }
        }
    });
    std::unique_ptr<ChildProcess> service;
    const auto call = [&](research::proto::ResearchRequest request) {
        request.set_version(1);
        request.set_service_id("research-one");
        request.set_correlation_id(unique_process_id());
        auto channel = ipc::Channel::connect(endpoint, 1s);
        channel.send(request.SerializeAsString(), 1s);
        task::proto::TaskResponse reply;
        if (!reply.ParseFromString(channel.receive(10s)))
            throw std::runtime_error("invalid research fixture response");
        protocol::validate_message(reply);
        if (reply.version() != request.version() || reply.service_id() != request.service_id()
            || reply.correlation_id() != request.correlation_id())
            throw std::runtime_error("research fixture response identity mismatch");
        if (reply.has_error())
            throw_remote_error(reply.error().code(), reply.error().message());
        return reply;
    };
    const auto start = [&] {
        service.reset();
        fs::remove(endpoint);
        fs::remove(endpoint + ".health");
        service = std::make_unique<ChildProcess>(
            "/usr/bin/env",
            std::vector<std::string>{
                "ASTERION_NODE_DIRECTORY=" + (folder.path / "node").string(),
                "ASTERION_NODE_DIRECTORY_ISOLATED=1",
                ASTERION_RESEARCH_PATH,
                "--config",
                config.string(),
                "--directory",
                root.string(),
                "--session",
                "research-one",
                "--endpoint",
                endpoint,
                "--health-endpoint",
                endpoint + ".health",
                "--node-id",
                "fixture-node",
                "--discovery-endpoint",
                discovery_endpoint,
                "--file-workers",
                "1"
            });
        pid = service->id();
        const auto until = std::chrono::steady_clock::now() + 10s;
        for (;;) {
            try {
                research::proto::ResearchRequest request;
                request.mutable_heartbeat();
                if (call(request).health().initialized())
                    return;
            } catch (const Error&) {
                if (service->exited())
                    throw;
            }
            if (std::chrono::steady_clock::now() >= until)
                throw std::runtime_error("research fixture failed to initialize");
            std::this_thread::sleep_for(20ms);
        }
    };
    start();
    research::proto::ResearchRequest submit;
    submit.mutable_submit()->set_id("review-one");
    auto* selected = submit.mutable_submit()->mutable_account_review();
    *selected->mutable_source() = evidence.operation().reference();
    selected->set_account_id("account-one");
    selected->set_environment(protocol::proto::BROKER_SIMULATION);
    EXPECT_EQ(call(submit).task().kind(), task::proto::ACCOUNT_REVIEW);
    online = false;
    const auto completed = testing_support::wait_task(call, "review-one", 10s);
    ASSERT_EQ(completed.state(), task::proto::SUCCEEDED) << completed.ShortDebugString();
    EXPECT_EQ(source_reads.load(), 1U);
    EXPECT_FALSE(fs::exists(root / "review-one/data-retention.pb"));
    EXPECT_EQ(sha256_file(root / "programs" / artifact), artifact);
    research::proto::ResearchRequest result;
    result.mutable_result()->set_id("review-one");
    const auto original = call(result);
    ASSERT_TRUE(original.has_account_review());
    EXPECT_EQ(original.account_review().program_artifact(), artifact);
    EXPECT_EQ(
        original.account_review().source().SerializeAsString(),
        selected->source().SerializeAsString());
    EXPECT_EQ(call(submit).task().state(), task::proto::SUCCEEDED);
    auto changed = submit;
    changed.mutable_submit()->mutable_account_review()->mutable_source()->set_version(
        std::string(64, 'c'));
    EXPECT_THROW((void)call(changed), Error);
    start();
    EXPECT_EQ(call(submit).task().state(), task::proto::SUCCEEDED);
    EXPECT_EQ(
        call(result).account_review().SerializeAsString(),
        original.account_review().SerializeAsString());
    EXPECT_EQ(source_reads.load(), 1U);
    research::proto::ResearchRequest resources;
    resources.mutable_resources()->set_kind("research_program");
    resources.mutable_resources()->set_limit(1);
    const auto program_time = fs::last_write_time(root / "programs" / artifact);
    const auto catalogue = call(resources).resources();
    ASSERT_EQ(catalogue.items_size(), 1);
    EXPECT_EQ(catalogue.items(0).object().version(), artifact);
    EXPECT_EQ(catalogue.items(0).object().owner().node_id(), "fixture-node");
    EXPECT_TRUE(catalogue.next_after().empty());
    EXPECT_EQ(
        fs::last_write_time(root / "programs" / artifact).time_since_epoch().count(),
        program_time.time_since_epoch().count());
    // Simulate acceptance interrupted before its index commit in this isolated owner.
    service.reset();
    const auto owned = original.result_task().account_review();
    for (const auto* id : {"prepared-owned", "prepared-missing"}) {
        tasks::Store store(root, {"research-one"});
        auto draft = task_submission(store, id, owned);
        store.admit_submission(draft);
        draft.prepare_files();
        // Each simulated crash ends its single preparation owner before reopening.
    }
    {
        tasks::Store store(root, {"research-one"});
        auto wrong_program = owned;
        wrong_program.set_program_artifact(std::string(64, 'e'));
        tasks::submit(store, "wrong-program", wrong_program);
    }
    fs::remove(root / "prepared-missing/input.pb");
    start();
    research::proto::ResearchRequest recover;
    recover.mutable_recover_submission()->set_id("prepared-owned");
    EXPECT_EQ(call(recover).task().kind(), task::proto::ACCOUNT_REVIEW);
    EXPECT_EQ(
        testing_support::wait_task(call, "prepared-owned", 10s).state(),
        task::proto::SUCCEEDED);
    EXPECT_EQ(source_reads.load(), 1U);
    recover.mutable_recover_submission()->set_id("prepared-missing");
    EXPECT_THROW((void)call(recover), Error);
    EXPECT_FALSE(fs::exists(root / "prepared-missing/input.pb"));
    EXPECT_EQ(source_reads.load(), 1U);
    online = true;
    EXPECT_EQ(call(recover).task().kind(), task::proto::ACCOUNT_REVIEW);
    EXPECT_EQ(
        testing_support::wait_task(call, "prepared-missing", 10s).state(),
        task::proto::SUCCEEDED);
    EXPECT_EQ(source_reads.load(), 2U);
    EXPECT_EQ(
        sha256_file(root / "prepared-owned/input.pb"),
        sha256_file(root / "prepared-missing/input.pb"));
    const auto refused = testing_support::wait_task(call, "wrong-program", 10s);
    EXPECT_EQ(refused.state(), task::proto::FAILED);
    EXPECT_EQ(refused.error_code(), "conflict");
    EXPECT_EQ(source_reads.load(), 2U);
    research::proto::ResearchRequest retire;
    retire.mutable_retirement()->set_id("review-one");
    retire.mutable_retirement()->set_execute(true);
    const auto retired = call(retire).retirement();
    EXPECT_EQ(retired.state(), task::proto::TaskRetirement::RETIRED);
    EXPECT_FALSE(retired.source_release_confirmed());
    EXPECT_TRUE(retired.has_release_error());
    EXPECT_TRUE(call(result).has_account_review());
}

TEST(AccountReviewBackup, RestoresOwnedEvidenceAndActualProgramWithoutPretendingToBackUpTrading)
{
    Folder folder;
    const auto root = folder.path / "research";
    fs::create_directory(root);
    auto fixed = input();
    const auto artifact = sha256_file(current_executable());
    fixed.set_program_artifact(artifact);
    {
        tasks::Store store(root, {"research-one"});
        (void)programs::Files(root / "programs", fixed.owner())
            .preserve(current_executable(), artifact);
        tasks::submit(store, "review-one", fixed);
        auto read = store.prepare_input("review-one");
        read.load_for_claim();
        const auto token = store.commit(store.claim(read)).token();
        auto attempt = store.confirm_attempt(std::move(read));
        attempt.set_token(token);
        auto output = tasks::execute(attempt, root, {}, [](auto, auto) {}, artifact);
        store.commit(tasks::finish(store, std::move(output)));
    }
    const auto index_before = sha256_file(root / "tasks.sqlite");
    const auto destination = folder.path / "backup";
    {
        auto cut = terminal_backup::cold_cut({{fixed.owner(), root}});
        const auto plan = cut.preview("review-backup");
        bool program_included = false, source_missing = false;
        for (const auto& dependency : plan.at("dependencies")) {
            if (dependency.at("object").at("kind") == "research_program")
                program_included = dependency.at("included");
            if (dependency.at("object").at("kind") == "trading_facts") {
                source_missing = !dependency.at("included").get<bool>();
                EXPECT_EQ(
                    dependency.at("object").at("owner").at("node"),
                    fixed.evidence().operation().reference().owner().node_id());
                EXPECT_EQ(
                    dependency.at("object").at("owner").at("service"),
                    fixed.evidence().operation().reference().owner().service_id());
                EXPECT_EQ(
                    dependency.at("object").at("version"),
                    fixed.evidence().operation().reference().version());
            }
        }
        EXPECT_TRUE(program_included);
        EXPECT_TRUE(source_missing);
        EXPECT_EQ(
            plan.at("limitations").dump().find("original_execution_program_identity_not_recorded"),
            std::string::npos);
        cut.save("review-backup", destination, sha256_bytes(plan.dump()));
    }
    const auto restore = folder.path / "restore";
    backup_files::restore_files(destination, restore);
    fs::rename(root, folder.path / "preserved-source");
    const auto restored = restore / "owners/0";
    EXPECT_EQ(sha256_file(restored / "tasks.sqlite"), index_before);
    EXPECT_EQ(
        programs::Files(restored / "programs", fixed.owner()).verify(artifact).digest,
        artifact);
    {
        auto inspected = research::cold_backup(restored, fixed.owner());
        EXPECT_TRUE(inspected.references.empty()) << "Trading is not a fabricated Data retention";
        EXPECT_EQ(inspected.objects.size(), 3U);
    }
    {
        tasks::Store store(restored, {"research-one"});
        const auto evidence = tasks::verified_result(store, "review-one");
        EXPECT_EQ(
            evidence.result_task().account_review().SerializeAsString(),
            fixed.SerializeAsString());
        EXPECT_EQ(evidence.account_review().program_artifact(), artifact);
    }
    auto wrong = fixed.owner();
    wrong.set_node_id("other-node");
    EXPECT_THROW(research::cold_backup(restored, wrong), Error);
    const auto before = sha256_file(restored / "programs" / artifact);
    replace_file_durably(restored / "programs" / artifact, "corrupt-isolated-fixture");
    EXPECT_THROW(research::cold_backup(restored, fixed.owner()), std::exception);
    EXPECT_EQ(sha256_file(folder.path / "preserved-source/programs" / artifact), before);
}
