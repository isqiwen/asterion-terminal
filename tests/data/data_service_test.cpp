#include "data_store.hpp"
#include "data/history_requests.hpp"
#include "download_budget.hpp"
#include "data/data_fixture.hpp"
#include "task_store.hpp"
#include "tasks/task_store_support.hpp"
#include "data/bar_fixture.hpp"
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/ipc/local_channel.hpp>
#include "history_daily.hpp"
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/data_client.hpp>
#include <gtest/gtest.h>
#include <fstream>
#include <thread>
#include <array>
using namespace asterion;
namespace fs = std::filesystem;
namespace {
struct Folder {
  fs::path path = fs::path("/tmp") / ("asterion-data-" + unique_process_id());
  Folder() { fs::create_directory(path); }
  ~Folder() {
    std::error_code error;
    fs::remove_all(path, error);
  }
};
struct Daily : HistoricalDailyPort {
  explicit Daily(std::string source = "fixture.daily") : source(std::move(source)) {}
  std::string source;
  HistorySemantics semantics() const override {
    return {source, "fixture.v1", "Asia/Shanghai", "trading_day"};
  }
  std::vector<HistoricalDailyBar> read(const HistoricalDailyRange& range,
                                       std::stop_token) override {
    std::vector<HistoricalDailyBar> bars;
    for (auto day = std::chrono::sys_days(range.begin); day <= std::chrono::sys_days(range.end);
         day += std::chrono::days(1)) {
      const auto price = Decimal::parse(std::to_string(10 + bars.size()));
      bars.push_back({std::chrono::year_month_day(day),
                      price,
                      price,
                      price,
                      price,
                      price,
                      price,
                      price,
                      {},
                      {},
                      price});
    }
    return bars;
  }
};
data::v1::DownloadAllocation allocation() {
  data::v1::DownloadAllocation request;
  auto* identity = request.mutable_identity();
  identity->set_data_instance("data-one");
  identity->set_task_instance("tasks-one");
  identity->set_task_id("download-one");
  identity->set_attempt(1);
  *request.mutable_daily() =
      asterion::testing_support::daily_request({{"version", 2},
                                                {"contract_id", "SHFE/rb/2026-10"},
                                                {"source", "tushare.fut_daily"},
                                                {"source_instrument", "rb2610"},
                                                {"begin_day", "2026-09-25"},
                                                {"end_day", "2026-09-25"},
                                                {"requests_per_minute", 60}});
  return request;
}
data::v1::DownloadPreparation download(const data::v1::DownloadAllocation& request,
                                       const std::string& directory) {
  Daily provider(request.daily().source());
  history_files::download_daily(provider, history_files::daily_range(request.daily()), directory);
  data::v1::DownloadPreparation prepared;
  *prepared.mutable_identity() = request.identity();
  auto* record = prepared.mutable_record();
  record->set_version(1);
  *record->mutable_daily() = request.daily();
  *record->mutable_daily_result() = history_files::daily_result(directory);
  return prepared;
}
void await_ready(protocol::DataClient& client) {
  using namespace std::chrono_literals;
  data::v1::DataRequest health;
  health.mutable_heartbeat();
  const auto until = std::chrono::steady_clock::now() + 10s;
  while (!client.call(health).health().initialized()) {
    ASSERT_LT(std::chrono::steady_clock::now(), until);
    std::this_thread::sleep_for(10ms);
  }
}
void configure_budget(data::DownloadBudget& budget, const std::string& account, unsigned limit) {
  const auto configuration = budget.configure("provider", account, limit);
  budget.persist(configuration);
  budget.committed(configuration);
}
} // namespace
TEST(DownloadBudget, AccountSharesWindowAndChangingLimitPreservesAdmissions) {
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

TEST(DownloadBudget, RestartAndReconfigurationCannotResetTheQuotaWindow) {
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

TEST(DownloadBudget, UncertainPolicyCommitStopsAdmissionsUntilRestart) {
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

TEST(DataAuthorization, CapturesFixedPrivateCredentialsAndRejectsReplacement) {
  Folder root;
  data::v1::DownloadAuthorizationRequest request;
  request.set_task_instance("tasks-one");
  request.set_task_id("download-one");
  request.set_credential("fixture-private-token");
  *request.mutable_daily() = allocation().daily();
  request.mutable_daily()->set_source("tushare.fut_daily");
  request.mutable_daily()->set_source_instrument("RB2610.SHF");
  data::v1::DownloadAuthorization authorization;
  {
    data::Store store(root.path, "data-one", "tasks-one");
    authorization = store.authorize(store.verify_authorization(request));
    EXPECT_EQ(authorization.data_instance(), "data-one");
    EXPECT_EQ(authorization.task_instance(), "tasks-one");
    EXPECT_EQ(authorization.task_id(), "download-one");
    EXPECT_EQ(authorization.daily().SerializeAsString(), request.daily().SerializeAsString());
    EXPECT_EQ(authorization.provider_artifact().size(), 64);
    EXPECT_EQ(authorization.SerializeAsString().find(request.credential()), std::string::npos);
    EXPECT_EQ(store.authorize(store.verify_authorization(request)).SerializeAsString(),
              authorization.SerializeAsString());
    auto changed = request;
    changed.set_credential("different-fixture-token");
    EXPECT_THROW(store.authorize(store.verify_authorization(changed)), Error);
    changed = request;
    changed.mutable_daily()->set_end_day("2026-09-26");
    EXPECT_THROW(store.authorize(store.verify_authorization(changed)), Error);
    changed = request;
    changed.set_task_instance("different-task-service");
    EXPECT_THROW(store.verify_authorization(changed), Error);
    const auto path = root.path / "authorizations" / (authorization.id() + ".pb");
    const auto public_permissions = fs::perms::group_all | fs::perms::others_all;
    EXPECT_EQ(fs::status(path).permissions() & public_permissions, fs::perms::none);
  }
  data::Store restored(root.path, "data-one", "tasks-one");
  EXPECT_EQ(restored.authorization(authorization.id()).SerializeAsString(),
            authorization.SerializeAsString());
}

TEST(DataAuthorization, FailedDurableCaptureCannotBeAcknowledgedUntilItsBarrierCompletes) {
  Folder root;
  data::Store store(root.path, "data-one", "tasks-one");
  data::v1::DownloadAuthorizationRequest request;
  request.set_task_instance("tasks-one");
  request.set_task_id("download-one");
  request.set_credential("fixture-private-token");
  *request.mutable_daily() = allocation().daily();
  request.mutable_daily()->set_source("tushare.fut_daily");
  request.mutable_daily()->set_source_instrument("RB2610.SHF");
  const auto verified = store.verify_authorization(request);
  fail_next_directory_syncs_for_testing(1);
  EXPECT_THROW(store.authorize(verified), std::runtime_error);
  fail_next_directory_syncs_for_testing(0);
  const auto id = sha256_bytes("data-one/tasks-one/download-one");
  fail_next_directory_syncs_for_testing(1);
  EXPECT_THROW(store.authorization(id), std::runtime_error);
  fail_next_directory_syncs_for_testing(0);
  const auto acknowledged = store.authorize(verified);
  EXPECT_EQ(store.authorization(id).SerializeAsString(), acknowledged.SerializeAsString());
}

TEST(DataService, AllocationAndPublicationSurviveAParentPathAliasWithoutRewritingEvidence) {
  Folder root;
  fs::create_directories(root.path / "physical/warehouse");
  fs::create_directory_symlink(root.path / "physical", root.path / "alias");
  auto request = allocation();
  std::string directory;
  fs::path receipt;
  std::string digest;
  {
    data::Store store(root.path / "physical/warehouse", "data-one", "tasks-one");
    directory = test::allocate_download(store, request).directory();
    for (const auto& file : fs::recursive_directory_iterator(root.path / "physical/warehouse"))
      if (file.path().filename() == "allocation.pb")
        receipt = file.path();
    ASSERT_FALSE(receipt.empty());
    digest = sha256_file(receipt);
  }
  data::Store reopened(root.path / "alias/warehouse", "data-one", "tasks-one");
  EXPECT_EQ(test::allocate_download(reopened, request).directory(), directory);
  EXPECT_EQ(sha256_file(receipt), digest);
  const auto prepared = download(request, directory);
  const auto accepted = reopened.prepare(reopened.verify_download(prepared));
  data::v1::DownloadPublication decision;
  *decision.mutable_identity() = request.identity();
  decision.set_candidate_digest(accepted.candidate_digest());
  decision.set_publication_id("publish-one");
  const auto published = reopened.publish(reopened.verify_publication(decision));
  EXPECT_EQ(published.record().SerializeAsString(), prepared.record().SerializeAsString());
  EXPECT_EQ(reopened.archive().get(accepted.dataset_id()).SerializeAsString(),
            prepared.record().SerializeAsString());
  EXPECT_EQ(sha256_file(receipt), digest);
  // A link inside the archive is not a parent alias and must not bypass ownership.
  fs::rename(directory, directory + "-moved");
  fs::create_directory_symlink(directory + "-moved", directory);
  EXPECT_THROW(reopened.archive().get(accepted.dataset_id()), std::invalid_argument);
  EXPECT_THROW(reopened.publish(reopened.verify_publication(decision)), std::invalid_argument);
}

TEST(DataService, PreparationIsPrivateAndPublicationKeepsItsIdentityAcrossRestarts) {
  Folder root;
  auto request = allocation();
  data::v1::DownloadPreparation prepared;
  data::v1::PreparedDownload accepted;
  {
    data::Store store(root.path, "data-one", "tasks-one");
    const auto directory = test::allocate_download(store, request).directory();
    EXPECT_EQ(test::allocate_download(store, request).directory(), directory);
    auto conflict = request;
    conflict.mutable_daily()->set_end_day("2026-09-26");
    EXPECT_THROW(store.allocate(store.verify_allocation(conflict)), Error);
    prepared = download(request, directory);
    accepted = store.prepare(store.verify_download(prepared));
    EXPECT_EQ(store.prepare(store.verify_download(prepared)).SerializeAsString(),
              accepted.SerializeAsString());
    EXPECT_TRUE(store.archive().datasets({}).empty());
  }
  data::v1::DownloadPublication decision;
  *decision.mutable_identity() = request.identity();
  decision.set_candidate_digest(accepted.candidate_digest());
  decision.set_publication_id("publish-one");
  {
    data::Store store(root.path, "data-one", "tasks-one");
    EXPECT_TRUE(store.archive().datasets({}).empty());
    auto wrong = decision;
    wrong.set_candidate_digest(std::string(64, 'a'));
    EXPECT_THROW(store.publish(store.verify_publication(wrong)), Error);
    const auto result = store.publish(store.verify_publication(decision));
    EXPECT_EQ(result.record().daily_result().manifest_sha256(), accepted.dataset_id());
    EXPECT_EQ(store.publish(store.verify_publication(decision)).SerializeAsString(),
              result.SerializeAsString());
    wrong = decision;
    wrong.set_publication_id("different-decision");
    EXPECT_THROW(store.publish(store.verify_publication(wrong)), Error);
  }
  {
    data::Store store(root.path, "data-one", "tasks-one");
    EXPECT_EQ(store.published(request.identity()).decision().publication_id(), "publish-one");
    EXPECT_EQ(store.archive().datasets({}).size(), 1);
  }
  EXPECT_THROW((data::Store(root.path, "another-data", "tasks-one")), std::invalid_argument);
  EXPECT_THROW((data::Store(root.path, "data-one", "another-task")), std::invalid_argument);
}
TEST(DataService, SeparateProcessServesPublishedHistoryWithoutATaskService) {
  using namespace std::chrono_literals;
  Folder root, sockets;
  auto request = allocation();
  data::v1::PreparedDownload accepted;
  {
    data::Store store(root.path, "data-one", "tasks-one");
    accepted = store.prepare(store.verify_download(
        download(request, test::allocate_download(store, request).directory())));
  }
  const auto endpoint = (sockets.path / "data.sock").string();
  const auto worker = (sockets.path / "worker.sock").string();
  auto process = std::make_unique<ChildProcess>(
      ASTERION_DATA_SERVICE_PATH,
      std::vector<std::string>{"--directory", root.path.string(), "--session", "data-one",
                               "--task-instance", "tasks-one", "--endpoint", endpoint,
                               "--worker-endpoint", worker},
      false, sockets.path / "service.log", true);
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (!fs::exists(endpoint) && !process->exited() && std::chrono::steady_clock::now() < deadline)
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
  data::v1::DataRequest capture;
  auto* authorization_request = capture.mutable_authorize_download();
  authorization_request->set_task_instance("tasks-one");
  authorization_request->set_task_id("authorized-download");
  authorization_request->set_credential("fixture-rpc-private-token");
  *authorization_request->mutable_daily() = request.daily();
  authorization_request->mutable_daily()->set_source("tushare.fut_daily");
  authorization_request->mutable_daily()->set_source_instrument("RB2610.SHF");
  data::v1::DataResponse captured;
  {
    std::array<data::v1::DataRequest, 2> requests{capture, capture};
    requests[1].mutable_authorize_download()->set_credential("different-fixture-token");
    std::array<ipc::Channel, 2> channels{ipc::Channel::connect(endpoint, 1s),
                                         ipc::Channel::connect(endpoint, 1s)};
    for (unsigned i = 0; i < requests.size(); ++i) {
      requests[i].set_version(1);
      requests[i].set_service_id("data-one");
      requests[i].set_correlation_id(unique_process_id());
      channels[i].send(requests[i].SerializeAsString(), 1s);
    }
    std::array<data::v1::DataResponse, 2> replies;
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
  EXPECT_EQ(captured.SerializeAsString().find(authorization_request->credential()),
            std::string::npos);
  data::v1::DataRequest admission;
  admission.mutable_download_authorization()->set_id(captured.download_authorization().id());
  EXPECT_THROW(public_client.call(admission), Error);
  EXPECT_EQ(coordinator.call(admission).download_authorization().SerializeAsString(),
            captured.download_authorization().SerializeAsString());
  data::v1::DataRequest credential_request;
  *credential_request.mutable_download_credentials() = request.identity();
  EXPECT_THROW(public_client.call(credential_request), Error);
  EXPECT_EQ(coordinator.call(credential_request).download_credentials().credential(), "fixture");
  data::v1::DataRequest permit;
  *permit.mutable_acquire_download_permit() = request.identity();
  EXPECT_THROW(public_client.call(permit), Error);
  EXPECT_THROW(coordinator.call(permit), Error);
  data::v1::DataRequest budget;
  budget.mutable_configure_download_budget()->set_source("tushare.ft_mins");
  budget.mutable_configure_download_budget()->set_credential("fixture");
  budget.mutable_configure_download_budget()->set_requests_per_minute(1);
  EXPECT_EQ(public_client.call(budget).download_budget().requests_per_minute(), 1);
  // Minute and daily sources of this account share the same provider budget.
  EXPECT_TRUE(coordinator.call(permit).download_permit().granted());
  EXPECT_GT(coordinator.call(permit).download_permit().retry_after_ms(), 0);
  data::v1::DataRequest publish;
  auto* decision = publish.mutable_publish_download();
  *decision->mutable_identity() = request.identity();
  decision->set_candidate_digest(accepted.candidate_digest());
  decision->set_publication_id("publish-one");
  EXPECT_THROW(public_client.call(publish), Error);
  EXPECT_TRUE(coordinator.call(publish).has_published_download());
  data::v1::DataRequest query;
  query.mutable_daily_page()->set_dataset_id(accepted.dataset_id());
  query.mutable_daily_page()->set_limit(10);
  const auto page = public_client.call(query);
  ASSERT_TRUE(page.has_daily_page());
  ASSERT_EQ(page.daily_page().bars_size(), 1);
  EXPECT_EQ(page.daily_page().bars(0).trading_day(), "2026-09-25");
  data::v1::DataRequest heartbeat;
  heartbeat.mutable_heartbeat();
  const auto health = public_client.call(heartbeat).health();
  EXPECT_EQ(health.instance_id(), "data-one");
  EXPECT_FALSE(health.recovery_required());
  process.reset();
  Folder empty_plugins;
  // The original operation remains confirmable after its provider is disabled.
  const auto restored_endpoint = (sockets.path / "restored.sock").string();
  ChildProcess restored(ASTERION_DATA_SERVICE_PATH,
                        {"--directory", root.path.string(), "--session", "data-one",
                         "--task-instance", "tasks-one", "--endpoint", restored_endpoint,
                         "--worker-endpoint", (sockets.path / "restored.workers").string(),
                         "--plugin-directory", empty_plugins.path.string()},
                        false, sockets.path / "restored.log", true);
  const auto restarted_until = std::chrono::steady_clock::now() + 10s;
  while (!fs::exists(restored_endpoint) && !restored.exited() &&
         std::chrono::steady_clock::now() < restarted_until)
    std::this_thread::sleep_for(10ms);
  ASSERT_TRUE(fs::exists(restored_endpoint));
  protocol::DataClient replay(restored_endpoint, "data-one");
  await_ready(replay);
  EXPECT_EQ(replay.call(capture).download_authorization().SerializeAsString(),
            captured.download_authorization().SerializeAsString());
  capture.mutable_authorize_download()->set_task_id("new-download");
  EXPECT_THROW(replay.call(capture), Error);
}

TEST(DownloadPublication, CancellationBeforeDecisionKeepsTheCandidatePrivate) {
  Folder tasks, warehouse;
  data::Store data(warehouse.path, "data-one", "tasks-one");
  tasks::Store store(tasks.path, tasks::Identity{"tasks-one", "data-one"});
  auto request = allocation();
  request.mutable_daily()->set_source("tushare.fut_daily");
  const auto id = request.identity().task_id();
  tasks::submit(store, test::authorize_download(data, "tasks-one", id, request.daily()));
  task::v1::TaskFinish finish;
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

  *finish.mutable_daily() = candidate.record().daily_result();
  auto completion = store.prepare_finish(finish);
  completion.prepare_payload();
  const auto prepared = data.prepare(data.verify_download(candidate));
  EXPECT_EQ(store.commit(store.cancel(id)).task().state(), task::v1::CANCEL_REQUESTED);
  EXPECT_FALSE(
      (store.commit(store.prepare_publication(std::move(completion), prepared)).task().state() ==
       asterion::task::v1::PUBLISHING));
  EXPECT_EQ(store.describe(id).state(), task::v1::CANCELLED);
  EXPECT_TRUE(store.pending_publications().empty());
  EXPECT_TRUE(data.archive().datasets({}).empty());
}

TEST(DownloadPublication, RestartResumesTheOriginalDecisionWithoutAWorkerOrDataStartupDependency) {
  using namespace std::chrono_literals;
  Folder tasks, warehouse, sockets;
  auto request = allocation();
  request.mutable_daily()->set_source("tushare.fut_daily");
  const auto id = request.identity().task_id();
  std::string token, dataset_id;
  data::v1::DownloadPublication decision;
  {
    data::Store data(warehouse.path, "data-one", "tasks-one");
    tasks::Store store(tasks.path, tasks::Identity{"tasks-one", "data-one"});
    tasks::submit(store, test::authorize_download(data, "tasks-one", id, request.daily()));
    tasks::submit(store,
                  test::authorize_download(data, "tasks-one", "queued-download", request.daily()));
    token = store.commit(store.claim(id)).token();
    const auto candidate = download(request, test::allocate_download(data, request).directory());
    const auto prepared = data.prepare(data.verify_download(candidate));
    dataset_id = prepared.dataset_id();
    task::v1::TaskFinish finish;
    finish.set_id(id);
    finish.set_token(token);
    *finish.mutable_daily() = candidate.record().daily_result();
    auto completion = store.prepare_finish(finish);
    completion.prepare_payload();
    ASSERT_TRUE(
        (store.commit(store.prepare_publication(std::move(completion), prepared)).task().state() ==
         asterion::task::v1::PUBLISHING));
    EXPECT_EQ(store.describe(id).state(), task::v1::PUBLISHING);
    EXPECT_EQ(store.commit(store.cancel(id)).task().state(), task::v1::PUBLISHING);
    EXPECT_THROW(store.commit(store.interrupt(id, token, "worker exited")), std::invalid_argument);
    EXPECT_THROW(store.commit(store.retry(id)).task(), std::invalid_argument);
    decision = store.pending_publications().at(0);
    EXPECT_TRUE(data.archive().datasets({}).empty());
  }
  const auto task_endpoint = (sockets.path / "task.sock").string();
  const auto data_endpoint = (sockets.path / "data.sock").string();
  const auto task_workers = (sockets.path / "task.workers").string();
  const auto data_workers = (sockets.path / "data.workers").string();
  ChildProcess coordinator(ASTERION_TASK_SERVICE_PATH,
                           {"--directory", tasks.path.string(), "--session", "tasks-one",
                            "--endpoint", task_endpoint, "--worker-endpoint", task_workers,
                            "--data-instance", "data-one", "--data-endpoint", data_workers},
                           false, sockets.path / "task.log", true);
  auto wait_endpoint = [&](const std::string& endpoint, ChildProcess& process) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!fs::exists(endpoint) && !process.exited() &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(10ms);
    if (!fs::exists(endpoint))
      throw std::runtime_error("fixture service did not initialize");
  };
  wait_endpoint(task_endpoint, coordinator);
  auto call = [&](task::v1::TaskRequest request, bool worker = false) {
    request.set_version(1);
    request.set_service_id("tasks-one");
    request.set_correlation_id(unique_process_id());
    auto channel = ipc::Channel::connect(worker ? task_workers : task_endpoint, 2s);
    channel.send(request.SerializeAsString(), 2s);
    task::v1::TaskResponse response;
    if (!response.ParseFromString(channel.receive(2s)) || response.has_error())
      throw std::runtime_error("task publication fixture request failed");
    return response;
  };
  task::v1::TaskRequest heartbeat;
  heartbeat.mutable_heartbeat();
  const auto ready_deadline = std::chrono::steady_clock::now() + 10s;
  while (!call(heartbeat).health().initialized()) {
    ASSERT_FALSE(coordinator.exited());
    ASSERT_LT(std::chrono::steady_clock::now(), ready_deadline);
    std::this_thread::sleep_for(10ms);
  }
  task::v1::TaskRequest get;
  get.mutable_get()->set_id(id);
  EXPECT_EQ(call(get).task().state(), task::v1::PUBLISHING);
  EXPECT_EQ(call(get).task().publication().SerializeAsString(), decision.SerializeAsString());
  task::v1::TaskRequest cancel;
  cancel.mutable_cancel()->set_id(id);
  EXPECT_EQ(call(cancel).task().state(), task::v1::PUBLISHING);
  task::v1::TaskRequest dispatch;
  dispatch.mutable_dispatch()->set_launch_slots(2);
  EXPECT_EQ(call(dispatch, true).launches().launches_size(), 0);
  // Data can start later. No claim, heartbeat, redownload, or new decision is sent.
  auto data_process = std::make_unique<ChildProcess>(
      ASTERION_DATA_SERVICE_PATH,
      std::vector<std::string>{"--directory", warehouse.path.string(), "--session", "data-one",
                               "--task-instance", "tasks-one", "--endpoint", data_endpoint,
                               "--worker-endpoint", data_workers},
      false, sockets.path / "data.log", true);
  wait_endpoint(data_endpoint, *data_process);
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (call(get).task().state() == task::v1::PUBLISHING &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(20ms);
  ASSERT_EQ(call(get).task().state(), task::v1::SUCCEEDED);
  ASSERT_EQ(call(dispatch, true).launches().launches_size(), 1);
  ASSERT_EQ(call(get).task().publication().SerializeAsString(), decision.SerializeAsString());
  protocol::DataClient data(data_workers, "data-one");
  data::v1::DataRequest published;
  *published.mutable_published_download() = decision.identity();
  const auto receipt = data.call(published).published_download();
  EXPECT_EQ(receipt.decision().SerializeAsString(), decision.SerializeAsString());
  EXPECT_EQ(receipt.record().daily_result().manifest_sha256(), dataset_id);
  data_process.reset();
  // Historical file access belongs to Data. Task still serves its confirmed
  // result evidence while the warehouse process and its files are unavailable.
  fs::rename(warehouse.path / "history", warehouse.path / "offline-history");
  task::v1::TaskRequest result;
  result.mutable_result()->set_id(id);
  EXPECT_EQ(call(result).daily().manifest_sha256(), dataset_id);
}

TEST(DataInputs, WorkersReadFixedVersionsAndResultsRemainReadableWithoutData) {
  using namespace std::chrono_literals;
  Folder tasks, warehouse, sockets;
  auto request = allocation();
  request.mutable_daily()->set_begin_day("2026-09-01");
  request.mutable_daily()->set_end_day("2026-10-10");
  std::string dataset_id;
  {
    data::Store data(warehouse.path, "data-one", "tasks-one");
    const auto prepared = data.prepare(data.verify_download(
        download(request, test::allocate_download(data, request).directory())));
    data::v1::DownloadPublication decision;
    *decision.mutable_identity() = prepared.identity();
    decision.set_candidate_digest(prepared.candidate_digest());
    decision.set_publication_id("fixture-publication");
    data.publish(data.verify_publication(decision));
    dataset_id = prepared.dataset_id();
  }
  const auto data_endpoint = (sockets.path / "data.sock").string();
  const auto data_workers = (sockets.path / "data.workers").string();
  const auto task_endpoint = (sockets.path / "task.sock").string();
  auto data_process = std::make_unique<ChildProcess>(
      ASTERION_DATA_SERVICE_PATH,
      std::vector<std::string>{"--directory", warehouse.path.string(), "--session", "data-one",
                               "--task-instance", "tasks-one", "--endpoint", data_endpoint,
                               "--worker-endpoint", data_workers});
  ChildProcess task_process(ASTERION_TASK_SERVICE_PATH,
                            {"--directory", tasks.path.string(), "--session", "tasks-one",
                             "--endpoint", task_endpoint, "--data-instance", "data-one",
                             "--data-endpoint", data_workers});
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while ((!fs::exists(data_endpoint) || !fs::exists(task_endpoint)) && !data_process->exited() &&
         !task_process.exited() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(10ms);
  ASSERT_TRUE(fs::exists(data_endpoint));
  ASSERT_TRUE(fs::exists(task_endpoint));
  auto call = [&](task::v1::TaskRequest request) {
    request.set_version(1);
    request.set_service_id("tasks-one");
    request.set_correlation_id(unique_process_id());
    auto channel = ipc::Channel::connect(task_endpoint, 2s);
    channel.send(request.SerializeAsString(), 2s);
    task::v1::TaskResponse reply;
    if (!reply.ParseFromString(channel.receive(5s)) || reply.has_error())
      throw std::runtime_error(reply.DebugString());
    return reply;
  };
  data::v1::BarDatasetRequest selection;
  selection.add_source_dataset_ids(dataset_id);
  selection.add_settlement_dataset_ids(dataset_id);
  *selection.mutable_contract() = test::contract();
  protocol::DataClient data(data_endpoint, "data-one");
  await_ready(data);
  data::v1::DataRequest query;
  *query.mutable_bar_dataset() = selection;
  const auto expected = data.call(query).bar_dataset();
  ASSERT_EQ(expected.bars_size(), 40);

  data::v1::NamedDataset saved;
  saved.set_version(1);
  saved.set_name("fixed-inputs");
  *saved.add_selections() = selection;
  saved.add_content_revisions(expected.revision());
  saved.set_id(protocol::named_dataset_revision(saved));
  query.Clear();
  *query.mutable_save_dataset() = saved;
  ASSERT_EQ(data.call(query).saved_dataset().id(), saved.id());
  query.mutable_history_usage()->set_id(dataset_id);
  const auto usage = data.call(query).history_usage();
  ASSERT_EQ(usage.references_size(), 1);
  EXPECT_EQ(usage.references(0).kind(), data::v1::HISTORY_SAVED_DATASET);
  EXPECT_EQ(usage.references(0).id(), saved.id());
  EXPECT_EQ(usage.references(0).roles_size(), 2);

  task::v1::TaskRequest submit;
  submit.mutable_submit()->set_id("backtest");
  auto* backtest = submit.mutable_submit()->mutable_backtest();
  auto* contract = backtest->add_contracts();
  *contract->mutable_data() = selection;
  auto* cost = contract->mutable_cost_schedule()->add_versions();
  cost->set_effective_from("2026-01-01");
  cost->set_source("fixture");
  auto* values = cost->mutable_values();
  for (auto* value :
       {values->mutable_margin_per_lot(), values->mutable_open_fee(),
        values->mutable_close_today_fee(), values->mutable_close_yesterday_fee(),
        values->mutable_margin_rate(), values->mutable_open_fee_rate(),
        values->mutable_close_today_fee_rate(), values->mutable_close_yesterday_fee_rate()})
    value->set_units(0);
  values->mutable_margin_per_lot()->set_units(Decimal::parse("100").raw());
  backtest->mutable_deposit()->set_units(Decimal::parse("10000").raw());
  backtest->mutable_risk()->mutable_max_order_quantity()->set_units(Decimal::parse("100").raw());
  backtest->mutable_risk()->mutable_max_gross_quantity()->set_units(Decimal::parse("100").raw());
  backtest->mutable_risk()->set_max_working_orders(100);
  backtest->mutable_sma()->set_fast(1);
  backtest->mutable_sma()->set_slow(3);
  backtest->mutable_sma()->mutable_quantity()->set_units(Decimal::parse("1").raw());
  backtest->mutable_sma()->set_sides(protocol::v1::LONG_ONLY);
  ASSERT_EQ(call(submit).task().kind(), task::v1::BACKTEST);
  submit.mutable_submit()->set_id("factor");
  auto* factor = submit.mutable_submit()->mutable_factor_request();
  *factor->add_series()->mutable_bars() = selection;
  factor->add_lookbacks(1);
  factor->set_horizon(1);
  factor->set_full_sample(true);
  ASSERT_EQ(call(submit).task().kind(), task::v1::FACTOR);

  // A mismatched executable must not consume the queued factor attempt.
  {
    ChildProcess wrong(ASTERION_BACKTEST_PATH,
                       {"--endpoint", task_endpoint, "--session", "tasks-one", "--task", "factor"});
    ASSERT_TRUE(wrong.wait(10s));
    EXPECT_NE(wrong.exit_code(), 0);
    task::v1::TaskRequest get;
    get.mutable_get()->set_id("factor");
    EXPECT_EQ(call(get).task().state(), task::v1::QUEUED);
    EXPECT_EQ(call(get).task().attempt(), 0U);
  }
  for (const auto& [program, id] :
       {std::pair{ASTERION_BACKTEST_PATH, "backtest"}, std::pair{ASTERION_FACTOR_PATH, "factor"}}) {
    ChildProcess worker(program,
                        {"--endpoint", task_endpoint, "--session", "tasks-one", "--task", id});
    ASSERT_TRUE(worker.wait(15s));
    ASSERT_EQ(worker.exit_code(), 0);
  }
  data_process.reset();
  fs::rename(warehouse.path / "history", warehouse.path / "offline-history");
  task::v1::TaskRequest get;
  get.mutable_get()->set_id("backtest");
  EXPECT_EQ(call(get).task().input().paper().contracts(0).dataset().SerializeAsString(),
            expected.SerializeAsString());
  get.mutable_get()->set_id("factor");
  EXPECT_EQ(call(get).task().factor().series(0).bars().SerializeAsString(),
            expected.SerializeAsString());
  task::v1::TaskRequest result;
  for (const auto* id : {"backtest", "factor"}) {
    result.mutable_result()->set_id(id);
    EXPECT_EQ(call(result).result_task().state(), task::v1::SUCCEEDED);
  }
}

TEST(DataInputs, PrivateDatasetReplySupportsTheMaximumTaskInput) {
  using namespace std::chrono;
  constexpr std::int64_t minute_ns = 60'000'000'000;
  constexpr unsigned count = 200000;
  const auto begin =
      duration_cast<nanoseconds>(sys_days(year(2026) / January / 1).time_since_epoch()).count();
  struct Minutes final : HistoricalBarPort {
    HistorySemantics semantics() const override {
      return {"tushare.ft_mins", "fixture.v1", "Asia/Shanghai", "bar_end"};
    }
    std::vector<HistoricalBar> read(const HistoricalBarRange& range, std::stop_token) override {
      std::vector<HistoricalBar> bars;
      const auto price = Decimal::parse("100");
      for (auto t = range.begin_ns; t <= range.end_ns; t += minute_ns) {
        const auto day =
            year_month_day(floor<days>(sys_time<nanoseconds>(nanoseconds(t)) + hours(8)));
        bars.push_back(
            {t, price, price, price, price, price, price, price, format_trading_date(day)});
      }
      return bars;
    }
  } minutes;
  Folder warehouse, sockets;
  data::v1::DataRequest query;
  auto* selection = query.mutable_bar_dataset();
  *selection->mutable_contract() = test::contract();
  {
    data::Store store(warehouse.path, "historical-data", "task");
    data::v1::MinuteDownload request;
    request.set_version(2);
    request.set_contract_id("SHFE/rb/2026-10");
    request.set_source("tushare.ft_mins");
    request.set_source_instrument("RB2610.SHF");
    request.set_interval_minutes(1);
    request.set_begin_ns(begin);
    request.set_end_ns(begin + (count - 1) * minute_ns);
    request.set_requests_per_minute(100);
    const auto source = test::publish_download(store, "minutes", request, minutes);
    selection->add_source_dataset_ids(source.minute_result().manifest_sha256());
    auto daily = allocation().daily();
    daily.set_begin_day("2026-01-01");
    daily.set_end_day("2026-05-20");
    Daily provider(daily.source());
    const auto settlement = test::publish_download(store, "settlements", daily, provider);
    selection->add_settlement_dataset_ids(settlement.daily_result().manifest_sha256());
  }
  const auto endpoint = (sockets.path / "data.sock").string();
  const auto worker = (sockets.path / "data.workers").string();
  ChildProcess process(ASTERION_DATA_SERVICE_PATH,
                       {"--directory", warehouse.path.string(), "--session", "historical-data",
                        "--task-instance", "task", "--endpoint", endpoint, "--worker-endpoint",
                        worker, "--file-workers", "1"});
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
