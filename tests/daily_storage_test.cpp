#include "sqlite_database.hpp"
#include "daily_factor_source.hpp"
#include "factor_engine.hpp"
#include "history_daily.hpp"
#include "tushare.hpp"
#include "task_store.hpp"
#include <asterion/protocol/data.hpp>
#include <limits>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/file_lock.hpp>
#include <fstream>
#include <thread>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <gtest/gtest.h>
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
      history_files::download_daily(provider, range(), folder.path, 500, stop.get_token(),
                                    [&](unsigned completed, unsigned total, std::uint64_t rows) {
                                      EXPECT_EQ(total, 2);
                                      EXPECT_EQ(rows, completed);
                                      if (completed == 1)
                                        stop.request_stop();
                                    }),
      std::runtime_error);
  const auto partial = history_files::inspect_daily(folder.path);
  EXPECT_FALSE(partial.complete);
  EXPECT_EQ(partial.pages, 1);
  EXPECT_THROW(history_files::read_daily(folder.path), std::invalid_argument);
  const auto first = contents(folder.path / "daily-0.json");
  const auto result = history_files::download_daily(provider, range(), folder.path, 500);
  EXPECT_TRUE(result.complete);
  EXPECT_EQ(result.rows, 2);
  EXPECT_EQ(starts, (std::vector<std::string>{"20230101", "20240102"}));
  EXPECT_EQ(first, contents(folder.path / "daily-0.json"));
  const auto data = history_files::read_daily(folder.path);
  ASSERT_EQ(data.bars.size(), 2);
  EXPECT_EQ(data.info.manifest_sha256, result.manifest_sha256);
  EXPECT_EQ(format_trading_date(data.bars[1].trading_day), "2024-01-02");
  EXPECT_EQ(data.bars[0].open.str(), "100.00000001");
  EXPECT_EQ(data.bars[0].amount.str(), "1234567.89012345");
  EXPECT_FALSE(data.bars[0].previous_close);
  EXPECT_EQ(data.bars[0].previous_settlement, Decimal::parse("99.5"));
  EXPECT_EQ(history_files::download_daily(provider, range(), folder.path, 500).rows, 2);
  EXPECT_EQ(starts.size(), 2);
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
  history_files::download_daily(provider, range(), folder.path, 500);
  auto manifest = Json::parse(contents(folder.path / "daily.json"));
  manifest["pages"].erase(1);
  manifest["rows"] = 1;
  manifest["complete"] = false;
  replace_file_durably(folder.path / "daily.json", manifest.dump());
  EXPECT_TRUE(history_files::download_daily(provider, range(), folder.path, 500).complete);
  EXPECT_EQ(requests, 2); // No HTTP call for the validated orphan segment.
  auto changed = range();
  changed.end = std::chrono::year(2024) / std::chrono::January / 4;
  EXPECT_THROW(history_files::download_daily(provider, changed, folder.path, 500),
               std::invalid_argument);
  replace_file_durably(folder.path / "daily-1.json", "{}");
  EXPECT_THROW(history_files::read_daily(folder.path), std::invalid_argument);
  EXPECT_THROW(history_files::download_daily(provider, range(), folder.path, 500),
               std::invalid_argument);
  EXPECT_EQ(requests, 2);
}
TEST(DailyStorage, SharedReadersExcludeWritersAndRejectForeignDirectories) {
  Folder folder;
  tushare::Daily provider("fixture", [](const auto& body, auto) { return response(body); });
  provider.start();
  history_files::download_daily(provider, range(), folder.path, 500);
  {
    FileLock reader(folder.path, "daily.lock", FileLock::Access::shared);
    EXPECT_EQ(history_files::read_daily(folder.path).bars.size(), 2);
    EXPECT_THROW(history_files::download_daily(provider, range(), folder.path, 500),
                 std::exception);
  }
  {
    FileLock writer(folder.path, "daily.lock");
    EXPECT_THROW(history_files::read_daily(folder.path), std::exception);
  }
  Folder foreign;
  replace_file_durably(foreign.path / "existing-data", "preserve");
  EXPECT_THROW(history_files::download_daily(provider, range(), foreign.path, 500),
               std::invalid_argument);
  EXPECT_EQ(contents(foreign.path / "existing-data"), "preserve");
}

TEST(DailyTasks, ValidatesDefinitionsDispatchesDailyAndRestoresVerifiedResults) {
  Folder folder;
  const Json definition = {{"version", 2},
                           {"contract_id", "SHFE/cu/2024-03"},
                           {"source", "tushare.fut_daily"},
                           {"source_instrument", "CU2403.SHF"},
                           {"begin_day", "2023-01-01"},
                           {"end_day", "2024-01-03"},
                           {"requests_per_minute", 500}};
  const auto input = history_files::daily_request(definition);
  EXPECT_EQ(history_files::daily_request_json(input), definition);
  for (const auto& mutation : std::vector<Json>{{{"version", 1}},
                                                {{"requests_per_minute", 0}},
                                                {{"requests_per_minute", 501}},
                                                {{"requests_per_minute", 1.5}},
                                                {{"begin_day", "2023-02-29"}},
                                                {{"end_day", "2022-01-01"}},
                                                {{"contract_id", "CU.SHF"}},
                                                {{"unknown", 1}}}) {
    auto bad = definition;
    bad.update(mutation);
    EXPECT_THROW(history_files::daily_request(bad), std::exception);
  }
  {
    tasks::Store store(folder.path);
    const auto task = store.submit("daily", input, "fixture-secret");
    EXPECT_EQ(task.kind(), research::v1::DAILY_DOWNLOAD);
    EXPECT_EQ(task.total(), 2);
    EXPECT_EQ(store.submit("daily", input, "replacement-must-not-overwrite").submission_sequence(),
              task.submission_sequence());
    const auto dispatch = store.dispatch({});
    ASSERT_EQ(dispatch.launches_size(), 1);
    EXPECT_EQ(dispatch.launches(0).provider_artifact(), store.get("daily").provider_artifact());
    EXPECT_EQ(dispatch.launches(0).provider_artifact().size(), 64);
    EXPECT_TRUE(dispatch.launches(0).daily_download());
    EXPECT_FALSE(dispatch.launches(0).minute_download());
    EXPECT_EQ(dispatch.launches(0).program(), research::v1::DATA_PIPELINE_PROGRAM);
    research::v1::TaskAttempt attempt;
    attempt.set_token(store.claim("daily"));
    *attempt.mutable_task() = store.get("daily");
    store.download_attempt(attempt);
    EXPECT_EQ(attempt.provider_token(), "fixture-secret");
    const auto dir = std::filesystem::path(attempt.output_directory());
    EXPECT_EQ(dir, folder.path / "history" / "SHFE" / "cu" / "2024-03" / "tushare.fut_daily" /
                       "daily" / "daily");
    tushare::Daily provider(attempt.provider_token(),
                            [](const auto& body, auto) { return response(body); });
    provider.start();
    history_files::download_daily(provider, history_files::daily_range(input), dir, 500);
    research::v1::TaskFinish finish;
    finish.set_id("daily");
    finish.set_token(attempt.token());
    *finish.mutable_daily() = history_files::daily_result(dir);
    auto wrong = finish;
    wrong.mutable_daily()->set_directory(folder.path.string());
    EXPECT_THROW(store.prepare_finish(wrong), std::invalid_argument);
    wrong = finish;
    wrong.mutable_daily()->set_rows(999);
    auto invalid = store.prepare_finish(wrong);
    EXPECT_THROW(invalid.verify(), std::invalid_argument);
    EXPECT_THROW(store.finish(std::move(invalid)), std::invalid_argument);
    auto completion = store.prepare_finish(finish);
    completion.verify();
    store.finish(std::move(completion));
    EXPECT_EQ(store.get("daily").state(), research::v1::SUCCEEDED);
    EXPECT_EQ(store.daily_result("daily").rows(), 2);
    EXPECT_THROW(store.minute_result("daily"), std::invalid_argument);
    const auto summary = store.list();
    EXPECT_FALSE(summary.tasks(0).has_daily());
    EXPECT_EQ(protocol::decode_task(summary.tasks(0)).at("kind"), "daily_download");
    research::v1::TaskResponse result;
    *result.mutable_result_task() = store.get("daily");
    *result.mutable_daily() = store.daily_result("daily");
    EXPECT_EQ(protocol::decode_task_result(result, "daily").at("experiment").at("begin_day"),
              "2023-01-01");
    result.mutable_daily()->set_manifest_sha256(std::string(64, 'z'));
    EXPECT_THROW(protocol::decode_task_result(result, "daily"), std::invalid_argument);
    for (const auto& file : std::filesystem::recursive_directory_iterator(folder.path))
      if (file.is_regular_file() && file.path().filename() != "provider.credential")
        EXPECT_EQ(contents(file.path()).find("fixture-secret"), std::string::npos);
  }
  tasks::Store restored(folder.path);
  EXPECT_EQ(restored.daily_result("daily").rows(), 2);
  EXPECT_EQ(restored.get("daily").daily().SerializeAsString(), input.SerializeAsString());
  EXPECT_TRUE(restored.dispatch({}).launches().empty());
}
TEST(DailyTasks, CancellationFencesVerifiedCompletionAndRetryReusesStoredPages) {
  Folder folder;
  const auto input = history_files::daily_request({{"version", 2},
                                                   {"contract_id", "SHFE/cu/2024-03"},
                                                   {"source", "tushare.fut_daily"},
                                                   {"source_instrument", "CU2403.SHF"},
                                                   {"begin_day", "2024-01-02"},
                                                   {"end_day", "2024-01-03"},
                                                   {"requests_per_minute", 500}});
  std::string first_token;
  research::v1::TaskFinish finish;
  {
    tasks::Store store(folder.path);
    store.submit("daily", input, "fixture");
    research::v1::TaskAttempt attempt;
    first_token = store.claim("daily");
    attempt.set_token(first_token);
    *attempt.mutable_task() = store.get("daily");
    store.download_attempt(attempt);
    tushare::Daily provider("fixture", [](const auto& body, auto) { return response(body); });
    provider.start();
    history_files::download_daily(provider, history_files::daily_range(input),
                                  attempt.output_directory(), 500);
    finish.set_id("daily");
    finish.set_token(first_token);
    *finish.mutable_daily() = history_files::daily_result(attempt.output_directory());
    auto completion = store.prepare_finish(finish);
    completion.verify();
    store.cancel("daily");
    store.finish(std::move(completion));
    EXPECT_EQ(store.get("daily").state(), research::v1::CANCELLED);
    EXPECT_THROW(store.daily_result("daily"), std::invalid_argument);
    store.retry("daily");
  }
  tasks::Store restored(folder.path);
  EXPECT_EQ(restored.get("daily").state(), research::v1::QUEUED);
  research::v1::TaskAttempt attempt;
  attempt.set_token(restored.claim("daily"));
  EXPECT_NE(first_token, attempt.token());
  *attempt.mutable_task() = restored.get("daily");
  restored.download_attempt(attempt);
  EXPECT_THROW(restored.prepare_finish(finish), std::invalid_argument);
  int requests = 0;
  tushare::Daily provider("fixture", [&](const auto& body, auto) {
    ++requests;
    return response(body);
  });
  provider.start();
  history_files::download_daily(provider, history_files::daily_range(input),
                                attempt.output_directory(), 500);
  EXPECT_EQ(requests, 0);
  finish.set_token(attempt.token());
  auto completion = restored.prepare_finish(finish);
  completion.verify();
  restored.finish(std::move(completion));
  EXPECT_EQ(restored.get("daily").attempt(), 2);
  EXPECT_EQ(restored.daily_result("daily").rows(), 1);
}

TEST(DailyTasks, RealServiceDispatchAndManagedWorkerResumeCompletedSourceData) {
  using namespace std::chrono_literals;
  Folder folder;
  const auto input = history_files::daily_request({{"version", 2},
                                                   {"contract_id", "SHFE/cu/2024-03"},
                                                   {"source", "tushare.fut_daily"},
                                                   {"source_instrument", "CU2403.SHF"},
                                                   {"begin_day", "2024-01-02"},
                                                   {"end_day", "2024-01-03"},
                                                   {"requests_per_minute", 500}});
  {
    tasks::Store store(folder.path);
    store.submit("daily", input, "fixture");
    const auto dir = folder.path / "history" / "SHFE" / "cu" / "2024-03" / "tushare.fut_daily" /
                     "daily" / "daily";
    std::filesystem::create_directories(dir);
    tushare::Daily provider("fixture", [](const auto& body, auto) { return response(body); });
    provider.start();
    history_files::download_daily(provider, history_files::daily_range(input), dir, 500);
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
  ChildProcess service(ASTERION_TASK_SERVICE_PATH,
                       {"--directory", folder.path.string(), "--endpoint", endpoint,
                        "--worker-endpoint", worker_endpoint, "--session", "daily-test"});
  auto call = [&](research::v1::TaskRequest request, bool worker = false) {
    request.set_version(1);
    request.set_service_id("daily-test");
    request.set_correlation_id(unique_process_id());
    auto channel = ipc::Channel::connect(worker ? worker_endpoint : endpoint, 2s);
    channel.send(request.SerializeAsString(), 2s);
    research::v1::TaskResponse reply;
    if (!reply.ParseFromString(channel.receive(2s)))
      throw std::runtime_error("bad response");
    protocol::validate_message(reply);
    EXPECT_EQ(reply.correlation_id(), request.correlation_id());
    return reply;
  };
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  for (;;) {
    try {
      research::v1::TaskRequest ping;
      ping.mutable_heartbeat();
      call(ping, true);
      break;
    } catch (const std::exception&) {
      if (service.exited() || std::chrono::steady_clock::now() > deadline)
        throw;
      std::this_thread::sleep_for(20ms);
    }
  }
  research::v1::TaskRequest request;
  request.mutable_dispatch();
  EXPECT_TRUE(call(request).has_error());
  const auto launches = call(request, true);
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
  request.mutable_result()->set_id("daily");
  auto result = call(request);
  ASSERT_TRUE(result.has_daily()) << result.DebugString();
  EXPECT_EQ(result.daily().rows(), 1);
  EXPECT_EQ(protocol::decode_task_result(result, "daily").at("kind"), "daily_download");
  EXPECT_EQ(result.result_task().state(), research::v1::SUCCEEDED);
  request.mutable_daily_page()->set_task_id("daily");
  request.mutable_daily_page()->set_limit(5);
  const auto page = call(request);
  ASSERT_TRUE(page.has_daily_page()) << page.DebugString();
  EXPECT_EQ(protocol::decode_daily_page(page.daily_page()).at("bars").size(), 1);
  EXPECT_EQ(page.daily_page().bars(0).trading_day(), "2024-01-02");
  request.mutable_daily_page()->set_task_id("../daily");
  EXPECT_TRUE(call(request).has_error());

  request.mutable_submit()->set_id("queued");
  *request.mutable_submit()->mutable_daily() = input;
  request.mutable_submit()->set_provider_token("fixture-secret");
  auto submitted = call(request);
  ASSERT_TRUE(submitted.has_task()) << submitted.DebugString();
  EXPECT_EQ(submitted.task().kind(), research::v1::DAILY_DOWNLOAD);
  EXPECT_EQ(submitted.SerializeAsString().find("fixture-secret"), std::string::npos);
  request.mutable_cancel()->set_id("queued");
  EXPECT_EQ(call(request).task().state(), research::v1::CANCELLED);
  request.mutable_list();
  const auto summary = call(request);
  ASSERT_TRUE(summary.has_tasks());
  ASSERT_EQ(summary.tasks().tasks_size(), 2);
  EXPECT_FALSE(summary.tasks().tasks(0).has_daily());
  EXPECT_EQ(summary.SerializeAsString().find("fixture-secret"), std::string::npos);
}

TEST(DailyPages, DatePagingKeepsExactPricesMissingSettlementAndDatasetOriginMacd) {
  using namespace std::chrono;
  Folder folder;
  const auto input = history_files::daily_request({{"version", 2},
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
  history_files::download_daily(provider, history_files::daily_range(input), folder.path, 500);
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
  replace_file_durably(folder.path / "daily-0.json", "{}");
  EXPECT_THROW(history_files::read_daily_page(input, result, query), std::invalid_argument);
}
TEST(DailyPages, EmptyDatasetRetainsRequestedDatesWithoutInventedCoverage) {
  Folder folder;
  const auto input = history_files::daily_request({{"version", 2},
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
  history_files::download_daily(provider, history_files::daily_range(input), folder.path, 500);
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
  const auto input = history_files::daily_request({{"version", 2},
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
  history_files::download_daily(provider, history_files::daily_range(input), folder.path, 500);
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
  Folder folder;
  auto store = std::make_unique<tasks::Store>(folder.path);
  const auto request = history_files::daily_request({{"version", 2},
                                                     {"contract_id", "SHFE/cu/2024-03"},
                                                     {"source", "tushare.fut_daily"},
                                                     {"source_instrument", "CU2403.SHF"},
                                                     {"begin_day", "2023-01-01"},
                                                     {"end_day", "2023-04-01"},
                                                     {"requests_per_minute", 500}});
  store->submit("source", request, "fixture");
  research::v1::TaskAttempt attempt;
  attempt.set_token(store->claim("source"));
  *attempt.mutable_task() = store->get("source");
  store->download_attempt(attempt);
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
                                attempt.output_directory(), 500);
  const auto result = history_files::daily_result(attempt.output_directory());
  EXPECT_THROW(tasks::daily_factor_dataset(store->get("source"), result), std::invalid_argument);
  research::v1::TaskFinish finish;
  finish.set_id("source");
  finish.set_token(attempt.token());
  *finish.mutable_daily() = result;
  auto completion = store->prepare_finish(finish);
  completion.verify();
  store->finish(std::move(completion));
  const auto source = store->get("source");
  const auto dataset = tasks::daily_factor_dataset(source, store->daily_result("source"));
  ASSERT_EQ(dataset.bars_size(), 80);
  EXPECT_EQ(dataset.source_task_id(), "source");
  EXPECT_EQ(dataset.manifest_sha256(), result.manifest_sha256());
  EXPECT_EQ(dataset.contract_id(), request.contract_id());
  EXPECT_EQ(dataset.bars(0).amount().units(), Decimal::parse("1.00000001").raw());
  EXPECT_FALSE(dataset.bars(0).has_previous_close());
  EXPECT_FALSE(dataset.bars(0).has_settlement());
  EXPECT_FALSE(dataset.bars(0).has_macd());
  research::v1::DailyFactorInput input;
  input.set_version(1);
  input.set_lookback(2);
  input.set_horizon(2);
  input.set_holdout_start(40);
  *input.mutable_dataset() = dataset;
  input.set_dataset_revision(protocol::daily_factor_revision(dataset));
  const auto analysis = factor::run_daily(input);
  EXPECT_EQ(analysis.samples_size(), 74);
  research::v1::DailyFactorRequest parameters;
  parameters.set_source_task_id("source");
  parameters.set_lookback(2);
  parameters.set_horizon(2);
  parameters.set_holdout_start(40);
  auto submission = store->prepare_daily_factor("analysis", parameters);
  EXPECT_THROW(store->submit(submission), std::invalid_argument);
  submission.verify();
  EXPECT_EQ(store->submit(submission).kind(), research::v1::DAILY_FACTOR);
  EXPECT_EQ(store->submit(submission).id(), "analysis");
  ASSERT_EQ(store->dispatch({}).launches_size(), 1);
  EXPECT_TRUE(store->dispatch({}).launches(0).daily_factor());
  EXPECT_EQ(store->dispatch({}).launches(0).program(), research::v1::FACTOR_PROGRAM);
  EXPECT_FALSE(store->list().tasks(1).has_daily_factor());
  const auto token = store->claim("analysis");
  research::v1::TaskFinish finished;
  finished.set_id("analysis");
  finished.set_token(token);
  *finished.mutable_daily_factor() = analysis;
  auto tampered = finished;
  tampered.mutable_daily_factor()->mutable_samples(0)->set_value(42);
  auto rejected = store->prepare_finish(tampered);
  EXPECT_THROW(rejected.verify(), std::invalid_argument);
  EXPECT_THROW(store->finish(std::move(rejected)), std::invalid_argument);
  auto verified = store->prepare_finish(finished);
  verified.verify();
  store->cancel("analysis");
  store->finish(std::move(verified));
  EXPECT_EQ(store->get("analysis").state(), research::v1::CANCELLED);
  store->retry("analysis");
  const auto next_token = store->claim("analysis");
  EXPECT_THROW(store->prepare_finish(finished), std::invalid_argument);
  finished.set_token(next_token);
  verified = store->prepare_finish(finished);
  verified.verify();
  store->finish(std::move(verified));
  EXPECT_EQ(store->daily_factor_result("analysis").SerializeAsString(),
            analysis.SerializeAsString());
  store.reset();
  store = std::make_unique<tasks::Store>(folder.path);
  EXPECT_EQ(store->get("analysis").attempt(), 2);
  EXPECT_EQ(store->get("analysis").daily_factor().SerializeAsString(), input.SerializeAsString());
  EXPECT_EQ(store->daily_factor_result("analysis").SerializeAsString(),
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
    ChildProcess service(ASTERION_TASK_SERVICE_PATH,
                         {"--directory", folder.path.string(), "--endpoint", endpoint,
                          "--worker-endpoint", workers, "--session", "daily-factor-test"});
    auto call = [&](research::v1::TaskRequest request, bool worker = false) {
      request.set_version(1);
      request.set_service_id("daily-factor-test");
      request.set_correlation_id(unique_process_id());
      auto channel = ipc::Channel::connect(worker ? workers : endpoint, 2s);
      channel.send(request.SerializeAsString(), 2s);
      research::v1::TaskResponse reply;
      if (!reply.ParseFromString(channel.receive(2s)))
        throw std::runtime_error("bad response");
      if (reply.has_error())
        throw std::runtime_error(reply.error().message());
      return reply;
    };
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    for (;;) {
      try {
        research::v1::TaskRequest ping;
        ping.mutable_heartbeat();
        call(ping);
        break;
      } catch (const std::exception&) {
        if (service.exited() || std::chrono::steady_clock::now() > deadline)
          throw;
        std::this_thread::sleep_for(20ms);
      }
    }
    research::v1::TaskRequest submit;
    submit.mutable_submit()->set_id("worker-analysis");
    *submit.mutable_submit()->mutable_daily_factor() = parameters;
    EXPECT_EQ(call(submit).task().kind(), research::v1::DAILY_FACTOR);
    research::v1::TaskRequest dispatch;
    dispatch.mutable_dispatch();
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
    research::v1::TaskRequest outcome;
    outcome.mutable_result()->set_id("worker-analysis");
    const auto reply = call(outcome);
    EXPECT_EQ(reply.daily_factor().SerializeAsString(), analysis.SerializeAsString());
    EXPECT_EQ(reply.result_task().state(), research::v1::SUCCEEDED);
    const auto evidence = protocol::decode_task_result(reply, "worker-analysis");
    EXPECT_EQ(evidence.at("kind"), "daily_factor");
    EXPECT_EQ(evidence.at("experiment").at("data").at("source_task_id"), "source");
  }
  store = std::make_unique<tasks::Store>(folder.path);
  EXPECT_EQ(store->daily_factor_result("worker-analysis").SerializeAsString(),
            analysis.SerializeAsString());
  auto wrong = result;
  wrong.set_manifest_sha256(std::string(64, 'a'));
  EXPECT_THROW(tasks::daily_factor_dataset(source, wrong), std::invalid_argument);
  wrong = result;
  wrong.set_rows(result.rows() + 1);
  EXPECT_THROW(tasks::daily_factor_dataset(source, wrong), std::invalid_argument);
  auto mismatched = source;
  mismatched.mutable_daily()->set_contract_id("SHFE/cu/2024-04");
  EXPECT_THROW(tasks::daily_factor_dataset(mismatched, result), std::invalid_argument);
  mismatched = source;
  mismatched.mutable_daily()->set_end_day("2023-04-02");
  EXPECT_THROW(tasks::daily_factor_dataset(mismatched, result), std::invalid_argument);
  auto manifest =
      Json::parse(contents(std::filesystem::path(attempt.output_directory()) / "daily.json"));
  manifest["complete"] = false;
  replace_file_durably(std::filesystem::path(attempt.output_directory()) / "daily.json",
                       manifest.dump());
  EXPECT_THROW(tasks::daily_factor_dataset(source, result), std::invalid_argument);
  replace_file_durably(std::filesystem::path(attempt.output_directory()) / "daily-0.json", "{}");
  EXPECT_THROW(tasks::daily_factor_dataset(source, result), std::invalid_argument);
  // The accepted snapshot remains usable after its source is damaged; no lazy file references.
  EXPECT_EQ(factor::run_daily(input).SerializeAsString(), analysis.SerializeAsString());
}
TEST(DailyTasks, ProviderArtifactIsImmutableAcrossRetryAndStoreRestart) {
  Folder folder;
  const auto input = history_files::daily_request({{"version", 2},
                                                   {"contract_id", "SHFE/cu/2024-03"},
                                                   {"source", "tushare.fut_daily"},
                                                   {"source_instrument", "CU2403.SHF"},
                                                   {"begin_day", "2024-01-02"},
                                                   {"end_day", "2024-01-03"},
                                                   {"requests_per_minute", 60}});
  std::string artifact;
  {
    tasks::Store store(folder.path);
    artifact = store.submit("pinned", input, "fixture").provider_artifact();
    ASSERT_EQ(artifact.size(), 64);
    store.cancel("pinned");
  }
  {
    tasks::Store store(folder.path);
    EXPECT_EQ(store.retry("pinned").provider_artifact(), artifact);
    EXPECT_EQ(store.dispatch({}).launches(0).provider_artifact(), artifact);
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
  EXPECT_THROW(tasks::Store{folder.path}, std::invalid_argument);
  sqlite::Database database(folder.path / "tasks.sqlite");
  sqlite::Database::Statement read(database, "SELECT manifest FROM tasks WHERE id='pinned'");
  ASSERT_TRUE(read.step());
  EXPECT_EQ(read.text(0), evidence) << "unsupported task evidence is kept, not rewritten";
}
