#include "data_retentions.hpp"
#include "support/data_discovery.hpp"
#include <asterion/research.pb.h>
#include "support/task_wait.hpp"
#include "data_store.hpp"
#include "data/history_requests.hpp"
#include "download_budget.hpp"
#include "data/data_fixture.hpp"
#include "data/history_fixture.hpp"
#include "task_store.hpp"
#include "sqlite_database.hpp"
#include "tasks/task_store_support.hpp"
#include "data/bar_fixture.hpp"
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/data_client.hpp>
#include <gtest/gtest.h>
#include <fstream>
#include <thread>
#include <array>
#include <barrier>
#include <future>
using namespace asterion;
namespace fs = std::filesystem;

namespace {

struct Folder
{
    fs::path path = fs::path("/tmp") / ("asterion-data-" + unique_process_id());

    Folder()
    {
        fs::create_directory(path);
    }

    ~Folder()
    {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

// A bar every minute of the requested range, on the day it falls in.
struct Minutes final : HistoricalBarPort
{
    HistorySemantics semantics() const override
    {
        return {"tushare.ft_mins", "fixture.v1", "Asia/Shanghai", "bar_end"};
    }

    std::vector<HistoricalBar> read(const HistoricalBarRange& range, std::stop_token) override
    {
        using namespace std::chrono;
        std::vector<HistoricalBar> bars;
        const auto price = Decimal::parse("100");
        for (auto t = range.begin_ns; t <= range.end_ns; t += 60'000'000'000) {
            const auto day =
                year_month_day(floor<days>(sys_time<nanoseconds>(nanoseconds(t)) + hours(8)));
            bars.push_back(
                {t, price, price, price, price, price, price, price, format_trading_date(day)});
        }
        return bars;
    }
};

// The one bar the downloads here ask for.
const auto minute = parse_shanghai_time("2026-09-25 09:01:00");

// Every calendar day from the first to the last, as the fixtures here trade.
std::vector<std::string> every_day(const char* first, const char* last)
{
    std::vector<std::string> days;
    for (auto day = std::chrono::sys_days(parse_trading_date(first));
        day <= std::chrono::sys_days(parse_trading_date(last));
        day += std::chrono::days(1))
        days.push_back(format_trading_date(std::chrono::year_month_day(day)));
    return days;
}

data::proto::DownloadAllocation allocation()
{
    data::proto::DownloadAllocation request;
    auto* identity = request.mutable_identity();
    identity->set_data_instance("data-one");
    identity->set_task_instance("data-one");
    identity->set_task_id("download-one");
    identity->set_attempt(1);
    *request.mutable_minutes() = asterion::testing_support::minute_request(
        {{"contract_id", "SHFE/rb/2026-10"},
            {"source", "tushare.ft_mins"},
            {"source_instrument", "RB2610.SHF"},
            {"interval_minutes", 1},
            {"begin_ns", std::to_string(minute)},
            {"end_ns", std::to_string(minute)},
            {"requests_per_minute", 60}});
    return request;
}

data::proto::DownloadPreparation
download(const data::proto::DownloadAllocation& request, const std::string& directory)
{
    Minutes provider;
    history_files::download_minutes(
        provider,
        history_files::minute_range(request.minutes()),
        directory);
    data::proto::DownloadPreparation prepared;
    *prepared.mutable_identity() = request.identity();
    auto* record = prepared.mutable_record();
    *record->mutable_minutes() = request.minutes();
    *record->mutable_minute_result() = history_files::minute_result(directory);
    return prepared;
}

void await_ready(protocol::DataClient& client)
{
    using namespace std::chrono_literals;
    data::proto::DataRequest health;
    health.mutable_heartbeat();
    const auto until = std::chrono::steady_clock::now() + 10s;
    while (!client.call(health).health().initialized()) {
        ASSERT_LT(std::chrono::steady_clock::now(), until);
        std::this_thread::sleep_for(10ms);
    }
}

void configure_budget(data::DownloadBudget& budget, const std::string& account, unsigned limit)
{
    const auto configuration = budget.configure("provider", account, limit);
    budget.persist(configuration);
    budget.committed(configuration);
}

} // namespace

TEST(DownloadBudget, AccountSharesWindowAndChangingLimitPreservesAdmissions)
{
    Folder root;
    auto clock = std::make_shared<ManualClock>();
    data::DownloadBudget budget(fs::canonical(root.path), clock);
    EXPECT_THROW(budget.acquire("provider", "account"), std::invalid_argument);
    configure_budget(budget, "account", 2);
    EXPECT_TRUE(budget.acquire("provider", "account").granted());
    clock->advance(10'000'000'000LL);
    EXPECT_TRUE(budget.acquire("provider", "account").granted());
    EXPECT_EQ(budget.acquire("provider", "account").retry_after_ms(), 50000);
    configure_budget(budget, "account", 1);
    EXPECT_EQ(budget.acquire("provider", "account").retry_after_ms(), 60000);
    const auto pending = budget.configure("provider", "another-account", 1);
    budget.persist(pending);
    EXPECT_THROW(budget.acquire("provider", "another-account"), std::invalid_argument);
    budget.committed(pending);
    EXPECT_TRUE(budget.acquire("provider", "another-account").granted());
    clock->advance(60'000'000'000LL);
    EXPECT_TRUE(budget.acquire("provider", "account").granted());
    EXPECT_FALSE(budget.acquire("provider", "account").granted());
}

TEST(DownloadBudget, RestartAndReconfigurationCannotResetTheQuotaWindow)
{
    Folder root;
    auto clock = std::make_shared<ManualClock>();
    {
        data::DownloadBudget budget(fs::canonical(root.path), clock);
        configure_budget(budget, "account", 1);
        EXPECT_TRUE(budget.acquire("provider", "account").granted());
    }
    data::DownloadBudget restored(fs::canonical(root.path), clock);
    configure_budget(restored, "account", 2);
    EXPECT_EQ(restored.acquire("provider", "account").retry_after_ms(), 60000);
    clock->advance(59'999'999'999LL);
    EXPECT_EQ(restored.acquire("provider", "account").retry_after_ms(), 1);
    clock->advance(1);
    EXPECT_TRUE(restored.acquire("provider", "account").granted());
    EXPECT_TRUE(restored.acquire("provider", "account").granted());
    EXPECT_FALSE(restored.acquire("provider", "account").granted());
}

TEST(DownloadBudget, UncertainPolicyCommitStopsAdmissionsUntilRestart)
{
    Folder root;
    auto clock = std::make_shared<ManualClock>();
    data::DownloadBudget budget(fs::canonical(root.path), clock);
    configure_budget(budget, "account", 2);
    const auto pending = budget.configure("provider", "account", 1);
    fail_next_directory_syncs_for_testing(1);
    EXPECT_THROW(budget.persist(pending), std::runtime_error);
    budget.persistence_failed();
    fail_next_directory_syncs_for_testing(0);
    EXPECT_THROW(budget.acquire("provider", "account"), Error);
    EXPECT_THROW(budget.configure("provider", "account", 2), Error);
    data::DownloadBudget restored(fs::canonical(root.path), clock);
    EXPECT_FALSE(restored.acquire("provider", "account").granted());
    clock->advance(60'000'000'000LL);
    EXPECT_TRUE(restored.acquire("provider", "account").granted());
    EXPECT_FALSE(restored.acquire("provider", "account").granted());
}

TEST(DataAuthorization, CapturesFixedPrivateCredentialsAndRejectsReplacement)
{
    Folder root;
    data::proto::DownloadAuthorizationRequest request;
    request.set_task_instance("data-one");
    request.set_task_id("download-one");
    request.set_credential("fixture-private-token");
    *request.mutable_minutes() = allocation().minutes();
    data::proto::DownloadAuthorization authorization;
    {
        data::Store store(root.path, "data-one");
        authorization = store.authorize(store.verify_authorization(request));
        EXPECT_EQ(authorization.data_instance(), "data-one");
        EXPECT_EQ(authorization.task_instance(), "data-one");
        EXPECT_EQ(authorization.task_id(), "download-one");
        EXPECT_EQ(
            authorization.minutes().SerializeAsString(),
            request.minutes().SerializeAsString());
        EXPECT_EQ(authorization.provider_artifact().size(), 64);
        EXPECT_EQ(authorization.SerializeAsString().find(request.credential()), std::string::npos);
        EXPECT_EQ(
            store.authorize(store.verify_authorization(request)).SerializeAsString(),
            authorization.SerializeAsString());
        auto changed = request;
        changed.set_credential("different-fixture-token");
        EXPECT_THROW(store.authorize(store.verify_authorization(changed)), Error);
        changed = request;
        changed.mutable_minutes()->set_end_ns(minute + 60'000'000'000);
        EXPECT_THROW(store.authorize(store.verify_authorization(changed)), Error);
        changed = request;
        changed.set_task_instance("different-task-service");
        EXPECT_THROW(store.verify_authorization(changed), Error);
        const auto path = root.path / "authorizations" / (authorization.id() + ".pb");
        const auto public_permissions = fs::perms::group_all | fs::perms::others_all;
        EXPECT_EQ(fs::status(path).permissions() & public_permissions, fs::perms::none);
    }
    data::Store restored(root.path, "data-one");
    EXPECT_EQ(
        restored.authorization(authorization.id()).SerializeAsString(),
        authorization.SerializeAsString());
}

TEST(DataAuthorization, FailedDurableCaptureCannotBeAcknowledgedUntilItsBarrierCompletes)
{
    Folder root;
    data::Store store(root.path, "data-one");
    data::proto::DownloadAuthorizationRequest request;
    request.set_task_instance("data-one");
    request.set_task_id("download-one");
    request.set_credential("fixture-private-token");
    *request.mutable_minutes() = allocation().minutes();
    const auto verified = store.verify_authorization(request);
    fail_next_directory_syncs_for_testing(1);
    EXPECT_THROW(store.authorize(verified), std::runtime_error);
    fail_next_directory_syncs_for_testing(0);
    const auto id = sha256_bytes("data-one/data-one/download-one");
    fail_next_directory_syncs_for_testing(1);
    EXPECT_THROW(store.authorization(id), std::runtime_error);
    fail_next_directory_syncs_for_testing(0);
    const auto acknowledged = store.authorize(verified);
    EXPECT_EQ(store.authorization(id).SerializeAsString(), acknowledged.SerializeAsString());
}

TEST(DataService, AllocationAndPublicationSurviveAParentPathAliasWithoutRewritingEvidence)
{
    Folder root;
    fs::create_directories(root.path / "physical/warehouse");
    fs::create_directory_symlink(root.path / "physical", root.path / "alias");
    auto request = allocation();
    std::string directory;
    fs::path receipt;
    std::string digest;
    {
        data::Store store(root.path / "physical/warehouse", "data-one");
        directory = test::allocate_download(store, request).directory();
        for (const auto& file : fs::recursive_directory_iterator(root.path / "physical/warehouse"))
            if (file.path().filename() == "allocation.pb")
                receipt = file.path();
        ASSERT_FALSE(receipt.empty());
        digest = sha256_file(receipt);
    }
    data::Store reopened(root.path / "alias/warehouse", "data-one");
    EXPECT_EQ(test::allocate_download(reopened, request).directory(), directory);
    EXPECT_EQ(sha256_file(receipt), digest);
    const auto prepared = download(request, directory);
    const auto accepted = reopened.prepare(reopened.verify_download(prepared));
    data::proto::DownloadPublication decision;
    *decision.mutable_identity() = request.identity();
    decision.set_candidate_digest(accepted.candidate_digest());
    decision.set_publication_id("publish-one");
    const auto published = reopened.publish(reopened.verify_publication(decision));
    const auto same_evidence = [&](data::proto::HistoryRecord projected) {
        // Location follows this owner's read root; it is not the immutable version identity.
        EXPECT_TRUE(fs::equivalent(projected.minute_result().directory(), directory));
        projected.mutable_minute_result()->set_directory(directory);
        EXPECT_EQ(projected.SerializeAsString(), prepared.record().SerializeAsString());
    };
    same_evidence(published.record());
    const auto index =
        root.path / "physical/warehouse/history/index" / (accepted.dataset_id() + ".pb");
    const auto indexed = sha256_file(index);
    same_evidence(reopened.archive().get(accepted.dataset_id()));
    history_files::Archive physical(
        root.path / "physical/warehouse/history",
        history_files::Archive::Access::read_only);
    same_evidence(physical.get(accepted.dataset_id()));
    EXPECT_EQ(sha256_file(index), indexed);
    EXPECT_EQ(sha256_file(receipt), digest);
    // A link inside the archive is not a parent alias and must not bypass ownership.
    fs::rename(directory, directory + "-moved");
    fs::create_directory_symlink(directory + "-moved", directory);
    EXPECT_THROW(reopened.archive().get(accepted.dataset_id()), std::invalid_argument);
    EXPECT_THROW(reopened.publish(reopened.verify_publication(decision)), std::invalid_argument);
}

TEST(DataService, PreparationIsPrivateAndPublicationKeepsItsIdentityAcrossRestarts)
{
    Folder root;
    auto request = allocation();
    data::proto::DownloadPreparation prepared;
    data::proto::PreparedDownload accepted;
    {
        data::Store store(root.path, "data-one");
        const auto directory = test::allocate_download(store, request).directory();
        EXPECT_EQ(test::allocate_download(store, request).directory(), directory);
        auto conflict = request;
        conflict.mutable_minutes()->set_end_ns(minute + 60'000'000'000);
        EXPECT_THROW(store.allocate(store.verify_allocation(conflict)), Error);
        prepared = download(request, directory);
        accepted = store.prepare(store.verify_download(prepared));
        EXPECT_EQ(
            store.prepare(store.verify_download(prepared)).SerializeAsString(),
            accepted.SerializeAsString());
        EXPECT_TRUE(store.archive().datasets({}).empty());
    }
    data::proto::DownloadPublication decision;
    *decision.mutable_identity() = request.identity();
    decision.set_candidate_digest(accepted.candidate_digest());
    decision.set_publication_id("publish-one");
    {
        data::Store store(root.path, "data-one");
        EXPECT_TRUE(store.archive().datasets({}).empty());
        auto wrong = decision;
        wrong.set_candidate_digest(std::string(64, 'a'));
        EXPECT_THROW(store.publish(store.verify_publication(wrong)), Error);
        const auto result = store.publish(store.verify_publication(decision));
        EXPECT_EQ(result.record().minute_result().manifest_sha256(), accepted.dataset_id());
        EXPECT_EQ(
            store.publish(store.verify_publication(decision)).SerializeAsString(),
            result.SerializeAsString());
        wrong = decision;
        wrong.set_publication_id("different-decision");
        EXPECT_THROW(store.publish(store.verify_publication(wrong)), Error);
    }
    {
        data::Store store(root.path, "data-one");
        EXPECT_EQ(store.published(request.identity()).decision().publication_id(), "publish-one");
        EXPECT_EQ(store.archive().datasets({}).size(), 1);
    }
    EXPECT_THROW((data::Store(root.path, "another-data")), std::invalid_argument);
    EXPECT_NO_THROW((data::Store(root.path, "data-one")));
}

TEST(DataService, SeparateProcessServesPublishedHistoryWithoutATaskService)
{
    using namespace std::chrono_literals;
    Folder root, sockets;
    auto request = allocation();
    data::proto::PreparedDownload accepted;
    {
        data::Store store(root.path, "data-one");
        accepted = store.prepare(store.verify_download(
            download(request, test::allocate_download(store, request).directory())));
    }
    const auto endpoint = (sockets.path / "data.sock").string();
    const auto worker = (sockets.path / "worker.sock").string();
    auto process = std::make_unique<ChildProcess>(
        ASTERION_DATA_SERVICE_PATH,
        std::vector<std::string>{
            "--directory",
            root.path.string(),
            "--session",
            "data-one",
            "--endpoint",
            endpoint,
            "--internal-endpoint",
            worker
        },
        false,
        sockets.path / "service.log",
        true);
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (
        !fs::exists(endpoint) && !process->exited() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
    std::ifstream log(sockets.path / "service.log");
    const std::string diagnostics{std::istreambuf_iterator<char>(log), {}};
    ASSERT_TRUE(fs::exists(endpoint)) << diagnostics;
    protocol::DataClient public_client(endpoint, "data-one"), coordinator(worker, "data-one");
    await_ready(public_client);
    // Connected peers which send no frame cannot occupy file execution slots or
    // consume the coordinator's separate admission capacity.
    std::vector<ipc::Channel> silent;
    for (unsigned i = 0; i < 9; ++i)
        silent.push_back(ipc::Channel::connect(endpoint, 1s));
    for (unsigned i = 0; i < 2; ++i)
        silent.push_back(ipc::Channel::connect(worker, 1s));
    data::proto::DataRequest capture;
    auto* authorization_request = capture.mutable_authorize_download();
    authorization_request->set_task_instance("data-one");
    authorization_request->set_task_id("authorized-download");
    authorization_request->set_credential("fixture-rpc-private-token");
    *authorization_request->mutable_minutes() = request.minutes();
    data::proto::DataResponse captured;
    {
        std::array<data::proto::DataRequest, 2> requests{capture, capture};
        requests[1].mutable_authorize_download()->set_credential("different-fixture-token");
        std::array<ipc::Channel, 2> channels{
            ipc::Channel::connect(endpoint, 1s),
            ipc::Channel::connect(endpoint, 1s)
        };
        for (unsigned i = 0; i < requests.size(); ++i) {
            requests[i].set_version(1);
            requests[i].set_service_id("data-one");
            requests[i].set_correlation_id(unique_process_id());
            channels[i].send(requests[i].SerializeAsString(), 1s);
        }
        std::array<data::proto::DataResponse, 2> replies;
        for (unsigned i = 0; i < replies.size(); ++i)
            ASSERT_TRUE(replies[i].ParseFromString(channels[i].receive(5s)));
        ASSERT_NE(replies[0].has_download_authorization(), replies[1].has_download_authorization());
        const unsigned accepted = replies[0].has_download_authorization() ? 0 : 1;
        EXPECT_EQ(replies[1 - accepted].error().code(), "conflict");
        captured = std::move(replies[accepted]);
        capture = std::move(requests[accepted]);
        authorization_request = capture.mutable_authorize_download();
    }
    ASSERT_TRUE(captured.has_download_authorization());
    EXPECT_EQ(
        captured.SerializeAsString().find(authorization_request->credential()),
        std::string::npos);
    data::proto::DataRequest admission;
    admission.mutable_download_authorization()->set_id(captured.download_authorization().id());
    EXPECT_THROW(public_client.call(admission), Error);
    EXPECT_EQ(
        coordinator.call(admission).download_authorization().SerializeAsString(),
        captured.download_authorization().SerializeAsString());
    data::proto::DataRequest credential_request;
    *credential_request.mutable_download_credentials() = request.identity();
    EXPECT_THROW(public_client.call(credential_request), Error);
    EXPECT_EQ(coordinator.call(credential_request).download_credentials().credential(), "fixture");
    data::proto::DataRequest permit;
    *permit.mutable_acquire_download_permit() = request.identity();
    EXPECT_THROW(public_client.call(permit), Error);
    EXPECT_THROW(coordinator.call(permit), Error);
    data::proto::DataRequest budget;
    budget.mutable_configure_download_budget()->set_source("tushare.ft_mins");
    budget.mutable_configure_download_budget()->set_credential("fixture");
    budget.mutable_configure_download_budget()->set_requests_per_minute(1);
    EXPECT_EQ(public_client.call(budget).download_budget().requests_per_minute(), 1);
    EXPECT_TRUE(coordinator.call(permit).download_permit().granted());
    EXPECT_GT(coordinator.call(permit).download_permit().retry_after_ms(), 0);
    data::proto::DataRequest publish;
    auto* decision = publish.mutable_publish_download();
    *decision->mutable_identity() = request.identity();
    decision->set_candidate_digest(accepted.candidate_digest());
    decision->set_publication_id("publish-one");
    EXPECT_THROW(public_client.call(publish), Error);
    EXPECT_TRUE(coordinator.call(publish).has_published_download());
    data::proto::DataRequest query;
    query.mutable_minute_page()->set_dataset_id(accepted.dataset_id());
    query.mutable_minute_page()->set_limit(10);
    const auto page = public_client.call(query);
    ASSERT_TRUE(page.has_minute_page());
    ASSERT_EQ(page.minute_page().bars_size(), 1);
    EXPECT_EQ(page.minute_page().bars(0).timestamp_ns(), minute);
    data::proto::DataRequest heartbeat;
    heartbeat.mutable_heartbeat();
    const auto health = public_client.call(heartbeat).health();
    EXPECT_EQ(health.instance_id(), "data-one");
    EXPECT_FALSE(health.recovery_required());
    process.reset();
    Folder empty_plugins;
    // The original operation remains confirmable after its provider is disabled.
    const auto restored_endpoint = (sockets.path / "restored.sock").string();
    ChildProcess restored(
        ASTERION_DATA_SERVICE_PATH,
        {"--directory",
            root.path.string(),
            "--session",
            "data-one",
            "--endpoint",
            restored_endpoint,
            "--internal-endpoint",
            (sockets.path / "restored.workers").string(),
            "--plugin-directory",
            empty_plugins.path.string()},
        false,
        sockets.path / "restored.log",
        true);
    const auto restarted_until = std::chrono::steady_clock::now() + 10s;
    while (!fs::exists(restored_endpoint) && !restored.exited()
        && std::chrono::steady_clock::now() < restarted_until)
        std::this_thread::sleep_for(10ms);
    ASSERT_TRUE(fs::exists(restored_endpoint));
    protocol::DataClient replay(restored_endpoint, "data-one");
    await_ready(replay);
    EXPECT_EQ(
        replay.call(capture).download_authorization().SerializeAsString(),
        captured.download_authorization().SerializeAsString());
    capture.mutable_authorize_download()->set_task_id("new-download");
    EXPECT_THROW(replay.call(capture), Error);
}

TEST(DownloadPublication, CancellationBeforeDecisionKeepsTheCandidatePrivate)
{
    Folder tasks, warehouse;
    data::Store data(warehouse.path, "data-one");
    tasks::Store store(asterion::data::task_domain(), tasks.path, tasks::Identity{"data-one"});
    auto request = allocation();
    const auto id = request.identity().task_id();
    tasks::submit(store, test::authorize_download(data, "data-one", id, request.minutes()));
    task::proto::TaskFinish finish;
    finish.set_id(id);
    finish.set_token(store.commit(store.claim(id)).token());
    const auto candidate = download(request, test::allocate_download(data, request).directory());
    EXPECT_FALSE(fs::exists(tasks.path / id / "provider.credential"));
    EXPECT_FALSE(fs::exists(tasks.path / "authorizations"));
    EXPECT_EQ(store.get(id).download_authorization(), request.authorization_id());
    const auto credentials = data.credentials(request.identity());
    EXPECT_EQ(credentials.authorization_id(), store.get(id).download_authorization());
    EXPECT_EQ(credentials.credential(), "fixture");
    auto foreign_attempt = request.identity();
    foreign_attempt.set_attempt(foreign_attempt.attempt() + 1);
    EXPECT_THROW(data.credentials(foreign_attempt), std::invalid_argument);

    *finish.mutable_minutes() = candidate.record().minute_result();
    auto completion = store.prepare_finish(finish);
    completion.prepare_payload();
    const auto prepared = data.prepare(data.verify_download(candidate));
    EXPECT_EQ(store.commit(store.cancel(id)).task().state(), task::proto::CANCEL_REQUESTED);
    EXPECT_FALSE(
        (store.commit(asterion::data::prepare_publication(store, std::move(completion), prepared))
                .task()
                .state()
            == asterion::task::proto::PUBLISHING));
    EXPECT_EQ(store.describe(id).state(), task::proto::CANCELLED);
    EXPECT_TRUE(store.pending_publications().empty());
    EXPECT_TRUE(data.archive().datasets({}).empty());
}

TEST(DownloadPublication, DataRestartResumesTheOriginalDecisionAndRetainsConfirmedResult)
{
    using namespace std::chrono_literals;
    Folder warehouse, sockets;
    const auto ledger = warehouse.path / "acquisition-tasks";
    auto request = allocation();
    const auto id = request.identity().task_id();
    std::string token, dataset_id;
    data::proto::DownloadPublication decision;
    {
        data::Store data(warehouse.path, "data-one");
        fs::create_directory(ledger);
        tasks::Store store(asterion::data::task_domain(), ledger, tasks::Identity{"data-one"});
        tasks::submit(store, test::authorize_download(data, "data-one", id, request.minutes()));
        token = store.commit(store.claim(id)).token();
        const auto candidate =
            download(request, test::allocate_download(data, request).directory());
        const auto prepared = data.prepare(data.verify_download(candidate));
        dataset_id = prepared.dataset_id();
        task::proto::TaskFinish finish;
        finish.set_id(id);
        finish.set_token(token);
        *finish.mutable_minutes() = candidate.record().minute_result();
        auto completion = store.prepare_finish(finish);
        completion.prepare_payload();
        ASSERT_TRUE(
            (store.commit(
                      asterion::data::prepare_publication(store, std::move(completion), prepared))
                    .task()
                    .state()
                == asterion::task::proto::PUBLISHING));
        EXPECT_EQ(store.describe(id).state(), task::proto::PUBLISHING);
        EXPECT_EQ(store.commit(store.cancel(id)).task().state(), task::proto::PUBLISHING);
        EXPECT_THROW(
            store.commit(store.interrupt(id, token, "worker exited")),
            std::invalid_argument);
        EXPECT_THROW(store.commit(store.retry(id)).task(), std::invalid_argument);
        decision = store.pending_publications().at(0);
        EXPECT_TRUE(data.archive().datasets({}).empty());
    }
    const auto endpoint = (sockets.path / "data.sock").string();
    const auto internal = (sockets.path / "data.internal").string();
    auto start = [&] {
        auto process = std::make_unique<ChildProcess>(
            ASTERION_DATA_SERVICE_PATH,
            std::vector<std::string>{
                "--directory",
                warehouse.path.string(),
                "--session",
                "data-one",
                "--endpoint",
                endpoint,
                "--internal-endpoint",
                internal
            });
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (!fs::exists(endpoint) && !process->exited()
            && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(10ms);
        if (!fs::exists(endpoint))
            throw std::runtime_error("isolated Data did not listen before deadline");
        return process;
    };
    auto process = start();
    protocol::DataClient data(endpoint, "data-one");
    await_ready(data);
    data::proto::DataRequest get;
    get.mutable_acquisition_task()->mutable_get()->set_id(id);
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (data.call(get).acquisition_task().task().state() == task::proto::PUBLISHING
        && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(20ms);
    const auto completed = data.call(get).acquisition_task().task();
    ASSERT_EQ(completed.state(), task::proto::SUCCEEDED);
    EXPECT_EQ(completed.attempt(), 1);
    EXPECT_EQ(completed.publication().SerializeAsString(), decision.SerializeAsString());
    protocol::DataClient worker(internal, "data-one");
    data::proto::DataRequest published;
    *published.mutable_published_download() = decision.identity();
    const auto receipt = worker.call(published).published_download();
    EXPECT_EQ(receipt.decision().SerializeAsString(), decision.SerializeAsString());
    EXPECT_EQ(receipt.record().minute_result().manifest_sha256(), dataset_id);
    data::proto::DataRequest result;
    result.mutable_acquisition_task()->mutable_result()->set_id(id);
    const auto evidence = data.call(result).acquisition_task();
    EXPECT_EQ(evidence.minutes().manifest_sha256(), dataset_id);
    process.reset();
    // Only this isolated child's sockets may need removal after termination.
    for (const auto& socket : {endpoint, internal})
        if (fs::is_socket(socket))
            fs::remove(socket);
    process = start();
    await_ready(data);
    EXPECT_EQ(
        data.call(result).acquisition_task().SerializeAsString(),
        evidence.SerializeAsString());
    EXPECT_EQ(data.call(get).acquisition_task().task().attempt(), 1);
}

TEST(DataInputs, ExecutionsReadFixedVersionsAndResultsRemainReadableWithoutData)
{
    using namespace std::chrono_literals;
    Folder tasks, warehouse, sockets;
    {
        data::Store data(warehouse.path, "data-one");
    }
    // The contract's days in the market-wide daily table.
    const auto daily = protocol::encode_daily_table(
        test::seed_daily_table(
            warehouse.path,
            "SHFE/rb/2026-10",
            every_day("2026-09-01", "2026-10-10"),
            110,
            100));
    const auto data_endpoint = (sockets.path / "data.sock").string();
    const auto data_workers = (sockets.path / "data.workers").string();
    const auto task_endpoint = (sockets.path / "task.sock").string();
    auto data_process = std::make_unique<ChildProcess>(
        ASTERION_DATA_SERVICE_PATH,
        std::vector<std::string>{
            "--directory",
            warehouse.path.string(),
            "--session",
            "data-one",
            "--node-id",
            "fixture-node",
            "--endpoint",
            data_endpoint,
            "--internal-endpoint",
            data_workers
        });
    testing_support::DataDiscovery discovery(
        task_endpoint + ".discovery",
        "data-one",
        data_endpoint);
    ChildProcess task_process(
        ASTERION_RESEARCH_PATH,
        {"--config",
            (std::filesystem::path(ASTERION_RESEARCH_PATH).parent_path() / "config/research.json")
                .string(),
            "--directory",
            tasks.path.string(),
            "--session",
            "tasks-one",
            "--endpoint",
            task_endpoint,
            "--node-id",
            "fixture-node",
            "--discovery-endpoint",
            task_endpoint + ".discovery"});
    discovery.research("tasks-one", task_process.id(), sha256_file(ASTERION_RESEARCH_PATH));
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while ((!fs::exists(data_endpoint) || !fs::exists(task_endpoint)) && !data_process->exited()
        && !task_process.exited() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
    ASSERT_TRUE(fs::exists(data_endpoint));
    ASSERT_TRUE(fs::exists(task_endpoint));
    auto call = [&](research::proto::ResearchRequest request) {
        request.set_version(1);
        request.set_service_id("tasks-one");
        request.set_correlation_id(unique_process_id());
        auto channel = ipc::Channel::connect(task_endpoint, 2s);
        channel.send(request.SerializeAsString(), 2s);
        task::proto::TaskResponse reply;
        // A submission reads its data from Data first; sanitizer builds under
        // parallel load take seconds for it.
        if (!reply.ParseFromString(channel.receive(30s)) || reply.has_error())
            throw std::runtime_error(reply.DebugString());
        return reply;
    };
    // Daily bars are the table's: the selection names no archive version.
    data::proto::BarDatasetRequest selection;
    selection.set_begin_day("2026-09-01");
    selection.set_end_day("2026-10-10");
    *selection.mutable_daily() = daily;
    selection.set_contract("SHFE/rb/2026-10");
    protocol::DataClient data(data_endpoint, "data-one");
    await_ready(data);
    data::proto::DataRequest query;
    *query.mutable_bar_dataset() = selection;
    const auto expected = data.call(query).bar_dataset();
    ASSERT_EQ(expected.bars_size(), 40);

    data::proto::NamedDataset saved;
    saved.set_name("fixed-inputs");
    *saved.add_selections() = selection;
    saved.add_content_revisions(expected.revision());
    saved.set_id(protocol::named_dataset_revision(saved));
    query.Clear();
    *query.mutable_save_dataset() = saved;
    ASSERT_EQ(data.call(query).saved_dataset().id(), saved.id());

    research::proto::ResearchRequest submit;
    submit.mutable_submit()->set_id("backtest");
    submit.mutable_submit()->mutable_data_owner()->set_service_id("data-one");
    auto* backtest = submit.mutable_submit()->mutable_backtest();
    auto* contract = backtest->add_contracts();
    *contract->mutable_data() = selection;
    auto* cost = contract->mutable_cost_schedule()->add_versions();
    cost->set_effective_from("2026-01-01");
    cost->set_source("fixture");
    auto* values = cost->mutable_values();
    for (auto* value :
        {values->mutable_margin_per_lot(),
            values->mutable_open_fee(),
            values->mutable_close_today_fee(),
            values->mutable_close_yesterday_fee(),
            values->mutable_margin_rate(),
            values->mutable_open_fee_rate(),
            values->mutable_close_today_fee_rate(),
            values->mutable_close_yesterday_fee_rate()})
        value->set_units(0);
    values->mutable_margin_per_lot()->set_units(Decimal::parse("100").raw());
    backtest->mutable_deposit()->set_units(Decimal::parse("10000").raw());
    backtest->mutable_risk()->mutable_max_order_quantity()->set_units(Decimal::parse("100").raw());
    backtest->mutable_risk()->mutable_max_gross_quantity()->set_units(Decimal::parse("100").raw());
    backtest->mutable_risk()->set_max_working_orders(100);
    *backtest->add_strategies() = protocol::encode_strategy(
        {{"quantity", "1"},
            {"sides", "long"},
            {"rule", {{"kind", "moving_average"}, {"fast", 1}, {"slow", 3}}}});
    ASSERT_EQ(call(submit).task().kind(), task::proto::BACKTEST);
    submit.mutable_submit()->set_id("factor");
    auto* factor = submit.mutable_submit()->mutable_factor_request();
    *factor->add_series()->mutable_bars() = selection;
    factor->set_factor(protocol::proto::PRICE_MOMENTUM);
    factor->add_lookbacks(1);
    factor->set_horizon(1);
    factor->set_full_sample(true);
    ASSERT_EQ(call(submit).task().kind(), task::proto::FACTOR);

    for (const auto* id : {"backtest", "factor"})
        ASSERT_EQ(
            testing_support::wait_task(
                [&](auto request) {
                    return call(request);
                },
                id)
                .state(),
            task::proto::SUCCEEDED);
    data_process.reset();
    fs::rename(warehouse.path / "history", warehouse.path / "offline-history");
    research::proto::ResearchRequest get;
    get.mutable_get()->set_id("backtest");
    EXPECT_EQ(
        call(get).task().input().paper().contracts(0).dataset().SerializeAsString(),
        expected.SerializeAsString());
    get.mutable_get()->set_id("factor");
    EXPECT_EQ(
        call(get).task().factor().series(0).bars().SerializeAsString(),
        expected.SerializeAsString());
    research::proto::ResearchRequest result;
    for (const auto* id : {"backtest", "factor"}) {
        result.mutable_result()->set_id(id);
        EXPECT_EQ(call(result).result_task().state(), task::proto::SUCCEEDED);
    }
}

TEST(DataInputs, PrivateDatasetReplySupportsTheMaximumTaskInput)
{
    using namespace std::chrono;
    constexpr std::int64_t minute_ns = 60'000'000'000;
    constexpr unsigned count = 200000;
    const auto begin =
        duration_cast<nanoseconds>(sys_days(year(2026) / January / 1).time_since_epoch()).count();
    Minutes minutes;
    Folder warehouse, sockets;
    data::proto::DataRequest query;
    auto* selection = query.mutable_bar_dataset();
    selection->set_contract("SHFE/rb/2026-10");
    {
        data::Store store(warehouse.path, "historical-data");
        data::proto::MinuteDownload request;
        request.set_contract_id("SHFE/rb/2026-10");
        request.set_source("tushare.ft_mins");
        request.set_source_instrument("RB2610.SHF");
        request.set_interval_minutes(1);
        request.set_begin_ns(begin);
        request.set_end_ns(begin + (count - 1) * minute_ns);
        request.set_requests_per_minute(100);
        const auto source = test::publish_download(store, "minutes", request, minutes);
        selection->add_source_dataset_ids(source.minute_result().manifest_sha256());
    }
    *selection->mutable_daily() = protocol::encode_daily_table(
        test::seed_daily_table(
            warehouse.path,
            "SHFE/rb/2026-10",
            every_day("2026-01-01", "2026-05-20"),
            100,
            100));
    const auto endpoint = (sockets.path / "data.sock").string();
    const auto worker = (sockets.path / "data.workers").string();
    ChildProcess process(
        ASTERION_DATA_SERVICE_PATH,
        {"--directory",
            warehouse.path.string(),
            "--session",
            "historical-data",
            "--endpoint",
            endpoint,
            "--internal-endpoint",
            worker,
            "--file-workers",
            "1"});
    const auto deadline = steady_clock::now() + 10s;
    while (!fs::exists(worker) && !process.exited() && steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
    ASSERT_TRUE(fs::exists(worker));
    protocol::DataClient coordinator(worker, "historical-data");
    await_ready(coordinator);
    const auto reply = coordinator.call(query);
    const auto& input = reply.bar_dataset();
    ASSERT_EQ(input.bars_size(), count);
    EXPECT_EQ(input.bars(0).timestamp_ns(), begin);
    EXPECT_EQ(input.bars(count - 1).timestamp_ns(), begin + (count - 1) * minute_ns);
    EXPECT_GT(reply.ByteSizeLong(), 4 * 1024 * 1024)
        << "Data's private endpoint carries bulk replies, not only small control frames";
}

namespace {

asterion::data::proto::DataOwner retention_owner()
{
    data::proto::DataOwner owner;
    owner.set_node_id("fixture-node");
    owner.set_service_id("historical-data");
    return owner;
}

data::proto::DataRetention retained_input(const Json& selected, const std::string& id = "run-one")
{
    data::proto::DataRetention value;
    *value.mutable_source() = retention_owner();
    auto* consumer = value.mutable_consumer();
    consumer->mutable_owner()->set_node_id("fixture-node");
    consumer->mutable_owner()->set_service_id("research-one");
    consumer->mutable_owner()->set_domain("research");
    consumer->set_kind("task");
    consumer->set_id(id);
    consumer->set_version(std::string(64, 'a'));
    for (const auto& id : selected.at("source_dataset_ids")) {
        auto* version = value.add_versions();
        version->set_kind(data::proto::DataVersion::MINUTE_VERSION);
        version->set_version(id.get<std::string>());
    }
    auto* snapshot = value.add_versions();
    snapshot->set_kind(data::proto::DataVersion::TABLE_SNAPSHOT);
    snapshot->set_version(selected.at("daily").at("snapshot").get<std::string>());
    return value;
}

} // namespace

TEST(DataRetentions, IdempotentPreparationAndConfirmationKeepFixedInputsAcrossReopen)
{
    Folder directory;
    const auto selected = test::seed_history(directory.path, {100, 101, 102}, "retained");
    const auto input = retained_input(selected);
    {
        data::Store store(directory.path, "historical-data");
        data::Retentions retentions(directory.path, retention_owner(), store);
        const auto prepared = retentions.prepare(input);
        EXPECT_EQ(prepared.phase(), data::proto::DataRetentionRecord::PREPARED);
        EXPECT_EQ(retentions.prepare(input).SerializeAsString(), prepared.SerializeAsString());
        EXPECT_EQ(retentions.holds(input.versions(0)).size(), 1U);
        const auto confirmed = retentions.confirm(input);
        EXPECT_EQ(confirmed.phase(), data::proto::DataRetentionRecord::CONFIRMED);
        EXPECT_EQ(retentions.confirm(input).SerializeAsString(), confirmed.SerializeAsString());
    }
    data::Store restored(directory.path, "historical-data");
    data::Retentions retentions(directory.path, retention_owner(), restored);
    EXPECT_EQ(
        retentions.get(input.consumer()).phase(),
        data::proto::DataRetentionRecord::CONFIRMED);
    EXPECT_EQ(retentions.holds(input.versions(0)).size(), 1U);
    EXPECT_EQ(retentions.holds(input.versions(1)).size(), 1U);
}

TEST(DataRetentions, ChangedConsumerInputOrSourceCannotOverwriteAndMissingVersionsNeverRetain)
{
    Folder directory;
    const auto selected = test::seed_history(directory.path, {100, 101, 102}, "retained");
    const auto input = retained_input(selected);
    data::Store store(directory.path, "historical-data");
    data::Retentions retentions(directory.path, retention_owner(), store);
    const auto original = retentions.prepare(input);
    auto changed = input;
    changed.mutable_consumer()->set_version(std::string(64, 'b'));
    EXPECT_THROW(retentions.prepare(changed), Error);
    EXPECT_THROW(retentions.confirm(changed), Error);
    changed = input;
    changed.mutable_source()->set_node_id("another-node");
    EXPECT_THROW(retentions.prepare(changed), Error);
    changed = retained_input(selected, "missing-version");
    changed.mutable_versions(1)->set_version(std::string(64, 'f'));
    EXPECT_THROW(retentions.prepare(changed), Error);
    EXPECT_THROW(retentions.get(changed.consumer()), Error);
    EXPECT_EQ(retentions.get(input.consumer()).SerializeAsString(), original.SerializeAsString());
    EXPECT_EQ(retentions.holds(input.versions(0)).size(), 1U);
}

TEST(DataRetentions, CommitFailureRollsBackAndPreparedHoldSurvivesLostAcknowledgement)
{
    Folder directory;
    const auto selected = test::seed_history(directory.path, {100, 101, 102}, "retained");
    const auto input = retained_input(selected);
    {
        data::Store store(directory.path, "historical-data");
        data::Retentions retentions(directory.path, retention_owner(), store);
        sqlite::fail_next_commits_for_testing(1);
        EXPECT_THROW(retentions.prepare(input), std::runtime_error);
        EXPECT_THROW(retentions.get(input.consumer()), Error);
        EXPECT_TRUE(retentions.holds(input.versions(0)).empty());
        (void)retentions.prepare(input); // Committed but its reply is deliberately discarded.
    }
    {
        data::Store store(directory.path, "historical-data");
        data::Retentions retentions(directory.path, retention_owner(), store);
        EXPECT_EQ(
            retentions.get(input.consumer()).phase(),
            data::proto::DataRetentionRecord::PREPARED);
        EXPECT_EQ(retentions.holds(input.versions(0)).size(), 1U);
        sqlite::fail_next_commits_for_testing(1);
        EXPECT_THROW(retentions.confirm(input), std::runtime_error);
        EXPECT_EQ(
            retentions.get(input.consumer()).phase(),
            data::proto::DataRetentionRecord::PREPARED);
        EXPECT_EQ(retentions.confirm(input).phase(), data::proto::DataRetentionRecord::CONFIRMED);
    }
}

TEST(DataRetentions, WrongOwnerCannotReopenOrRewriteExistingRetentionMetadata)
{
    Folder directory;
    const auto selected = test::seed_history(directory.path, {100, 101, 102}, "retained");
    {
        data::Store store(directory.path, "historical-data");
        data::Retentions retentions(directory.path, retention_owner(), store);
        (void)retentions.prepare(retained_input(selected));
    }
    const auto database = directory.path / "retentions.sqlite";
    const auto before = sha256_file(database);
    {
        data::Store store(directory.path, "historical-data");
        auto owner = retention_owner();
        owner.set_node_id("another-node");
        EXPECT_THROW(
            data::Retentions rejected(directory.path, owner, store),
            std::invalid_argument);
    }
    EXPECT_EQ(sha256_file(database), before);
}

TEST(DataRetentions, OwnerReleaseProofIsRequiredAndReleasedReferencesCannotResurrect)
{
    Folder directory;
    const auto selected = test::seed_history(directory.path, {100, 101, 102}, "retained");
    const auto input = retained_input(selected);
    {
        data::Store store(directory.path, "historical-data");
        data::Retentions retentions(directory.path, retention_owner(), store);
        (void)retentions.prepare(input);
        resource::proto::ReleaseDecision proof;
        *proof.mutable_object() = input.consumer();
        EXPECT_THROW(retentions.release(input.consumer(), proof), Error);
        proof.set_state(resource::proto::ReleaseDecision::RETAINED);
        EXPECT_THROW(retentions.release(input.consumer(), proof), Error);
        proof.set_state(resource::proto::ReleaseDecision::RELEASED);
        proof.mutable_object()->set_version(std::string(64, 'b'));
        EXPECT_THROW(retentions.release(input.consumer(), proof), Error);
        EXPECT_EQ(retentions.holds(input.versions(0)).size(), 1U);
        *proof.mutable_object() = input.consumer();
        sqlite::fail_next_commits_for_testing(1);
        EXPECT_THROW(retentions.release(input.consumer(), proof), std::runtime_error);
        EXPECT_EQ(retentions.holds(input.versions(0)).size(), 1U);
        EXPECT_EQ(
            retentions.release(input.consumer(), proof).phase(),
            data::proto::DataRetentionRecord::RELEASED);
        EXPECT_TRUE(retentions.holds(input.versions(0)).empty());
        EXPECT_EQ(
            retentions.release(input.consumer(), proof).phase(),
            data::proto::DataRetentionRecord::RELEASED);
        EXPECT_THROW(retentions.prepare(input), Error);
        EXPECT_THROW(retentions.confirm(input), Error);
    }
    data::Store restored(directory.path, "historical-data");
    data::Retentions retentions(directory.path, retention_owner(), restored);
    EXPECT_EQ(retentions.get(input.consumer()).phase(), data::proto::DataRetentionRecord::RELEASED);
    EXPECT_TRUE(retentions.holds(input.versions(0)).empty());
    EXPECT_EQ(
        restored.archive().get(input.versions(0).version()).minute_result().manifest_sha256(),
        input.versions(0).version()); // Releasing a consumer is not deleting Data's object.
}

TEST(DataRetentions, MissingAuthorityCannotBeRecreatedAsAnEmptyLedger)
{
    Folder directory;
    const auto selected = test::seed_history(directory.path, {100, 101, 102}, "retained");
    {
        data::Store store(directory.path, "historical-data");
        data::Retentions retentions(directory.path, retention_owner(), store);
        (void)retentions.prepare(retained_input(selected));
    }
    ASSERT_TRUE(fs::exists(directory.path / "retentions.required.pb"));
    ASSERT_TRUE(fs::remove(directory.path / "retentions.sqlite"));
    data::Store store(directory.path, "historical-data");
    EXPECT_THROW(
        data::Retentions rejected(directory.path, retention_owner(), store),
        std::runtime_error);
    EXPECT_FALSE(fs::exists(directory.path / "retentions.sqlite"));
    const auto input = retained_input(selected);
    EXPECT_EQ(
        store.archive().get(input.versions(0).version()).minute_result().manifest_sha256(),
        input.versions(0).version());
}

namespace {

class RetirementProcess
{
    fs::path root_;
    std::string endpoint_;
    std::unique_ptr<ChildProcess> process_;

public:
    explicit RetirementProcess(const fs::path& root)
        : root_(root),
          endpoint_((root / "data.sock").string()),
          client(endpoint_, "historical-data")
    {
        start();
    }

    void start()
    {
        process_ = std::make_unique<ChildProcess>(
            ASTERION_DATA_SERVICE_PATH,
            std::vector<std::string>{
                "--directory",
                root_.string(),
                "--session",
                "historical-data",
                "--node-id",
                "fixture-node",
                "--endpoint",
                endpoint_,
                "--internal-endpoint",
                endpoint_ + ".workers"
            });
        using namespace std::chrono_literals;
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        for (;;) {
            try {
                await_ready(client);
                return;
            } catch (const Error& error) {
                if (error.code() != ErrorCode::unavailable || process_->exited()
                    || std::chrono::steady_clock::now() >= deadline)
                    throw;
                std::this_thread::sleep_for(10ms);
            }
        }
    }

    void restart()
    {
        process_.reset();
        start();
    }

    protocol::DataClient client;
};

data::proto::DataRequest retire_minute(const std::string& id, bool execute)
{
    data::proto::DataRequest request;
    request.mutable_minute_retirement()->set_id(id);
    request.mutable_minute_retirement()->set_execute(execute);
    return request;
}

std::string minute_manifest(const data::proto::HistoryRecord& record)
{
    std::ifstream input(
        fs::path(record.minute_result().directory()) / "minutes.json",
        std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

} // namespace

TEST(DataMinuteRetirement, PreviewThenConcurrentReferenceAndDeletionHaveOnlyOneWinner)
{
    using namespace std::chrono_literals;
    Folder directory;
    const auto selected = test::seed_history(directory.path, {100, 101, 102}, "retire-race");
    const auto pin = retained_input(selected);
    const auto id = pin.versions(0).version();
    RetirementProcess data(directory.path);
    const auto preview = data.client.call(retire_minute(id, false)).minute_retirement();
    ASSERT_EQ(preview.state(), data::proto::MinuteRetirement::AVAILABLE);
    ASSERT_TRUE(preview.has_payload_bytes());
    EXPECT_GT(preview.payload_bytes(), 0U);
    data::proto::DataRequest prepare;
    *prepare.mutable_retention()->mutable_prepare() = pin;
    std::barrier start(3);
    auto retaining = std::async(std::launch::async, [&] {
        start.arrive_and_wait();
        try {
            (void)data.client.call(prepare, 10s);
            return true;
        } catch (const Error&) {
            return false;
        }
    });
    auto deleting = std::async(std::launch::async, [&] {
        start.arrive_and_wait();
        return data.client.call(retire_minute(id, true), 10s).minute_retirement();
    });
    start.arrive_and_wait();
    const auto result = deleting.get();
    const bool retained = retaining.get();
    if (retained) {
        ASSERT_EQ(result.state(), data::proto::MinuteRetirement::BLOCKED);
        ASSERT_EQ(result.holds_size(), 1);
        EXPECT_EQ(
            data.client.call(retire_minute(id, true)).minute_retirement().state(),
            data::proto::MinuteRetirement::BLOCKED);
        data::proto::DataRequest record;
        record.mutable_record()->set_id(id);
        EXPECT_TRUE(fs::exists(data.client.call(record).record().minute_result().directory()));
    } else {
        ASSERT_EQ(result.state(), data::proto::MinuteRetirement::REMOVED);
        EXPECT_THROW(data.client.call(prepare), Error);
        EXPECT_EQ(
            data.client.call(retire_minute(id, true)).minute_retirement().state(),
            data::proto::MinuteRetirement::REMOVED);
    }
    data.restart();
    EXPECT_EQ(
        data.client.call(retire_minute(id, false)).minute_retirement().state(),
        result.state());
    data::proto::DataRequest heartbeat;
    heartbeat.mutable_heartbeat();
    EXPECT_FALSE(data.client.call(heartbeat).health().recovery_required());
}

TEST(DataMinuteRetirement, NamedDatasetBlocksDeletionAndDoesNotLosePayload)
{
    Folder directory;
    const auto selected = test::seed_history(directory.path, {100, 101, 102}, "retire-named");
    const auto pin = retained_input(selected);
    RetirementProcess data(directory.path);
    data::proto::DataRequest query;
    auto* selection = query.mutable_bar_dataset();
    selection->set_contract("SHFE/rb/2026-10");
    selection->set_begin_day("2026-09-25");
    selection->set_end_day("2026-09-25");
    *selection->mutable_daily() = protocol::encode_daily_table(selected.at("daily"));
    selection->add_source_dataset_ids(pin.versions(0).version());
    const auto resolved = data.client.call(query).bar_dataset();
    data::proto::NamedDataset saved;
    saved.set_name("retained selection");
    *saved.add_selections() = *selection;
    saved.add_content_revisions(resolved.revision());
    saved.set_id(protocol::named_dataset_revision(saved));
    query.Clear();
    *query.mutable_save_dataset() = saved;
    (void)data.client.call(query);
    const auto result =
        data.client.call(retire_minute(pin.versions(0).version(), true)).minute_retirement();
    ASSERT_EQ(result.state(), data::proto::MinuteRetirement::BLOCKED);
    ASSERT_EQ(result.internal_usage().references_size(), 1);
    EXPECT_EQ(result.internal_usage().references(0).id(), saved.id());
    data::proto::DataRequest still_readable;
    still_readable.mutable_record()->set_id(pin.versions(0).version());
    EXPECT_TRUE(
        fs::is_directory(data.client.call(still_readable).record().minute_result().directory()));
}

TEST(DataMinuteRetirement, DurablePendingSealResumesThroughPublicServiceAfterRestart)
{
    Folder directory;
    const auto selected = test::seed_history(directory.path, {100, 101, 102}, "retire-resume");
    const auto pin = retained_input(selected);
    const auto id = pin.versions(0).version();
    fs::path payload;
    {
        data::Store store(directory.path, "historical-data");
        const auto record = store.archive().get(id);
        payload = record.minute_result().directory();
        store.begin_minute_retirement(record, minute_manifest(record));
        EXPECT_THROW(store.archive().get(id), Error);
        history_files::Archive archive(directory.path / "history");
        EXPECT_THROW(archive.publish_verified(record), Error);
        data::Retentions retentions(directory.path, retention_owner(), store);
        EXPECT_THROW(retentions.prepare(pin), Error);
    }
    RetirementProcess data(directory.path);
    EXPECT_EQ(
        data.client.call(retire_minute(id, false)).minute_retirement().state(),
        data::proto::MinuteRetirement::PENDING);
    data::proto::DataRequest inventory;
    inventory.mutable_minute_retirements()->set_limit(1);
    const auto pending = data.client.call(inventory).minute_retirements();
    ASSERT_EQ(pending.items_size(), 1);
    EXPECT_EQ(pending.items(0).id(), id);
    EXPECT_EQ(pending.items(0).state(), data::proto::MinuteRetirement::PENDING);
    EXPECT_TRUE(pending.next_after().empty());
    inventory.mutable_minute_retirements()->set_after(id);
    EXPECT_EQ(data.client.call(inventory).minute_retirements().items_size(), 0);

    EXPECT_EQ(
        data.client.call(retire_minute(id, true)).minute_retirement().state(),
        data::proto::MinuteRetirement::REMOVED);
    EXPECT_FALSE(fs::exists(payload));
    data.restart();
    EXPECT_EQ(
        data.client.call(retire_minute(id, true)).minute_retirement().state(),
        data::proto::MinuteRetirement::REMOVED);
    data::proto::DataRequest prepare;
    *prepare.mutable_retention()->mutable_prepare() = pin;
    EXPECT_THROW(data.client.call(prepare), Error);
}

TEST(DataMinuteRetirement, InterruptedCleanupAndUncertainCompletionKeepSealAndAuthorityIsRequired)
{
    Folder directory;
    const auto selected = test::seed_history(directory.path, {100, 101, 102}, "retire-interrupted");
    const auto id = retained_input(selected).versions(0).version();
    {
        data::Store store(directory.path, "historical-data");
        const auto record = store.archive().get(id);
        store.begin_minute_retirement(record, minute_manifest(record));
        std::stop_source stop;
        stop.request_stop();
        EXPECT_THROW(store.clean_minute_retirement(id, stop.get_token()), Error);
        EXPECT_TRUE(fs::exists(record.minute_result().directory()));
        EXPECT_FALSE(store.archive().retirement(id)->complete());
    }
    {
        data::Store store(directory.path, "historical-data");
        store.clean_minute_retirement(id, {});
        sqlite::fail_next_commits_for_testing(1);
        EXPECT_THROW(store.finish_minute_retirement(id), std::runtime_error);
        EXPECT_FALSE(store.archive().retirement(id)->complete());
    }
    {
        data::Store store(directory.path, "historical-data");
        store.finish_minute_retirement(id);
        EXPECT_TRUE(store.archive().retirement(id)->complete());
    }

    {
        sqlite::Database read(
            directory.path / "history" / "retirements.sqlite",
            sqlite::Database::Access::read_only);
        sqlite::Database::Statement evidence(read, "SELECT manifest FROM retirements WHERE id=?");
        evidence.bind(1, id);
        ASSERT_TRUE(evidence.step());
        EXPECT_EQ(sha256_bytes(evidence.blob(0)), id);
    }
    ASSERT_TRUE(fs::remove(directory.path / "history" / "retirements.sqlite"));
    EXPECT_THROW(data::Store rejected(directory.path, "historical-data"), std::runtime_error);
    EXPECT_FALSE(fs::exists(directory.path / "history" / "retirements.sqlite"));
    EXPECT_TRUE(fs::exists(directory.path / "history" / "index" / (id + ".pb")));
}

TEST(DataMinuteRetirement, PreparedReferenceBlocksDeletionEvenWhenConsumerIsOffline)
{
    Folder directory;
    const auto selected = test::seed_history(directory.path, {100, 101, 102}, "retire-held");
    const auto pin = retained_input(selected);
    {
        data::Store store(directory.path, "historical-data");
        data::Retentions retentions(directory.path, retention_owner(), store);
        (void)retentions.prepare(pin);
    }
    RetirementProcess data(directory.path);
    const auto result =
        data.client.call(retire_minute(pin.versions(0).version(), true)).minute_retirement();
    ASSERT_EQ(result.state(), data::proto::MinuteRetirement::BLOCKED);
    ASSERT_EQ(result.holds_size(), 1);
    EXPECT_EQ(result.holds(0).phase(), data::proto::DataRetentionRecord::PREPARED);
    EXPECT_EQ(
        result.holds(0).retention().consumer().SerializeAsString(),
        pin.consumer().SerializeAsString());
    EXPECT_GT(result.payload_bytes(), 0U);
}

TEST(DataMinuteRetirement, IndependentCopyResumesOnlyItsOwnSealedPayloadAndKeepsOriginal)
{
    Folder source, restored;
    const auto selected = test::seed_history(source.path, {100, 101, 102}, "retire-portable");
    const auto id = retained_input(selected).versions(0).version();
    fs::path original_payload;
    {
        data::Store store(source.path, "historical-data");
        const auto record = store.archive().get(id);
        original_payload =
            fs::path(record.minute_result().directory()).lexically_relative(source.path);
        store.begin_minute_retirement(record, minute_manifest(record));
    }
    const auto copy = restored.path / "copy";
    fs::copy(source.path, copy, fs::copy_options::recursive);
    const auto preserved =
        source.path.parent_path() / (source.path.filename().string() + "-preserved");
    fs::rename(source.path, preserved);
    // Transfer cleanup responsibility to Folder; the original remains inaccessible at its old locator.
    source.path = preserved;
    {
        data::Store store(copy, "historical-data");
        const auto decision = store.archive().retirement(id);
        ASSERT_TRUE(decision.has_value());
        EXPECT_EQ(
            fs::path(decision->record().minute_result().directory()),
            copy / original_payload);
        EXPECT_FALSE(decision->complete());
        store.clean_minute_retirement(id, {});
        store.finish_minute_retirement(id);
        EXPECT_TRUE(store.archive().retirement(id)->complete());
        EXPECT_FALSE(fs::exists(copy / original_payload));
    }
    data::Store original(source.path, "historical-data");
    EXPECT_FALSE(original.archive().retirement(id)->complete());
    EXPECT_TRUE(fs::is_directory(source.path / original_payload));
}
