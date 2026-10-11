#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/protocol/data.hpp>
#include <asterion/protocol/data_client.hpp>
#include <asterion/protocol/task.hpp>
#include <asterion/v1/task.pb.h>
#include <fstream>
#include <gtest/gtest.h>
#include <thread>
using namespace asterion;
using namespace std::chrono_literals;
namespace {
struct Folder {
  std::filesystem::path path;
  explicit Folder(const char* prefix)
      : path(std::filesystem::path("/tmp") / (prefix + unique_process_id().substr(0, 12))) {
    std::filesystem::create_directory(path);
    std::filesystem::permissions(path, std::filesystem::perms::owner_all);
  }
  ~Folder() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};
std::string contents(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), {}};
}
// The data and task services of one node, with the fixture plugin as the only
// data source: an exchange with one contract and a week holding every outcome.
struct Node {
  Folder warehouse{"ast-tw-"}, ledger{"ast-tl-"}, sockets{"ast-ts-"};
  const std::string endpoint = (sockets.path / "task.sock").string(),
                    workers = (sockets.path / "worker.sock").string(),
                    data_endpoint = (sockets.path / "data.sock").string(),
                    data_workers = (sockets.path / "data.workers").string();
  ChildProcess data_service{ASTERION_DATA_SERVICE_PATH,
                            {"--directory", warehouse.path.string(), "--session", "fixture-data",
                             "--task-instance", "table-test", "--endpoint", data_endpoint,
                             "--worker-endpoint", data_workers, "--plugin-directory",
                             PLUGIN_FIXTURE_DIRECTORY}};
  protocol::DataClient data{data_endpoint, "fixture-data"};
  ChildProcess task_service{ASTERION_TASK_SERVICE_PATH,
                            {"--directory", ledger.path.string(), "--endpoint", endpoint,
                             "--worker-endpoint", workers, "--session", "table-test",
                             "--data-instance", "fixture-data", "--data-endpoint", data_workers}};
  task::v1::TaskResponse call(task::v1::TaskRequest request, bool worker = false) {
    request.set_version(1);
    request.set_service_id("table-test");
    request.set_correlation_id(unique_process_id());
    auto channel = ipc::Channel::connect(worker ? workers : endpoint, 2s);
    channel.send(request.SerializeAsString(), 2s);
    task::v1::TaskResponse reply;
    if (!reply.ParseFromString(channel.receive(5s)))
      throw std::runtime_error("bad response");
    return reply;
  }
  Node() {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    for (;;) {
      try {
        task::v1::TaskRequest ping;
        ping.mutable_heartbeat();
        data::v1::DataRequest data_ping;
        data_ping.mutable_heartbeat();
        if (call(ping, true).health().initialized() && call(ping).health().initialized() &&
            data.call(data_ping).health().initialized())
          break;
      } catch (const std::exception&) {
        if (std::chrono::steady_clock::now() > deadline)
          throw;
      }
      std::this_thread::sleep_for(20ms);
    }
    // The account's share of requests, as a user applies it before downloading.
    for (const auto* credential : {"fixture", "refused"}) {
      data::v1::DataRequest budget;
      budget.mutable_configure_download_budget()->set_source("fixture.daily");
      budget.mutable_configure_download_budget()->set_credential(credential);
      budget.mutable_configure_download_budget()->set_requests_per_minute(60);
      (void)data.call(budget);
    }
  }
  // Submits an update of the daily table and runs its worker to the end.
  task::v1::Task update(const std::string& id, const std::string& begin,
                        const std::string& credential = "fixture") {
    data::v1::DataRequest authorize;
    auto* request = authorize.mutable_authorize_download();
    request->set_task_instance("table-test");
    request->set_task_id(id);
    request->set_credential(credential);
    *request->mutable_table_update() =
        protocol::encode_table_update({{"provider", "test.independent.c"},
                                       {"tables", {"futures.daily"}},
                                       {"begin_day", begin},
                                       {"end_day", "2024-03-07"},
                                       {"requests_per_minute", 500}});
    task::v1::TaskRequest submit;
    submit.mutable_submit()->set_id(id);
    submit.mutable_submit()->set_download_authorization(
        data.call(authorize).download_authorization().id());
    const auto submitted = call(submit);
    EXPECT_TRUE(submitted.has_task()) << submitted.DebugString();
    EXPECT_EQ(submitted.task().kind(), task::v1::TABLE_UPDATE);
    EXPECT_EQ(submitted.task().total(), 100U);
    EXPECT_EQ(submitted.task().instrument(), "futures.daily");
    EXPECT_EQ(submitted.SerializeAsString().find(credential), std::string::npos);
    task::v1::TaskRequest dispatch;
    dispatch.mutable_dispatch()->set_launch_slots(2);
    auto launches = call(dispatch, true);
    for (const auto deadline = std::chrono::steady_clock::now() + 10s;
         launches.launches().launches().empty() && std::chrono::steady_clock::now() < deadline;
         launches = call(dispatch, true))
      std::this_thread::sleep_for(20ms);
    EXPECT_EQ(launches.launches().launches_size(), 1) << launches.DebugString();
    EXPECT_TRUE(launches.launches().launches(0).table_update());
    EXPECT_EQ(launches.launches().launches(0).provider_artifact().size(), 64U);
    const auto log = ledger.path / (id + ".log");
    ChildProcess worker(ASTERION_DATA_PIPELINE_PATH,
                        {"--endpoint", workers, "--session", "table-test", "--task", id,
                         "--table-update", "--plugin-directory", PLUGIN_FIXTURE_DIRECTORY},
                        false, log, true);
    EXPECT_TRUE(worker.wait(20s)) << contents(log);
    task::v1::TaskRequest get;
    get.mutable_get()->set_id(id);
    return call(get).task();
  }
  data::v1::TableUpdateResult result(const std::string& id) {
    task::v1::TaskRequest request;
    request.mutable_result()->set_id(id);
    const auto reply = call(request);
    EXPECT_TRUE(reply.has_table_update()) << reply.DebugString();
    EXPECT_EQ(protocol::decode_task_result(reply, id).at("kind"), "table_update");
    return reply.table_update();
  }
  data::v1::TableCoverage coverage() {
    data::v1::DataRequest request;
    request.mutable_table_coverage();
    return data.call(request).table_coverage();
  }
};
} // namespace
TEST(TableUpdates, AnUpdateStoresWhatItCanAndSaysWhatItCouldNot) {
  Node node;
  const auto first = node.update("tables", "2024-02-01");
  ASSERT_EQ(first.state(), task::v1::SUCCEEDED) << first.error();
  EXPECT_EQ(first.completed(), 100U);
  const auto done = node.result("tables");
  // The contract list, the year of the calendar, and the three ordinary days
  // from the first listing on: nothing is read before the exchange had a contract.
  EXPECT_EQ(done.stored(), 5U);
  EXPECT_EQ(done.unchanged(), 0U);
  ASSERT_EQ(done.rejected_size(), 2);
  EXPECT_EQ(done.rejected(0).partition().period(), "2024-03-05");
  EXPECT_EQ(done.rejected(0).reason(),
            "futures.daily SHFE 2024-03-05: row 1 \"SHFE/cu/2024-03\" column volume: "
            "\"1200.5\" has more than 0 decimal places");
  EXPECT_EQ(done.rejected(1).partition().period(), "2024-03-06");
  EXPECT_EQ(done.rejected(1).reason(),
            "futures.daily SHFE 2024-03-06: vendor/cu2499 is not in the contract list");
  EXPECT_EQ(done.unpublished(), 1U);
  ASSERT_EQ(done.unpublished_examples_size(), 1);
  EXPECT_EQ(done.unpublished_examples(0).period(), "2024-03-07");
  EXPECT_EQ(done.snapshot().size(), 64U);

  auto held = node.coverage();
  EXPECT_EQ(held.snapshot(), done.snapshot());
  ASSERT_EQ(held.sources_size(), 3);
  const auto daily =
      std::ranges::find(held.rows(), "futures.daily", &data::v1::TableCoverageRow::table);
  ASSERT_NE(daily, held.rows().end());
  EXPECT_EQ(daily->exchange(), "SHFE");
  EXPECT_EQ(daily->source(), "fixture.daily.v1");
  EXPECT_EQ(daily->partitions(), 3U);
  EXPECT_EQ(daily->first_period(), "2024-02-29");
  EXPECT_EQ(daily->last_period(), "2024-03-04");
  ASSERT_TRUE(daily->has_missing_days());
  EXPECT_EQ(daily->missing_days(), 0U) << "the weekend between is not a gap";

  data::v1::DataRequest page;
  *page.mutable_table_page() = protocol::encode_table_page_query({{"table", "futures.daily"},
                                                                  {"source", "fixture.daily.v1"},
                                                                  {"exchange", "SHFE"},
                                                                  {"period", "2024-03-01"},
                                                                  {"offset", 0},
                                                                  {"limit", 100}});
  const auto rows = protocol::decode_table_page(node.data.call(page).table_page());
  ASSERT_EQ(rows.at("rows").size(), 1U);
  EXPECT_EQ(rows.at("columns").at(9), "amount");
  EXPECT_EQ(rows.at("rows").at(0).at(0), "SHFE/cu/2024-03");
  EXPECT_EQ(rows.at("rows").at(0).at(9), "432000000000.25") << "beyond an eight-place int64";
  EXPECT_TRUE(rows.at("rows").at(0).at(1).is_null()) << "no open price was published";

  // Continuing reads only what follows the last stored day, and what was
  // already held is not stored twice.
  const auto second = node.update("again", "");
  ASSERT_EQ(second.state(), task::v1::SUCCEEDED) << second.error();
  const auto repeated = node.result("again");
  EXPECT_EQ(repeated.stored(), 0U);
  EXPECT_EQ(repeated.unchanged(), 1U) << "the contract list, read again and found the same";
  EXPECT_EQ(repeated.rejected_size(), 2);
  EXPECT_EQ(repeated.unpublished(), 1U);
  EXPECT_EQ(node.coverage().snapshot(), done.snapshot()) << "nothing was appended";
}
TEST(TableUpdates, ARefusedRequestFailsTheTaskInTheSourcesOwnWords) {
  Node node;
  const auto task = node.update("denied", "2024-02-01", "refused");
  EXPECT_EQ(task.state(), task::v1::FAILED);
  EXPECT_EQ(task.error(), "fixture refused the request: the account has no permission");
  EXPECT_TRUE(node.coverage().rows().empty());
}
