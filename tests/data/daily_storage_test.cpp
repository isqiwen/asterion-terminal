#include "sqlite_database.hpp"
#include "data/history_requests.hpp"
#include "data_store.hpp"
#include "data/data_fixture.hpp"
#include <asterion/protocol/data_client.hpp>
#include "daily_factor_source.hpp"
#include "factor_engine.hpp"
#include "history_daily.hpp"
#include "tushare.hpp"
#include "task_store.hpp"
#include "tasks/task_store_support.hpp"
#include <asterion/protocol/data.hpp>
#include <limits>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/file_lock.hpp>
#include <fstream>
#include <thread>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <gtest/gtest.h>
#include <asterion/kernel/process/artifact.hpp>
using namespace asterion;
namespace {
struct Folder {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() / ("ast-daily-" + unique_process_id());
  Folder() { std::filesystem::create_directory(path); }
  ~Folder() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};
HistoricalDailyRange range() {
  using namespace std::chrono;
  return {{"SHFE", "cu", "2024-03"},
          year(2023) / January / 1,
          year(2024) / January / 3,
          "tushare.fut_daily",
          "CU2403.SHF"};
}
std::string response(const std::string& body) {
  auto date = Json::parse(body).at("params").at("start_date").get<std::string>();
  return "{\"code\":0,\"data\":{\"fields\":[\"ts_code\",\"trade_date\",\"pre_close\",\"pre_"
         "settle\",\"open\",\"high\",\"low\",\"close\",\"settle\",\"vol\",\"amount\",\"oi\"],"
         "\"items\":[[\"CU2403.SHF\",\"" +
         date + "\",null,99.5,100.00000001,102,99,101,100.5,20,123.456789012345,2000]]}}";
}
std::string contents(const std::filesystem::path& path) {
  std::ifstream input(path);
  return std::string(std::istreambuf_iterator<char>(input), {});
}
} // namespace
TEST(DailyStorage, PersistsTypedValuesAndResumesAfterCancellationWithoutRedownload) {
  Folder folder;
  std::vector<std::string> starts;
  tushare::Daily provider("fixture-secret", [&](const auto& body, auto) {
    starts.push_back(Json::parse(body).at("params").at("start_date"));
    return response(body);
  });
  provider.start();
  std::stop_source stop;
  EXPECT_THROW(
      history_files::download_daily(provider, range(), folder.path, stop.get_token(),
                                    [&](unsigned completed, unsigned total, std::uint64_t rows) {
                                      EXPECT_EQ(total, 2);
                                      EXPECT_EQ(rows, completed);
                                      if (completed == 1)
                                        stop.request_stop();
                                    }),
      std::runtime_error);
  const auto partial = history_files::inspect_daily(folder.path);
  EXPECT_FALSE(partial.complete);
  EXPECT_EQ(partial.acquired_at_ns, 0);
  EXPECT_EQ(partial.pages, 1);
  EXPECT_THROW(history_files::read_daily(folder.path), std::invalid_argument);
  const auto first = contents(folder.path / "daily-0.parquet");
  const auto result = history_files::download_daily(provider, range(), folder.path);
  EXPECT_TRUE(result.complete);
  EXPECT_GT(result.acquired_at_ns, 0);
  EXPECT_EQ(result.rows, 2);
  EXPECT_EQ(starts, (std::vector<std::string>{"20230101", "20240102"}));
  EXPECT_EQ(first, contents(folder.path / "daily-0.parquet"));
  const auto data = history_files::read_daily(folder.path);
  ASSERT_EQ(data.bars.size(), 2);
  EXPECT_EQ(data.info.manifest_sha256, result.manifest_sha256);
  EXPECT_EQ(format_trading_date(data.bars[1].trading_day), "2024-01-02");
  EXPECT_EQ(data.bars[0].open.str(), "100.00000001");
  EXPECT_EQ(data.bars[0].amount.str(), "1234567.89012345");
  EXPECT_FALSE(data.bars[0].previous_close);
  EXPECT_EQ(data.bars[0].previous_settlement, Decimal::parse("99.5"));
  const auto resumed = history_files::download_daily(provider, range(), folder.path);
  EXPECT_EQ(resumed.rows, 2);
  EXPECT_EQ(resumed.acquired_at_ns, result.acquired_at_ns);
  EXPECT_EQ(resumed.manifest_sha256, result.manifest_sha256);
  EXPECT_EQ(starts.size(), 2);
  auto obsolete = Json::parse(contents(folder.path / "daily.json"));
  obsolete["version"] = 3;
  obsolete.erase("acquired_at_ns");
  obsolete.erase("source_availability");
  replace_file_durably(folder.path / "daily.json", obsolete.dump());
  EXPECT_THROW(history_files::download_daily(provider, range(), folder.path),
               std::invalid_argument);
  EXPECT_EQ(Json::parse(contents(folder.path / "daily.json")), obsolete);
  for (const auto& file : std::filesystem::directory_iterator(folder.path))
    EXPECT_EQ(contents(file.path()).find("fixture-secret"), std::string::npos);
}
TEST(DailyStorage, RecoversDurableUnindexedPageAndRejectsChangedOrCorruptedInputs) {
  Folder folder;
  int requests = 0;
  tushare::Daily provider("fixture", [&](const auto& body, auto) {
    ++requests;
    return response(body);
  });
  provider.start();
  history_files::download_daily(provider, range(), folder.path);
  auto manifest = Json::parse(contents(folder.path / "daily.json"));
  manifest["pages"].erase(1);
  manifest["rows"] = 1;
  manifest["complete"] = false;
  manifest["acquired_at_ns"] = "0";
  replace_file_durably(folder.path / "daily.json", manifest.dump());
  EXPECT_TRUE(history_files::download_daily(provider, range(), folder.path).complete);
  EXPECT_EQ(requests, 2); // No HTTP call for the validated orphan segment.
  auto changed = range();
  changed.end = std::chrono::year(2024) / std::chrono::January / 4;
  EXPECT_THROW(history_files::download_daily(provider, changed, folder.path),
               std::invalid_argument);
  replace_file_durably(folder.path / "daily-1.parquet", "{}");
  EXPECT_THROW(history_files::read_daily(folder.path), std::invalid_argument);
  EXPECT_THROW(history_files::download_daily(provider, range(), folder.path),
               std::invalid_argument);
  EXPECT_EQ(requests, 2);
}
TEST(DailyStorage, SharedReadersExcludeWritersAndRejectForeignDirectories) {
  Folder folder;
  tushare::Daily provider("fixture", [](const auto& body, auto) { return response(body); });
  provider.start();
  history_files::download_daily(provider, range(), folder.path);
  {
    FileLock reader(folder.path, "daily.lock", FileLock::Access::shared);
    EXPECT_EQ(history_files::read_daily(folder.path).bars.size(), 2);
    EXPECT_THROW(history_files::download_daily(provider, range(), folder.path), std::exception);
  }
  {
    FileLock writer(folder.path, "daily.lock");
    EXPECT_THROW(history_files::read_daily(folder.path), std::exception);
  }
  Folder foreign;
  replace_file_durably(foreign.path / "existing-data", "preserve");
  EXPECT_THROW(history_files::download_daily(provider, range(), foreign.path),
               std::invalid_argument);
  EXPECT_EQ(contents(foreign.path / "existing-data"), "preserve");
}

TEST(DailyTasks, RealServiceDispatchAndManagedWorkerResumeCompletedSourceData) {
  using namespace std::chrono_literals;
  Folder folder, warehouse;
  const auto input = asterion::testing_support::daily_request({{"version", 2},
                                                               {"contract_id", "SHFE/cu/2024-03"},
                                                               {"source", "tushare.fut_daily"},
                                                               {"source_instrument", "CU2403.SHF"},
                                                               {"begin_day", "2024-01-02"},
                                                               {"end_day", "2024-01-03"},
                                                               {"requests_per_minute", 500}});
  {
    tasks::Store store(folder.path, tasks::Identity{"daily-test", "fixture-data"});
    data::Store data(warehouse.path, "fixture-data", "daily-test");
    tasks::submit(store, test::authorize_download(data, "daily-test", "daily", input));
    data::v1::DownloadAllocation allocation;
    auto* identity = allocation.mutable_identity();
    identity->set_data_instance("fixture-data");
    identity->set_task_instance("daily-test");
    identity->set_task_id("daily");
    identity->set_attempt(1);
    *allocation.mutable_daily() = input;
    const auto dir = test::allocate_download(data, allocation).directory();
    tushare::Daily provider("fixture", [](const auto& body, auto) { return response(body); });
    provider.start();
    history_files::download_daily(provider, history_files::daily_range(input), dir);
  }
  const auto socket_dir =
      std::filesystem::path("/tmp") / ("ast-d-" + unique_process_id().substr(0, 12));
  std::filesystem::create_directory(socket_dir);
  std::filesystem::permissions(socket_dir, std::filesystem::perms::owner_all);
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code e;
      std::filesystem::remove_all(path, e);
    }
  } cleanup{socket_dir};
  const auto endpoint = (socket_dir / "task.sock").string();
  const auto worker_endpoint = (socket_dir / "worker.sock").string();
  const auto data_endpoint = (socket_dir / "data.sock").string();
  const auto data_workers = (socket_dir / "data.workers").string();
  ChildProcess data_service(ASTERION_DATA_SERVICE_PATH,
                            {"--directory", warehouse.path.string(), "--session", "fixture-data",
                             "--task-instance", "daily-test", "--endpoint", data_endpoint,
                             "--worker-endpoint", data_workers});
  protocol::DataClient data(data_endpoint, "fixture-data");
  ChildProcess service(ASTERION_TASK_SERVICE_PATH,
                       {"--directory", folder.path.string(), "--endpoint", endpoint,
                        "--worker-endpoint", worker_endpoint, "--session", "daily-test",
                        "--data-instance", "fixture-data", "--data-endpoint", data_workers});
  auto call = [&](task::v1::TaskRequest request, bool worker = false) {
    request.set_version(1);
    request.set_service_id("daily-test");
    request.set_correlation_id(unique_process_id());
    auto channel = ipc::Channel::connect(worker ? worker_endpoint : endpoint, 2s);
    channel.send(request.SerializeAsString(), 2s);
    task::v1::TaskResponse reply;
    if (!reply.ParseFromString(channel.receive(2s)))
      throw std::runtime_error("bad response");
    protocol::validate_message(reply);
    EXPECT_EQ(reply.correlation_id(), request.correlation_id());
    return reply;
  };
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  for (;;) {
    try {
      task::v1::TaskRequest ping;
      ping.mutable_heartbeat();
      const bool task_ready =
          call(ping, true).health().initialized() && call(ping).health().initialized();
      data::v1::DataRequest data_ping;
      data_ping.mutable_heartbeat();
      if (task_ready && data.call(data_ping).health().initialized())
        break;
    } catch (const std::exception&) {
      if (service.exited() || std::chrono::steady_clock::now() > deadline)
        throw;
    }
    ASSERT_LT(std::chrono::steady_clock::now(), deadline);
    std::this_thread::sleep_for(20ms);
  }
  task::v1::TaskRequest request;
  request.mutable_dispatch()->set_launch_slots(2);
  EXPECT_TRUE(call(request).has_error());
  auto launches = call(request, true);
  // Agent periodically asks for launches; Task's independent observation of
  // Data readiness may follow the heartbeat observed by this test.
  while (launches.has_launches() && launches.launches().launches().empty() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(20ms);
    launches = call(request, true);
  }
  ASSERT_TRUE(launches.has_launches()) << launches.DebugString();
  ASSERT_EQ(launches.launches().launches_size(), 1);
  EXPECT_TRUE(launches.launches().launches(0).daily_download());
  const auto worker_log = folder.path / "worker.log";
  ChildProcess worker(ASTERION_DATA_PIPELINE_PATH,
                      {"--endpoint", worker_endpoint, "--session", "daily-test", "--task", "daily",
                       "--daily-download"},
                      false, worker_log, true);
  ASSERT_TRUE(worker.wait(10s)) << contents(worker_log);
  EXPECT_EQ(worker.exit_code(), 0) << contents(worker_log);
  request.mutable_get()->set_id("daily");
  const auto published_deadline = std::chrono::steady_clock::now() + 10s;
  while (call(request).task().state() == task::v1::PUBLISHING &&
         std::chrono::steady_clock::now() < published_deadline)
    std::this_thread::sleep_for(20ms);
  request.mutable_result()->set_id("daily");
  auto result = call(request);
  ASSERT_TRUE(result.has_daily()) << result.DebugString();
  EXPECT_EQ(result.daily().rows(), 1);
  EXPECT_EQ(protocol::decode_task_result(result, "daily").at("kind"), "daily_download");
  EXPECT_EQ(result.result_task().state(), task::v1::SUCCEEDED);
  data::v1::DataRequest page_query;
  page_query.mutable_daily_page()->set_dataset_id(result.daily().manifest_sha256());
  page_query.mutable_daily_page()->set_limit(5);
  const auto page = data.call(page_query);
  ASSERT_TRUE(page.has_daily_page()) << page.DebugString();
  EXPECT_EQ(protocol::decode_daily_page(page.daily_page()).at("bars").size(), 1);
  EXPECT_EQ(page.daily_page().bars(0).trading_day(), "2024-01-02");

  request.mutable_submit()->set_id("queued");
  data::v1::DataRequest authorize;
  authorize.mutable_authorize_download()->set_task_instance("daily-test");
  authorize.mutable_authorize_download()->set_task_id("queued");
  authorize.mutable_authorize_download()->set_credential("fixture");
  *authorize.mutable_authorize_download()->mutable_daily() = input;
  request.mutable_submit()->set_download_authorization(
      data.call(authorize).download_authorization().id());
  auto submitted = call(request);
  ASSERT_TRUE(submitted.has_task()) << submitted.DebugString();
  EXPECT_EQ(submitted.task().kind(), task::v1::DAILY_DOWNLOAD);
  EXPECT_EQ(submitted.SerializeAsString().find("fixture-secret"), std::string::npos);
  request.mutable_cancel()->set_id("queued");
  EXPECT_EQ(call(request).task().state(), task::v1::CANCELLED);
  request.mutable_list()->set_limit(200);
  const auto summary = call(request);
  ASSERT_TRUE(summary.has_tasks());
  ASSERT_EQ(summary.tasks().tasks_size(), 2);
  EXPECT_FALSE(summary.tasks().tasks(0).has_daily());
  EXPECT_EQ(summary.SerializeAsString().find("fixture-secret"), std::string::npos);
}

TEST(DailyPages, DatePagingKeepsExactPricesMissingSettlementAndDatasetOriginMacd) {
  using namespace std::chrono;
  Folder folder;
  const auto input = asterion::testing_support::daily_request({{"version", 2},
                                                               {"contract_id", "SHFE/cu/2024-03"},
                                                               {"source", "tushare.fut_daily"},
                                                               {"source_instrument", "CU2403.SHF"},
                                                               {"begin_day", "2023-01-01"},
                                                               {"end_day", "2023-06-01"},
                                                               {"requests_per_minute", 500}});
  tushare::Daily provider("fixture", [](const auto& body, auto) {
    auto data = Json::parse(response(body));
    auto& rows = data["data"]["items"];
    rows = Json::array();
    for (int i = 0; i < 120; ++i) {
      auto day = format_trading_date(year_month_day(sys_days(year(2023) / January / 1) + days(i)));
      std::erase(day, '-');
      const int close = 100 + i + (i > 100 ? 20 : 0);
      rows.push_back({"CU2403.SHF", day, nullptr, 99.5, close, close + 2, close - 2, close, nullptr,
                      20, 123.125, 2000});
    }
    return data.dump();
  });
  provider.start();
  history_files::download_daily(provider, history_files::daily_range(input), folder.path);
  const auto result = history_files::daily_result(folder.path);
  data::v1::DailyPageQuery query;
  query.set_task_id("daily");
  query.set_limit(120);
  query.set_include_macd(true);
  const auto all = history_files::read_daily_page(input, result, query);
  ASSERT_EQ(all.bars_size(), 120);
  EXPECT_EQ(all.first_day(), "2023-01-01");
  EXPECT_EQ(all.last_day(), "2023-04-30");
  EXPECT_FALSE(all.bars(32).has_macd());
  EXPECT_TRUE(all.bars(33).has_macd());
  EXPECT_NE(all.bars(110).macd().histogram(), 0);
  const auto decoded = protocol::decode_daily_page(all);
  EXPECT_TRUE(decoded.at("bars")[0].at("settlement").is_null());
  EXPECT_TRUE(decoded.at("bars")[0].at("previous_close").is_null());
  EXPECT_EQ(decoded.at("bars")[0].at("previous_settlement"), "99.5");
  EXPECT_EQ(decoded.at("bars")[0].at("amount"), "1231250");
  EXPECT_FALSE(decoded.at("bars")[0].contains("timestamp_ns"));
  query.set_offset(110);
  query.set_limit(5);
  const auto paged = history_files::read_daily_page(input, result, query);
  query.set_offset(0);
  query.set_begin_day(all.bars(110).trading_day());
  const auto filtered = history_files::read_daily_page(input, result, query);
  EXPECT_EQ(filtered.matched_rows(), 10);
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(paged.bars(i).SerializeAsString(), all.bars(i + 110).SerializeAsString());
    EXPECT_EQ(filtered.bars(i).SerializeAsString(), paged.bars(i).SerializeAsString());
  }
  query.set_begin_day("2023-05-01");
  const auto empty = history_files::read_daily_page(input, result, query);
  EXPECT_EQ(empty.matched_rows(), 0);
  EXPECT_EQ(empty.total_rows(), 120);
  EXPECT_TRUE(protocol::decode_daily_page(empty).at("bars").empty());
  query.set_offset(1);
  EXPECT_THROW(history_files::read_daily_page(input, result, query), std::invalid_argument);
  query.set_offset(0);
  query.set_begin_day("");
  query.set_limit(201);
  EXPECT_THROW(history_files::read_daily_page(input, result, query), std::invalid_argument);
  query.set_limit(5);
  query.set_begin_day("2023-02-29");
  EXPECT_THROW(history_files::read_daily_page(input, result, query), std::invalid_argument);
  query.set_begin_day("2023-06-02");
  EXPECT_THROW(history_files::read_daily_page(input, result, query), std::invalid_argument);
  query.set_begin_day("");
  query.set_offset(120);
  EXPECT_THROW(history_files::read_daily_page(input, result, query), std::invalid_argument);
  auto malformed = all;
  malformed.mutable_bars(0)->clear_amount();
  EXPECT_THROW(protocol::decode_daily_page(malformed), std::invalid_argument);
  malformed = all;
  malformed.mutable_bars(1)->set_trading_day(all.bars(0).trading_day());
  EXPECT_THROW(protocol::decode_daily_page(malformed), std::invalid_argument);
  malformed = all;
  malformed.mutable_bars(119)->mutable_macd()->set_diff(std::numeric_limits<double>::infinity());
  EXPECT_THROW(protocol::decode_daily_page(malformed), std::invalid_argument);
  malformed = all;
  malformed.set_manifest_sha256(std::string(64, 'z'));
  EXPECT_THROW(protocol::decode_daily_page(malformed), std::invalid_argument);
  malformed = all;
  malformed.set_source("invalid/source");
  EXPECT_THROW(protocol::decode_daily_page(malformed), std::invalid_argument);
  query.set_offset(0);
  {
    FileLock reader(folder.path, "daily.lock", FileLock::Access::shared);
    EXPECT_EQ(history_files::read_daily_page(input, result, query).bars_size(), 5);
  }
  {
    FileLock writer(folder.path, "daily.lock");
    EXPECT_THROW(history_files::read_daily_page(input, result, query), std::runtime_error);
  }
  replace_file_durably(folder.path / "daily-0.parquet", "{}");
  EXPECT_THROW(history_files::read_daily_page(input, result, query), std::invalid_argument);
}
TEST(DailyPages, EmptyDatasetRetainsRequestedDatesWithoutInventedCoverage) {
  Folder folder;
  const auto input = asterion::testing_support::daily_request({{"version", 2},
                                                               {"contract_id", "SHFE/cu/2024-03"},
                                                               {"source", "tushare.fut_daily"},
                                                               {"source_instrument", "CU2403.SHF"},
                                                               {"begin_day", "2023-01-01"},
                                                               {"end_day", "2023-01-03"},
                                                               {"requests_per_minute", 500}});
  tushare::Daily provider("fixture", [](const auto& body, auto) {
    auto data = Json::parse(response(body));
    data["data"]["items"] = Json::array();
    return data.dump();
  });
  provider.start();
  history_files::download_daily(provider, history_files::daily_range(input), folder.path);
  auto result = history_files::daily_result(folder.path);
  data::v1::DailyPageQuery query;
  query.set_task_id("empty");
  query.set_limit(10);
  const auto page = history_files::read_daily_page(input, result, query);
  EXPECT_TRUE(page.first_day().empty());
  EXPECT_TRUE(page.last_day().empty());
  EXPECT_EQ(page.begin_day(), "2023-01-01");
  EXPECT_EQ(page.end_day(), "2023-01-03");
  EXPECT_TRUE(protocol::decode_daily_page(page).at("bars").empty());
  result.set_rows(1);
  EXPECT_THROW(history_files::read_daily_page(input, result, query), std::invalid_argument);
}

TEST(DailyPages, WeeklyAndMonthlyAggregateBeforeFilteringPagingAndMacd) {
  using namespace std::chrono;
  Folder folder;
  const auto input = asterion::testing_support::daily_request({{"version", 2},
                                                               {"contract_id", "SHFE/cu/2024-03"},
                                                               {"source", "tushare.fut_daily"},
                                                               {"source_instrument", "CU2403.SHF"},
                                                               {"begin_day", "2023-01-01"},
                                                               {"end_day", "2023-12-31"},
                                                               {"requests_per_minute", 500}});
  tushare::Daily provider("fixture", [](const auto& body, auto) {
    auto data = Json::parse(response(body));
    auto& rows = data["data"]["items"];
    rows = Json::array();
    for (int i = 0; i < 365; ++i) {
      auto day = format_trading_date(year_month_day(sys_days(year(2023) / January / 1) + days(i)));
      std::erase(day, '-');
      rows.push_back({"CU2403.SHF", day, nullptr, 99, 100 + i, 102 + i, 98 + i, 101 + i, nullptr, 2,
                      1, 2000 + i});
    }
    return data.dump();
  });
  provider.start();
  history_files::download_daily(provider, history_files::daily_range(input), folder.path);
  const auto result = history_files::daily_result(folder.path);
  data::v1::DailyPageQuery query;
  query.set_task_id("weekly");
  query.set_limit(200);
  query.set_include_macd(true);
  query.set_period(data::v1::WEEK);
  const auto all = history_files::read_daily_page(input, result, query);
  EXPECT_EQ(all.total_rows(), 53);
  EXPECT_EQ(all.bars(0).trading_day(), "2023-01-01");
  EXPECT_EQ(all.bars(1).trading_day(), "2023-01-08");
  EXPECT_EQ(Decimal::from_raw(all.bars(1).volume().units()).str(), "14");
  EXPECT_EQ(Decimal::from_raw(all.bars(1).open_interest().units()).str(), "2007");
  EXPECT_FALSE(all.bars(32).has_macd());
  EXPECT_TRUE(all.bars(33).has_macd());
  EXPECT_EQ(protocol::decode_daily_page(all).at("period"), "week");
  query.set_offset(40);
  query.set_limit(3);
  const auto paged = history_files::read_daily_page(input, result, query);
  query.set_offset(0);
  query.set_begin_day(all.bars(40).trading_day());
  const auto filtered = history_files::read_daily_page(input, result, query);
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(paged.bars(i).SerializeAsString(), all.bars(40 + i).SerializeAsString());
    EXPECT_EQ(filtered.bars(i).SerializeAsString(), all.bars(40 + i).SerializeAsString());
  }
  query.set_begin_day("");
  query.set_limit(200);
  query.set_period(data::v1::MONTH);
  const auto months = history_files::read_daily_page(input, result, query);
  EXPECT_EQ(months.total_rows(), 12);
  EXPECT_EQ(months.bars(0).trading_day(), "2023-01-31");
  EXPECT_EQ(Decimal::from_raw(months.bars(0).volume().units()).str(), "62");
  EXPECT_EQ(Decimal::from_raw(months.bars(0).amount().units()).str(), "310000");
  EXPECT_EQ(protocol::decode_daily_page(months).at("period"), "month");
  query.set_period(data::v1::QUARTER);
  const auto quarters = history_files::read_daily_page(input, result, query);
  ASSERT_EQ(quarters.total_rows(), 4);
  EXPECT_EQ(quarters.bars(0).trading_day(), "2023-03-31");
  EXPECT_EQ(Decimal::from_raw(quarters.bars(0).volume().units()).str(), "180");
  EXPECT_EQ(protocol::decode_daily_page(quarters).at("period"), "quarter");
  query.set_offset(1);
  query.set_limit(1);
  EXPECT_EQ(history_files::read_daily_page(input, result, query).bars(0).SerializeAsString(),
            quarters.bars(1).SerializeAsString());
  query.set_offset(0);
  query.set_limit(200);
  query.set_period(data::v1::YEAR);
  const auto years = history_files::read_daily_page(input, result, query);
  ASSERT_EQ(years.total_rows(), 1);
  EXPECT_EQ(years.bars(0).trading_day(), "2023-12-31");
  EXPECT_EQ(Decimal::from_raw(years.bars(0).volume().units()).str(), "730");
  EXPECT_EQ(Decimal::from_raw(years.bars(0).amount().units()).str(), "3650000");
  EXPECT_FALSE(years.bars(0).has_macd());
  EXPECT_EQ(protocol::decode_daily_page(years).at("period"), "year");
  query.set_period(static_cast<data::v1::DailyPeriod>(99));
  EXPECT_THROW(history_files::read_daily_page(input, result, query), std::invalid_argument);
  auto bad = months;
  bad.set_period(static_cast<data::v1::DailyPeriod>(99));
  EXPECT_THROW(protocol::decode_daily_page(bad), std::invalid_argument);
}

TEST(DailyFactorSource, SnapshotsVerifiedCompletedSourceAndRejectsChangedEvidence) {
  using namespace std::chrono;
  Folder folder, warehouse;
  auto data_store =
      std::make_unique<data::Store>(warehouse.path, "fixture-data", "daily-factor-test");
  auto store = std::make_unique<tasks::Store>(folder.path,
                                              tasks::Identity{"daily-factor-test", "fixture-data"});
  const auto request =
      asterion::testing_support::daily_request({{"version", 2},
                                                {"contract_id", "SHFE/cu/2024-03"},
                                                {"source", "tushare.fut_daily"},
                                                {"source_instrument", "CU2403.SHF"},
                                                {"begin_day", "2023-01-01"},
                                                {"end_day", "2023-04-01"},
                                                {"requests_per_minute", 500}});
  tasks::submit(*store,
                test::authorize_download(*data_store, "daily-factor-test", "source", request));
  task::v1::TaskAttempt attempt;
  attempt.set_token(store->commit(store->claim("source")).token());
  *attempt.mutable_task() = store->get("source");
  data::v1::DownloadAllocation allocation;
  auto* identity = allocation.mutable_identity();
  identity->set_data_instance("fixture-data");
  identity->set_task_instance("daily-factor-test");
  identity->set_task_id("source");
  identity->set_attempt(attempt.task().attempt());
  *allocation.mutable_daily() = request;
  attempt.set_output_directory(test::allocate_download(*data_store, allocation).directory());
  tushare::Daily provider("fixture", [](const auto& body, auto) {
    auto data = Json::parse(response(body));
    auto& rows = data["data"]["items"];
    rows = Json::array();
    for (int i = 0; i < 80; ++i) {
      auto day = format_trading_date(year_month_day(sys_days(year(2023) / January / 1) + days(i)));
      std::erase(day, '-');
      const int price = 100 + i + i % 3;
      rows.push_back({"CU2403.SHF", day, nullptr, "99.5", price, price, price, price, nullptr, 20,
                      "0.000100000001", 2000});
    }
    return data.dump();
  });
  provider.start();
  history_files::download_daily(provider, history_files::daily_range(request),
                                attempt.output_directory());
  const auto result = history_files::daily_result(attempt.output_directory());
  factor::v1::DailyFactorRequest unpublished;
  unpublished.set_source_dataset_id(result.manifest_sha256());
  EXPECT_THROW(data_store->archive().get(unpublished.source_dataset_id()), std::invalid_argument);
  task::v1::TaskFinish finish;
  finish.set_id("source");
  finish.set_token(attempt.token());
  *finish.mutable_daily() = result;
  auto completion = store->prepare_finish(finish);
  completion.prepare_payload();
  const auto prepared =
      data_store->prepare(data_store->verify_download(completion.download_preparation()));
  ASSERT_TRUE(
      (store->commit(store->prepare_publication(std::move(completion), prepared)).task().state() ==
       asterion::task::v1::PUBLISHING));
  store->commit(store->confirm_publication(
      data_store->publish(data_store->verify_publication(store->pending_publications().at(0)))));
  data::v1::HistoryRecord source;
  source.set_version(1);
  *source.mutable_daily() = request;
  *source.mutable_daily_result() = result;
  const auto dataset = data::daily_factor_dataset(source);
  ASSERT_EQ(dataset.bars_size(), 80);
  EXPECT_EQ(dataset.source_dataset_id(), result.manifest_sha256());
  EXPECT_EQ(dataset.manifest_sha256(), result.manifest_sha256());
  EXPECT_EQ(dataset.contract_id(), request.contract_id());
  EXPECT_EQ(dataset.bars(0).amount().units(), Decimal::parse("1.00000001").raw());
  EXPECT_FALSE(dataset.bars(0).has_previous_close());
  EXPECT_FALSE(dataset.bars(0).has_settlement());
  EXPECT_FALSE(dataset.bars(0).has_macd());
  factor::v1::DailyFactorInput input;
  input.set_version(1);
  input.set_lookback(2);
  input.set_horizon(2);
  input.set_holdout_start(40);
  *input.mutable_dataset() = dataset;
  input.set_dataset_revision(protocol::daily_factor_revision(dataset));
  const auto analysis = factor::run_daily(input);
  EXPECT_EQ(analysis.samples_size(), 74);
  factor::v1::DailyFactorRequest parameters;
  parameters.set_source_dataset_id(result.manifest_sha256());
  parameters.set_lookback(2);
  parameters.set_horizon(2);
  parameters.set_holdout_start(40);
  EXPECT_EQ(tasks::submit(*store, "analysis", input).kind(), task::v1::DAILY_FACTOR);
  EXPECT_EQ(tasks::submit(*store, "analysis", input).id(), "analysis");
  const auto usage = store->history_usage(result.manifest_sha256());
  ASSERT_EQ(usage.references_size(), 2);
  const auto factor_reference = std::ranges::find_if(
      usage.references(), [](const auto& row) { return row.id() == "analysis"; });
  ASSERT_NE(factor_reference, usage.references().end());
  EXPECT_EQ(factor_reference->kind(), data::v1::HISTORY_DAILY_FACTOR);
  ASSERT_EQ(factor_reference->roles_size(), 1);
  EXPECT_EQ(factor_reference->roles(0), data::v1::HISTORY_MARKET);

  task::v1::TaskDispatch allowance;
  allowance.set_launch_slots(1);
  const auto launches = store->dispatch(allowance);
  ASSERT_EQ(launches.launches_size(), 1);
  EXPECT_TRUE(launches.launches(0).daily_factor());
  EXPECT_EQ(launches.launches(0).program(), task::v1::FACTOR_PROGRAM);
  EXPECT_FALSE(store->list().tasks(1).has_daily_factor());
  const auto token = store->commit(store->claim("analysis")).token();
  task::v1::TaskFinish finished;
  finished.set_id("analysis");
  finished.set_token(token);
  *finished.mutable_daily_factor() = analysis;
  auto tampered = finished;
  tampered.mutable_daily_factor()->mutable_samples(0)->set_trading_day("2023-01-01");
  auto rejected = store->prepare_finish(tampered);
  EXPECT_THROW(rejected.prepare_payload(), std::invalid_argument);
  EXPECT_THROW(store->commit(store->finish(std::move(rejected))), std::invalid_argument);
  auto verified = store->prepare_finish(finished);
  verified.prepare_payload();
  store->commit(store->cancel("analysis")).task();
  store->commit(store->finish(std::move(verified)));
  EXPECT_EQ(store->get("analysis").state(), task::v1::CANCELLED);
  store->commit(store->retry("analysis")).task();
  const auto next_token = store->commit(store->claim("analysis")).token();
  EXPECT_THROW(store->prepare_finish(finished), std::invalid_argument);
  finished.set_token(next_token);
  verified = store->prepare_finish(finished);
  verified.prepare_payload();
  store->commit(store->finish(std::move(verified)));
  EXPECT_EQ(tasks::daily_factor_result(*store, "analysis").SerializeAsString(),
            analysis.SerializeAsString());
  store.reset();
  store = std::make_unique<tasks::Store>(folder.path,
                                         tasks::Identity{"daily-factor-test", "fixture-data"});
  EXPECT_EQ(store->get("analysis").attempt(), 2);
  EXPECT_EQ(store->get("analysis").daily_factor().SerializeAsString(), input.SerializeAsString());
  EXPECT_EQ(tasks::daily_factor_result(*store, "analysis").SerializeAsString(),
            analysis.SerializeAsString());
  store.reset();
  {
    using namespace std::chrono_literals;
    const auto socket_dir =
        std::filesystem::path("/tmp") / ("ast-df-" + unique_process_id().substr(0, 12));
    std::filesystem::create_directory(socket_dir);
    std::filesystem::permissions(socket_dir, std::filesystem::perms::owner_all);
    struct Cleanup {
      std::filesystem::path path;
      ~Cleanup() {
        std::error_code e;
        std::filesystem::remove_all(path, e);
      }
    } cleanup{socket_dir};
    const auto endpoint = (socket_dir / "task.sock").string();
    const auto workers = (socket_dir / "worker.sock").string();
    data_store.reset();
    const auto data_endpoint = (socket_dir / "data.sock").string();
    const auto data_workers = (socket_dir / "data.workers").string();
    auto data_process = std::make_unique<ChildProcess>(
        ASTERION_DATA_SERVICE_PATH,
        std::vector<std::string>{"--directory", warehouse.path.string(), "--session",
                                 "fixture-data", "--task-instance", "daily-factor-test",
                                 "--endpoint", data_endpoint, "--worker-endpoint", data_workers});
    protocol::DataClient data(data_endpoint, "fixture-data");
    ChildProcess service(ASTERION_TASK_SERVICE_PATH,
                         {"--directory", folder.path.string(), "--endpoint", endpoint,
                          "--worker-endpoint", workers, "--session", "daily-factor-test",
                          "--data-instance", "fixture-data", "--data-endpoint", data_workers});
    auto call = [&](task::v1::TaskRequest request, bool worker = false) {
      request.set_version(1);
      request.set_service_id("daily-factor-test");
      request.set_correlation_id(unique_process_id());
      auto channel = ipc::Channel::connect(worker ? workers : endpoint, 2s);
      channel.send(request.SerializeAsString(), 2s);
      task::v1::TaskResponse reply;
      if (!reply.ParseFromString(channel.receive(2s)))
        throw std::runtime_error("bad response");
      if (reply.has_error())
        throw std::runtime_error(reply.error().message());
      return reply;
    };
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    for (;;) {
      try {
        task::v1::TaskRequest ping;
        ping.mutable_heartbeat();
        const bool task_ready = call(ping).health().initialized();
        data::v1::DataRequest data_ping;
        data_ping.mutable_heartbeat();
        if (task_ready && data.call(data_ping).health().initialized())
          break;
      } catch (const std::exception&) {
        if (service.exited())
          throw;
      }
      if (std::chrono::steady_clock::now() > deadline)
        throw std::runtime_error("task fixture did not initialize");
      std::this_thread::sleep_for(20ms);
    }
    task::v1::TaskRequest submit;
    submit.mutable_submit()->set_id("worker-analysis");
    *submit.mutable_submit()->mutable_daily_factor() = parameters;
    EXPECT_EQ(call(submit).task().kind(), task::v1::DAILY_FACTOR);
    // The worker reads fixed versions from the paired Data service.
    task::v1::TaskRequest dispatch;
    dispatch.mutable_dispatch()->set_launch_slots(2);
    const auto launches = call(dispatch, true).launches();
    ASSERT_EQ(launches.launches_size(), 1);
    EXPECT_TRUE(launches.launches(0).daily_factor());
    {
      ChildProcess wrong(ASTERION_FACTOR_PATH, {"--endpoint", workers, "--session",
                                                "daily-factor-test", "--task", "worker-analysis"});
      ASSERT_TRUE(wrong.wait(10s));
      EXPECT_NE(wrong.exit_code(), 0);
    }
    {
      ChildProcess worker(ASTERION_FACTOR_PATH,
                          {"--endpoint", workers, "--session", "daily-factor-test", "--task",
                           "worker-analysis", "--daily-factor"});
      ASSERT_TRUE(worker.wait(10s));
      EXPECT_EQ(worker.exit_code(), 0);
    }
    task::v1::TaskRequest outcome;
    outcome.mutable_result()->set_id("worker-analysis");
    data_process.reset(); // Confirmed results retain their own experiment evidence.
    const auto reply = call(outcome);
    EXPECT_EQ(reply.daily_factor().SerializeAsString(), analysis.SerializeAsString());
    EXPECT_EQ(reply.result_task().state(), task::v1::SUCCEEDED);
    const auto evidence = protocol::decode_task_result(reply, "worker-analysis");
    EXPECT_EQ(evidence.at("kind"), "daily_factor");
    EXPECT_EQ(evidence.at("experiment").at("data").at("source_dataset_id"),
              result.manifest_sha256());
    EXPECT_EQ(evidence.at("experiment").at("data").at("history_evidence"),
              protocol::decode_history_evidence(input.dataset().history_evidence()));
    EXPECT_EQ(input.dataset().history_evidence().acquired_at_ns(),
              history_files::inspect_daily(attempt.output_directory()).acquired_at_ns);
  }
  store = std::make_unique<tasks::Store>(folder.path,
                                         tasks::Identity{"daily-factor-test", "fixture-data"});
  EXPECT_EQ(tasks::daily_factor_result(*store, "worker-analysis").SerializeAsString(),
            analysis.SerializeAsString());
  auto wrong = source;
  wrong.mutable_daily_result()->set_manifest_sha256(std::string(64, 'a'));
  EXPECT_THROW(data::daily_factor_dataset(wrong), std::invalid_argument);
  wrong = source;
  wrong.mutable_daily_result()->set_rows(result.rows() + 1);
  EXPECT_THROW(data::daily_factor_dataset(wrong), std::invalid_argument);
  auto mismatched = source;
  mismatched.mutable_daily()->set_contract_id("SHFE/cu/2024-04");
  EXPECT_THROW(data::daily_factor_dataset(mismatched), std::invalid_argument);
  mismatched = source;
  mismatched.mutable_daily()->set_end_day("2023-04-02");
  EXPECT_THROW(data::daily_factor_dataset(mismatched), std::invalid_argument);
  auto manifest =
      Json::parse(contents(std::filesystem::path(attempt.output_directory()) / "daily.json"));
  manifest["complete"] = false;
  manifest["acquired_at_ns"] = "0";
  replace_file_durably(std::filesystem::path(attempt.output_directory()) / "daily.json",
                       manifest.dump());
  EXPECT_THROW(data::daily_factor_dataset(source), std::invalid_argument);
  replace_file_durably(std::filesystem::path(attempt.output_directory()) / "daily-0.parquet", "{}");
  EXPECT_THROW(data::daily_factor_dataset(source), std::invalid_argument);
  // The accepted snapshot remains usable after its source is damaged; no lazy file references.
  EXPECT_EQ(factor::run_daily(input).SerializeAsString(), analysis.SerializeAsString());
}
TEST(DailyTasks, ProviderArtifactIsImmutableAcrossRetryAndStoreRestart) {
  Folder folder, warehouse;
  data::Store data(warehouse.path, "fixture-data", "daily-test");
  const auto input = asterion::testing_support::daily_request({{"version", 2},
                                                               {"contract_id", "SHFE/cu/2024-03"},
                                                               {"source", "tushare.fut_daily"},
                                                               {"source_instrument", "CU2403.SHF"},
                                                               {"begin_day", "2024-01-02"},
                                                               {"end_day", "2024-01-03"},
                                                               {"requests_per_minute", 60}});
  std::string artifact;
  {
    tasks::Store store(folder.path, tasks::Identity{"daily-test", "fixture-data"});
    artifact = tasks::submit(store, test::authorize_download(data, "daily-test", "pinned", input))
                   .provider_artifact();
    ASSERT_EQ(artifact.size(), 64);
    store.commit(store.cancel("pinned")).task();
  }
  {
    tasks::Store store(folder.path, tasks::Identity{"daily-test", "fixture-data"});
    EXPECT_EQ(store.commit(store.retry("pinned")).task().provider_artifact(), artifact);
    task::v1::TaskDispatch allowance;
    allowance.set_launch_slots(1);
    EXPECT_EQ(store.dispatch(allowance).launches(0).provider_artifact(), artifact);
  }
  std::string evidence;
  {
    sqlite::Database database(folder.path / "tasks.sqlite");
    sqlite::Database::Statement read(database, "SELECT manifest FROM tasks WHERE id='pinned'");
    ASSERT_TRUE(read.step());
    auto manifest = Json::parse(read.text(0));
    manifest["version"] = 2;
    manifest.erase("provider_artifact");
    evidence = manifest.dump();
    sqlite::Database::Statement write(database, "UPDATE tasks SET manifest=? WHERE id='pinned'");
    write.bind(1, evidence).step();
  }
  EXPECT_THROW(tasks::Store(folder.path, tasks::Identity{"daily-test", "fixture-data"}),
               std::invalid_argument);
  sqlite::Database database(folder.path / "tasks.sqlite");
  sqlite::Database::Statement read(database, "SELECT manifest FROM tasks WHERE id='pinned'");
  ASSERT_TRUE(read.step());
  EXPECT_EQ(read.text(0), evidence) << "unsupported task evidence is kept, not rewritten";
}
