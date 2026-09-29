#include <asterion/kernel/process/file_lock.hpp>
#include "tushare.hpp"
#include "minutes.hpp"
#include "task_store.hpp"
#include <asterion/protocol/data.hpp>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <thread>
#include <asterion/kernel/durable_file.hpp>
#include <gtest/gtest.h>
#include <fstream>
#include <chrono>
#include <random>
#include <cmath>
#include <limits>
using namespace asterion;
namespace {
const auto begin = tushare::parse_time("2023-08-25 09:00:00");
HistoricalBarRange range() {
  return {{"SHFE", "CU2310"}, 1, begin, begin + 600000000000LL};
}
std::string response(const std::string& time = "2023-08-25 09:01:00") {
  return "{\"code\":0,\"data\":{\"fields\":[\"ts_code\",\"trade_time\",\"open\",\"high\",\"low\","
         "\"close\",\"vol\",\"amount\",\"oi\"],\"items\":[[\"CU2310.SHF\",\"" +
         time + "\",100.1,102,99.9,101.2,20.0,12345678.12345678,2000]]}}";
}
class Folder {
public:
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("asterion-minutes-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
       std::to_string(std::random_device{}()));
  Folder() { std::filesystem::create_directory(path); }
  ~Folder() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};
} // namespace
TEST(Tushare, ParsesExactPricesAndAmountAndRequest) {
  tushare::Minutes plugin("fixture-token", [](const auto& body, auto) {
    auto request = Json::parse(body);
    EXPECT_EQ(request.at("api_name"), "ft_mins");
    EXPECT_EQ(request.at("params").at("freq"), "1min");
    EXPECT_EQ(request.at("params").at("start_date"), "2023-08-25 09:00:00");
    return response();
  });
  plugin.start();
  auto rows = plugin.read(range(), {});
  ASSERT_EQ(rows.size(), 1);
  EXPECT_EQ(rows[0].amount.str(), "12345678.12345678");
  EXPECT_EQ(rows[0].open.str(), "100.1");
  EXPECT_EQ(rows[0].timestamp_ns, begin + 60000000000LL);
}
TEST(Tushare, RejectsAliasesMalformedTimeAndUnsupportedIntervals) {
  EXPECT_THROW(tushare::instrument("CU.SHF"), std::invalid_argument);
  EXPECT_THROW(tushare::instrument("CU8888.SHF/../x"), std::invalid_argument);
  EXPECT_THROW(tushare::parse_time("2023-02-29 09:00:00"), std::invalid_argument);
  EXPECT_THROW(tushare::parse_time("2023-08-25 25:00:00"), std::invalid_argument);
  EXPECT_EQ(tushare::format_time(begin), "2023-08-25 09:00:00");
  auto input = range();
  input.interval_minutes = 2;
  EXPECT_THROW(tushare::validate(input), std::invalid_argument);
}
TEST(Tushare, RejectsInvalidRowsAndTruncationWithoutEchoingProviderSecrets) {
  std::vector<std::string> cases;
  auto json = Json::parse(response());
  auto duplicate = json;
  duplicate["data"]["items"].push_back(duplicate["data"]["items"][0]);
  cases.push_back(duplicate.dump());
  auto wrong = json;
  wrong["data"]["items"][0][0] = "CU2401.SHF";
  cases.push_back(wrong.dump());
  auto missing = json;
  missing["data"]["items"][0][8] = nullptr;
  cases.push_back(missing.dump());
  auto invalid = json;
  invalid["data"]["items"][0][3] = 90;
  cases.push_back(invalid.dump());
  auto large = json;
  large["data"]["items"] = Json::array();
  for (int i = 0; i < 8000; ++i)
    large["data"]["items"].push_back(json["data"]["items"][0]);
  cases.push_back(large.dump());
  cases.push_back(response("2023-08-26 09:01:00"));
  cases.push_back("{\"code\":0,\"code\":0}");
  cases.push_back("{\"code\":-2002,\"msg\":\"fixture-secret\"}");
  for (const auto& raw : cases) {
    tushare::Minutes p("fixture-secret", [&](const auto&, auto) { return raw; });
    p.start();
    try {
      (void)p.read(range(), {});
      FAIL() << "invalid response accepted";
    } catch (const std::exception& error) {
      EXPECT_EQ(std::string(error.what()).find("fixture-secret"), std::string::npos);
    }
  }
}
TEST(Tushare, SortsSourceRowsAndPreservesScientificDecimal) {
  auto raw = Json::parse(response());
  raw["data"]["items"].push_back(raw["data"]["items"][0]);
  raw["data"]["items"][0][1] = "2023-08-25 09:02:00";
  raw["data"]["items"][0][7] = "1.234567812345678e7";
  tushare::Minutes p("fixture", [&](const auto&, auto) { return raw.dump(); });
  p.start();
  auto rows = p.read(range(), {});
  EXPECT_LT(rows[0].timestamp_ns, rows[1].timestamp_ns);
  EXPECT_EQ(rows[1].amount.str(), "12345678.12345678");
}
TEST(Tushare, ResumeSkipsDurablePagesAndRejectsChangedInputOrDamagedPage) {
  Folder folder;
  auto input = range();
  input.end_ns = input.begin_ns + 86400000000000LL + 600000000000LL;
  unsigned calls = 0;
  bool fail = true;
  tushare::Minutes p("fixture", [&](const auto&, auto) {
    ++calls;
    if (calls == 2 && fail)
      throw std::runtime_error("fixture interrupted");
    return response(calls == 1 ? "2023-08-25 09:01:00" : "2023-08-26 09:01:00");
  });
  p.start();
  EXPECT_THROW(data_pipeline::download_minutes(p, input, folder.path, 500), std::runtime_error);
  auto manifest = data_pipeline::inspect_minutes(folder.path);
  EXPECT_FALSE(manifest.at("complete"));
  EXPECT_EQ(manifest.at("rows"), 1);
  EXPECT_THROW(data_pipeline::read_minutes(folder.path, [](const auto&) {}), std::invalid_argument);
  fail = false;
  manifest = data_pipeline::download_minutes(p, input, folder.path, 500);
  EXPECT_TRUE(manifest.at("complete"));
  EXPECT_EQ(calls, 3);
  EXPECT_EQ(manifest.at("rows"), 2);
  EXPECT_EQ(manifest.dump().find("fixture"), std::string::npos);
  std::vector<HistoricalBar> bars;
  data_pipeline::read_minutes(folder.path, [&](const auto& bar) { bars.push_back(bar); });
  ASSERT_EQ(bars.size(), 2);
  (void)data_pipeline::download_minutes(p, input, folder.path, 500);
  EXPECT_EQ(calls, 3);
  input.interval_minutes = 5;
  EXPECT_THROW(data_pipeline::download_minutes(p, input, folder.path, 500), std::invalid_argument);
  write_file_durably(folder.path / "minutes-0.json", "{}");
  EXPECT_THROW(data_pipeline::inspect_minutes(folder.path), std::exception);
}
TEST(Tushare, CancellationDoesNotAdvanceManifestOrCallProvider) {
  Folder folder;
  std::stop_source stop;
  stop.request_stop();
  unsigned calls = 0;
  tushare::Minutes p("fixture", [&](const auto&, auto) {
    ++calls;
    return response();
  });
  p.start();
  EXPECT_THROW(data_pipeline::download_minutes(p, range(), folder.path, 60, stop.get_token()),
               std::runtime_error);
  EXPECT_EQ(calls, 0);
  EXPECT_FALSE(data_pipeline::inspect_minutes(folder.path).at("complete"));
}

TEST(Tushare, ManagedTaskPersistsDefinitionWithoutCredentialAndRestoresResult) {
  Folder root;
  const auto definition =
      data_pipeline::minute_request({{"version", 1},
                                     {"ts_code", "CU2310.SHF"},
                                     {"interval_minutes", 1},
                                     {"begin_ns", std::to_string(range().begin_ns)},
                                     {"end_ns", std::to_string(range().end_ns)},
                                     {"requests_per_minute", 60}});
  {
    tasks::Store store(root.path);
    auto task = store.submit("minutes-fixture", definition, "fixture-secret");
    EXPECT_EQ(task.kind(), research::v1::MINUTE_DOWNLOAD);
    EXPECT_EQ(store.submit("minutes-fixture", definition, "fixture-secret").submission_sequence(),
              task.submission_sequence());
    EXPECT_EQ(task.SerializeAsString().find("fixture-secret"), std::string::npos);
    auto dispatch = store.dispatch({});
    ASSERT_EQ(dispatch.launches_size(), 1);
    EXPECT_TRUE(dispatch.launches(0).minute_download());
    research::v1::TaskAttempt attempt;
    attempt.set_token(store.claim(task.id()));
    *attempt.mutable_task() = store.get(task.id());
    store.download_attempt(attempt);
    EXPECT_EQ(attempt.provider_token(), "fixture-secret");
    tushare::Minutes provider(attempt.provider_token(),
                              [](const auto&, auto) { return response(); });
    provider.start();
    const auto dir = std::filesystem::path(attempt.output_directory());
    (void)data_pipeline::download_minutes(provider, range(), dir, 60);
    research::v1::TaskFinish finish;
    finish.set_id(task.id());
    finish.set_token(attempt.token());
    *finish.mutable_minutes() = data_pipeline::minute_result(dir);
    auto completion = store.prepare_finish(finish);
    completion.verify();
    store.finish(std::move(completion));
    EXPECT_EQ(store.get(task.id()).state(), research::v1::SUCCEEDED);
    EXPECT_EQ(store.minute_result(task.id()).rows(), 1);
    for (const auto& file :
         std::filesystem::recursive_directory_iterator(root.path / "minutes-fixture")) {
      if (file.is_regular_file() && file.path().filename() != "tushare.token") {
        std::ifstream in(file.path(), std::ios::binary);
        std::string raw((std::istreambuf_iterator<char>(in)), {});
        EXPECT_EQ(raw.find("fixture-secret"), std::string::npos);
      }
    }
  }
  tasks::Store restored(root.path);
  EXPECT_EQ(restored.minute_result("minutes-fixture").rows(), 1);
  EXPECT_EQ(restored.list().tasks_size(), 1);
}
TEST(Tushare, RecoversDurablePageAfterManifestInterruptionAndProtectsExistingFiles) {
  Folder folder;
  unsigned calls = 0;
  tushare::Minutes provider("fixture", [&](const auto&, auto) {
    ++calls;
    return response();
  });
  provider.start();
  auto manifest = data_pipeline::download_minutes(provider, range(), folder.path, 60);
  manifest["pages"] = Json::array();
  manifest["rows"] = 0;
  manifest["complete"] = false;
  replace_file_durably(folder.path / "minutes.json", manifest.dump());
  auto recovered = data_pipeline::download_minutes(provider, range(), folder.path, 60);
  EXPECT_TRUE(recovered.at("complete"));
  EXPECT_EQ(recovered.at("rows"), 1);
  EXPECT_EQ(calls, 1);
  Folder occupied;
  write_file_durably(occupied.path / "user-data.txt", "preserve");
  EXPECT_THROW(data_pipeline::download_minutes(provider, range(), occupied.path, 60),
               std::invalid_argument);
  std::ifstream existing(occupied.path / "user-data.txt");
  std::string text;
  existing >> text;
  EXPECT_EQ(text, "preserve");
}
TEST(Tushare, CancellingAnInflightReadDoesNotPublishItsPage) {
  Folder folder;
  std::stop_source cancel;
  tushare::Minutes provider("fixture", [&](const auto&, auto) {
    cancel.request_stop();
    return response();
  });
  provider.start();
  EXPECT_THROW(
      data_pipeline::download_minutes(provider, range(), folder.path, 60, cancel.get_token()),
      std::runtime_error);
  EXPECT_EQ(data_pipeline::inspect_minutes(folder.path).at("pages").size(), 0);
  EXPECT_FALSE(std::filesystem::exists(folder.path / "minutes-0.json"));
}

TEST(Tushare, ContractCatalogAndWholeLifetime) {
  auto rows = tushare::contracts("fixture-token", "SHFE", "CU", {}, [](const auto& body, auto) {
    auto request = Json::parse(body);
    EXPECT_EQ(request.at("api_name"), "fut_basic");
    EXPECT_EQ(request.at("params").at("fut_type"), "1");
    EXPECT_EQ(request.at("params").at("fut_code"), "CU");
    return R"({"code":0,"data":{"fields":["ts_code","name","exchange","fut_code","list_date","delist_date","multiplier","per_unit","trade_unit","quote_unit"],"items":[["CU2310.SHF","Copper 2310","SHFE","CU","20221017","20231016",null,5.00000001,"吨","元/吨"]]}})";
  });
  ASSERT_EQ(rows.size(), 1);
  EXPECT_FALSE(rows[0].multiplier);
  EXPECT_EQ(rows[0].per_unit->str(), "5.00000001");
  EXPECT_EQ(rows[0].trade_unit, "吨");
  EXPECT_EQ(rows[0].quote_unit, "元/吨");
  auto expired = tushare::contract_range(rows[0], 1, tushare::parse_time("2026-09-28 12:00:00"));
  EXPECT_EQ(expired.begin_ns, tushare::parse_time("2022-10-17 00:00:00"));
  EXPECT_EQ(expired.end_ns, tushare::parse_time("2023-10-16 23:59:59"));
  auto active = tushare::contract_range(rows[0], 5, begin);
  EXPECT_EQ(active.end_ns, begin);
  EXPECT_THROW(tushare::contract_range(rows[0], 1, tushare::parse_time("2022-01-01 00:00:00")),
               std::invalid_argument);
  EXPECT_THROW(tushare::contract_range(rows[0], 7, begin), std::invalid_argument);
}

TEST(Tushare, RejectsIncompleteOrAmbiguousContractCatalog) {
  auto load = [](Json rows) {
    return tushare::contracts("fixture-token", "SHFE", "CU", {}, [rows](const auto&, auto) {
      return Json{{"code", 0},
                  {"data",
                   {{"fields",
                     {"ts_code", "name", "exchange", "fut_code", "list_date", "delist_date",
                      "multiplier", "per_unit", "trade_unit", "quote_unit"}},
                    {"items", rows}}}}
          .dump();
    });
  };
  Json row = {"CU2310.SHF", "Copper", "SHFE", "CU", "20221017",
              "20231016",   nullptr,  "5",    "吨", "元/吨"};
  EXPECT_THROW(load(Json::array({row, row})), std::invalid_argument);
  row[0] = "CU.SHF";
  EXPECT_THROW(load(Json::array({row})), std::invalid_argument);
  row[0] = "CU2310.SHF";
  row[4] = "20230230";
  EXPECT_THROW(load(Json::array({row})), std::invalid_argument);
  row[4] = "20241017";
  EXPECT_THROW(load(Json::array({row})), std::invalid_argument);
  row[4] = "20221017";
  for (auto invalid : {"0", "-1", "NaN", "1.123456789"}) {
    row[7] = invalid;
    EXPECT_THROW(load(Json::array({row})), std::invalid_argument);
  }
  row[7] = "5";
  row[6] = "300";
  EXPECT_EQ(load(Json::array({row}))[0].multiplier->str(), "300");
  row[8] = "bad\nunit";
  EXPECT_THROW(load(Json::array({row})), std::invalid_argument);
  row[6] = row[7] = row[8] = row[9] = nullptr;
  const auto missing = load(Json::array({row}))[0];
  EXPECT_FALSE(missing.multiplier);
  EXPECT_FALSE(missing.per_unit);
  EXPECT_FALSE(missing.trade_unit);
  EXPECT_FALSE(missing.quote_unit);
  EXPECT_TRUE(load(Json::array()).empty());
  try {
    tushare::contracts("fixture-secret", "SHFE", "CU", {},
                       [](const auto&, auto) { return R"({"code":-1,"msg":"fixture-secret"})"; });
    FAIL();
  } catch (const std::exception& e) {
    EXPECT_EQ(std::string(e.what()).find("fixture-secret"), std::string::npos);
  }
}

TEST(Tushare, DatasetViewPagesExactValuesFiltersAndRejectsCorruption) {
  Folder folder;
  tushare::Minutes provider("fixture", [](const auto&, auto) {
    return R"({"code":0,"data":{"fields":["ts_code","trade_time","open","high","low","close","vol","amount","oi"],"items":[["CU2310.SHF","2023-08-25 09:00:00","100.00000001","102","99","101","1","12345678.12345678","10"],["CU2310.SHF","2023-08-25 09:01:00","101","103","100","102","2","200","11"],["CU2310.SHF","2023-08-25 09:02:00","102","104","101","103","3","300","12"]]}})";
  });
  provider.start();
  const auto spec = range();
  data_pipeline::download_minutes(provider, spec, folder.path, 500);
  data::v1::MinuteDownload input;
  input.set_version(1);
  input.set_ts_code("CU2310.SHF");
  input.set_interval_minutes(1);
  input.set_begin_ns(spec.begin_ns);
  input.set_end_ns(spec.end_ns);
  input.set_requests_per_minute(500);
  const auto result = data_pipeline::minute_result(folder.path);
  data::v1::MinutePageQuery query;
  query.set_task_id("fixture");
  query.set_limit(2);
  auto first = data_pipeline::read_minute_page(input, result, query);
  EXPECT_EQ(first.total_rows(), 3);
  EXPECT_EQ(first.first_ns(), begin);
  EXPECT_EQ(first.last_ns(), begin + 120000000000LL);
  ASSERT_EQ(first.bars_size(), 2);
  EXPECT_EQ(first.bars(0).open().units(), Decimal::parse("100.00000001").raw());
  EXPECT_EQ(protocol::decode_minute_page(first).at("bars").at(0).at("amount"), "12345678.12345678");
  query.set_offset(2);
  EXPECT_EQ(data_pipeline::read_minute_page(input, result, query).bars_size(), 1);
  query.set_offset(3);
  EXPECT_THROW(data_pipeline::read_minute_page(input, result, query), std::invalid_argument);
  query.set_offset(0);
  query.set_begin_ns(begin + 60000000000LL);
  query.set_end_ns(begin + 60000000000LL);
  auto filtered = data_pipeline::read_minute_page(input, result, query);
  EXPECT_EQ(filtered.matched_rows(), 1);
  EXPECT_EQ(filtered.bars(0).timestamp_ns(), begin + 60000000000LL);
  query.set_begin_ns(begin + 180000000000LL);
  query.set_end_ns(begin + 240000000000LL);
  EXPECT_EQ(data_pipeline::read_minute_page(input, result, query).matched_rows(), 0);
  query.set_limit(201);
  EXPECT_THROW(data_pipeline::read_minute_page(input, result, query), std::invalid_argument);
  query.set_limit(2);
  replace_file_durably(folder.path / "minutes-0.json", "{}");
  EXPECT_THROW(data_pipeline::read_minute_page(input, result, query), std::invalid_argument);
}

TEST(Tushare, DatasetViewSkipsUnrequestedChunksAndStillVerifiesRequestedData) {
  Folder folder;
  auto spec = range();
  spec.end_ns = spec.begin_ns + 2 * 86400000000000LL;
  tushare::Minutes provider("fixture", [](const auto& body, auto) {
    const auto start = Json::parse(body).at("params").at("start_date").template get<std::string>();
    return response(start);
  });
  provider.start();
  data_pipeline::download_minutes(provider, spec, folder.path, 500);
  data::v1::MinuteDownload input;
  input.set_version(1);
  input.set_ts_code("CU2310.SHF");
  input.set_interval_minutes(1);
  input.set_begin_ns(spec.begin_ns);
  input.set_end_ns(spec.end_ns);
  input.set_requests_per_minute(500);
  const auto result = data_pipeline::minute_result(folder.path);
  data::v1::MinutePageQuery query;
  query.set_task_id("fixture");
  query.set_limit(1);
  // First and last chunks always establish actual coverage. Interior chunks are lazy.
  replace_file_durably(folder.path / "minutes-1.json", "{}");
  EXPECT_EQ(data_pipeline::read_minute_page(input, result, query).bars_size(), 1);
  query.set_offset(1);
  EXPECT_THROW(data_pipeline::read_minute_page(input, result, query), std::invalid_argument);
  query.set_offset(2);
  EXPECT_EQ(data_pipeline::read_minute_page(input, result, query).bars_size(), 1);
  query.set_include_macd(true);
  EXPECT_THROW(data_pipeline::read_minute_page(input, result, query), std::invalid_argument);
}

TEST(Tushare, CompletedDatasetPagesThroughRealTaskService) {
  using namespace std::chrono_literals;
  Folder root;
  std::filesystem::path dataset_directory;
  auto spec = range();
  spec.end_ns = begin + 39 * 60000000000LL;
  {
    tasks::Store store(root.path);
    data::v1::MinuteDownload input;
    input.set_version(1);
    input.set_ts_code("CU2310.SHF");
    input.set_interval_minutes(1);
    input.set_begin_ns(spec.begin_ns);
    input.set_end_ns(spec.end_ns);
    input.set_requests_per_minute(60);
    store.submit("viewer", input, "fixture");
    research::v1::TaskAttempt attempt;
    attempt.set_token(store.claim("viewer"));
    *attempt.mutable_task() = store.get("viewer");
    store.download_attempt(attempt);
    tushare::Minutes provider("fixture", [](const auto&, auto) {
      auto result = Json::parse(response());
      auto row = result["data"]["items"][0];
      result["data"]["items"] = Json::array();
      for (int i = 0; i < 40; ++i) {
        row[1] = tushare::format_time(begin + i * 60000000000LL);
        result["data"]["items"].push_back(row);
      }
      return result.dump();
    });
    provider.start();
    const std::filesystem::path dir(attempt.output_directory());
    dataset_directory = dir;
    data_pipeline::download_minutes(provider, spec, dir, 60);
    research::v1::TaskFinish finish;
    finish.set_id("viewer");
    finish.set_token(attempt.token());
    *finish.mutable_minutes() = data_pipeline::minute_result(dir);
    auto completion = store.prepare_finish(finish);
    completion.verify();
    store.finish(std::move(completion));
  }
#ifdef _WIN32
  const auto endpoint = "asterion.viewer." + unique_process_id();
#else
  const auto socket_dir =
      std::filesystem::path("/tmp") / ("ast-v-" + unique_process_id().substr(0, 12));
  std::filesystem::create_directory(socket_dir);
  std::filesystem::permissions(socket_dir, std::filesystem::perms::owner_all);
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code ec;
      std::filesystem::remove_all(path, ec);
    }
  } cleanup{socket_dir};
  const auto endpoint = (socket_dir / "task.sock").string();
#endif
  const auto path = root.path.u8string();
  ChildProcess service(ASTERION_TASK_SERVICE_PATH,
                       {"--directory", std::string(path.begin(), path.end()), "--endpoint",
                        endpoint, "--session", "viewer-test"});
  auto call = [&](research::v1::TaskRequest request) {
    request.set_version(1);
    request.set_service_id("viewer-test");
    request.set_correlation_id(unique_process_id());
    auto channel = ipc::Channel::connect(endpoint, 2s);
    channel.send(request.SerializeAsString(), 2s);
    research::v1::TaskResponse reply;
    if (!reply.ParseFromString(channel.receive(2s)))
      throw std::runtime_error("bad response");
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
  research::v1::TaskRequest request;
  request.mutable_minute_page()->set_task_id("viewer");
  request.mutable_minute_page()->set_limit(100);
  request.mutable_minute_page()->set_include_macd(true);
  auto result = call(request);
  ASSERT_TRUE(result.has_minute_page()) << result.DebugString();
  EXPECT_EQ(protocol::decode_minute_page(result.minute_page()).at("bars").size(), 40);
  EXPECT_FALSE(result.minute_page().bars(32).has_macd());
  EXPECT_TRUE(result.minute_page().bars(33).has_macd());
  EXPECT_DOUBLE_EQ(result.minute_page().bars(39).macd().histogram(), 0);
  {
    FileLock reader(dataset_directory, "minutes.lock", FileLock::Access::shared);
    const auto concurrent = call(request);
    ASSERT_TRUE(concurrent.has_minute_page()) << concurrent.DebugString();
    EXPECT_EQ(concurrent.minute_page().SerializeAsString(),
              result.minute_page().SerializeAsString());
  }
  {
    FileLock writer(dataset_directory, "minutes.lock");
    EXPECT_TRUE(call(request).has_error());
  }
  EXPECT_TRUE(call(request).has_minute_page());

  request.mutable_minute_page()->set_task_id("../viewer");
  EXPECT_TRUE(call(request).has_error());
  request.mutable_minute_page()->set_task_id("viewer");
  request.mutable_minute_page()->set_limit(201);
  EXPECT_TRUE(call(request).has_error());
  research::v1::TaskRequest list;
  list.mutable_list();
  const auto summary = call(list);
  ASSERT_TRUE(summary.has_tasks());
  ASSERT_EQ(summary.tasks().tasks_size(), 1);
  EXPECT_EQ(summary.tasks().tasks(0).minute_interval_minutes(), 1);
  EXPECT_FALSE(summary.tasks().tasks(0).has_minutes());
}

TEST(Tushare, ChartMacdUsesDatasetOriginAcrossPagesAndTimeFilters) {
  Folder folder;
  tushare::Minutes provider("fixture", [](const auto&, auto) {
    Json items = Json::array();
    for (int i = 0; i < 120; ++i)
      items.push_back({"CU2310.SHF", tushare::format_time(begin + i * 60000000000LL), 100 + i,
                       102 + i, 99 + i, 100 + i, 1, 100, 10});
    return Json{
        {"code", 0},
        {"data",
         {{"fields",
           {"ts_code", "trade_time", "open", "high", "low", "close", "vol", "amount", "oi"}},
          {"items", items}}}}
        .dump();
  });
  provider.start();
  auto spec = range();
  spec.end_ns = begin + 119 * 60000000000LL;
  data_pipeline::download_minutes(provider, spec, folder.path, 500);
  data::v1::MinuteDownload input;
  input.set_version(1);
  input.set_ts_code("CU2310.SHF");
  input.set_interval_minutes(1);
  input.set_begin_ns(spec.begin_ns);
  input.set_end_ns(spec.end_ns);
  input.set_requests_per_minute(500);
  const auto result = data_pipeline::minute_result(folder.path);
  data::v1::MinutePageQuery query;
  query.set_task_id("macd");
  query.set_limit(120);
  query.set_include_macd(true);
  const auto whole = data_pipeline::read_minute_page(input, result, query);
  ASSERT_EQ(whole.bars_size(), 120);
  for (int i = 0; i < 33; ++i)
    EXPECT_FALSE(whole.bars(i).has_macd());
  EXPECT_TRUE(whole.bars(33).has_macd());
  // Closed-form difference of two first-close-seeded EMAs for a unit ramp.
  const auto diff = [](int i) {
    return 7.0 + 5.5 * std::pow(11.0 / 13.0, i) - 12.5 * std::pow(25.0 / 27.0, i);
  };
  double expected_signal = 0;
  for (int i = 0; i <= 77; ++i)
    expected_signal += 0.2 * diff(i) * std::pow(0.8, 77 - i);
  EXPECT_NEAR(whole.bars(77).macd().diff(), diff(77), 1e-11);
  EXPECT_NEAR(whole.bars(77).macd().signal(), expected_signal, 1e-11);
  EXPECT_NEAR(whole.bars(77).macd().histogram(), 2 * (diff(77) - expected_signal), 1e-11);
  query.set_offset(77);
  query.set_limit(20);
  const auto page = data_pipeline::read_minute_page(input, result, query);
  EXPECT_EQ(page.bars(0).macd().SerializeAsString(), whole.bars(77).macd().SerializeAsString());
  query.set_offset(0);
  query.set_begin_ns(begin + 77 * 60000000000LL);
  const auto filtered = data_pipeline::read_minute_page(input, result, query);
  EXPECT_EQ(filtered.bars(0).macd().SerializeAsString(), whole.bars(77).macd().SerializeAsString());
  EXPECT_TRUE(protocol::decode_minute_page(filtered).at("bars").at(0).contains("macd"));
  query.set_include_macd(false);
  EXPECT_FALSE(data_pipeline::read_minute_page(input, result, query).bars(0).has_macd());
  auto invalid = filtered;
  invalid.mutable_bars(0)->mutable_macd()->set_diff(std::numeric_limits<double>::infinity());
  EXPECT_THROW(protocol::decode_minute_page(invalid), std::invalid_argument);
}

TEST(Tushare, DatasetReadersCoexistAndExcludeDownloadWrites) {
  Folder folder;
  tushare::Minutes provider("fixture", [](const auto&, auto) { return response(); });
  provider.start();
  const auto spec = range();
  data_pipeline::download_minutes(provider, spec, folder.path, 500);
  data::v1::MinuteDownload input;
  input.set_version(1);
  input.set_ts_code("CU2310.SHF");
  input.set_interval_minutes(1);
  input.set_begin_ns(spec.begin_ns);
  input.set_end_ns(spec.end_ns);
  input.set_requests_per_minute(500);
  const auto result = data_pipeline::minute_result(folder.path);
  data::v1::MinutePageQuery query;
  query.set_task_id("concurrent-read");
  query.set_limit(10);
  query.set_include_macd(true);
  unsigned consumed = 0;
  data_pipeline::read_minutes(folder.path, [&](const HistoricalBar&) {
    ++consumed;
    // Keep one reader alive while the chart and metadata readers enter.
    EXPECT_NO_THROW(data_pipeline::inspect_minutes(folder.path));
    EXPECT_NO_THROW({
      const auto page = data_pipeline::read_minute_page(input, result, query);
      EXPECT_EQ(page.bars_size(), 1);
      EXPECT_EQ(page.bars(0).close().units(), Decimal::parse("101.2").raw());
    });
    EXPECT_THROW(data_pipeline::download_minutes(provider, spec, folder.path, 500),
                 std::runtime_error);
  });
  EXPECT_EQ(consumed, 1u);
  // Reader destruction releases the lock; resuming the same immutable download is allowed.
  EXPECT_NO_THROW(data_pipeline::download_minutes(provider, spec, folder.path, 500));
}

TEST(Tushare, MacdPrefixRemainsExactAcrossMultipleSegments) {
  Folder folder;
  constexpr std::int64_t day = 86400000000000LL;
  tushare::Minutes provider("fixture", [](const auto& body, auto) {
    const auto request = Json::parse(body);
    const auto first =
        tushare::parse_time(request.at("params").at("start_date").template get<std::string>());
    const auto day_index = static_cast<int>((first - begin) / day);
    Json items = Json::array();
    for (int i = 0; i < 20; ++i) {
      const int price = 100 + day_index * 20 + i;
      items.push_back({"CU2310.SHF", tushare::format_time(first + i * 60000000000LL), price,
                       price + 2, price - 1, price, 1, 100, 10});
    }
    return Json{
        {"code", 0},
        {"data",
         {{"fields",
           {"ts_code", "trade_time", "open", "high", "low", "close", "vol", "amount", "oi"}},
          {"items", items}}}}
        .dump();
  });
  provider.start();
  auto spec = range();
  spec.end_ns = begin + 7 * day + 19 * 60000000000LL;
  data_pipeline::download_minutes(provider, spec, folder.path, 500);
  data::v1::MinuteDownload input;
  input.set_version(1);
  input.set_ts_code("CU2310.SHF");
  input.set_interval_minutes(1);
  input.set_begin_ns(spec.begin_ns);
  input.set_end_ns(spec.end_ns);
  input.set_requests_per_minute(500);
  const auto result = data_pipeline::minute_result(folder.path);
  data::v1::MinutePageQuery query;
  query.set_task_id("cache-eviction");
  query.set_offset(135);
  query.set_limit(25);
  query.set_include_macd(true);
  const auto page = data_pipeline::read_minute_page(input, result, query);
  ASSERT_EQ(page.bars_size(), 25);
  EXPECT_EQ(page.matched_rows(), 160);
  EXPECT_EQ(page.first_ns(), begin);
  EXPECT_EQ(page.last_ns(), spec.end_ns);
  for (int i = 0; i < page.bars_size(); ++i) {
    const int index = 135 + i;
    const auto diff = [](int bar) {
      return 7.0 + 5.5 * std::pow(11.0 / 13.0, bar) - 12.5 * std::pow(25.0 / 27.0, bar);
    };
    double signal = 0;
    for (int j = 0; j <= index; ++j)
      signal += 0.2 * diff(j) * std::pow(0.8, index - j);
    ASSERT_TRUE(page.bars(i).has_macd());
    EXPECT_NEAR(page.bars(i).macd().diff(), diff(index), 1e-11);
    EXPECT_NEAR(page.bars(i).macd().signal(), signal, 1e-11);
    EXPECT_NEAR(page.bars(i).macd().histogram(), 2 * (diff(index) - signal), 1e-11);
  }
  query.set_offset(0);
  query.set_begin_ns(begin + 6 * day + 15 * 60000000000LL);
  const auto filtered = data_pipeline::read_minute_page(input, result, query);
  ASSERT_EQ(filtered.bars_size(), 25);
  for (int i = 0; i < 25; ++i)
    EXPECT_EQ(filtered.bars(i).SerializeAsString(), page.bars(i).SerializeAsString());
  // The streaming prefix still verifies every consumed chunk, even when the
  // corrupted chunk is outside the requested output page.
  replace_file_durably(folder.path / "minutes-3.json", "{}");
  EXPECT_THROW(data_pipeline::read_minute_page(input, result, query), std::invalid_argument);
  query.set_include_macd(false);
  EXPECT_EQ(data_pipeline::read_minute_page(input, result, query).bars_size(), 25);
  query.set_begin_ns(begin + 6 * day + 20 * 60000000000LL);
  query.set_end_ns(begin + 6 * day + 21 * 60000000000LL);
  query.set_include_macd(true);
  // No returned bars means no MACD prefix is needed. Coverage and the queried
  // boundary chunk are still checked, but unrelated corrupted chunks are not.
  const auto empty = data_pipeline::read_minute_page(input, result, query);
  EXPECT_EQ(empty.matched_rows(), 0);
  EXPECT_EQ(empty.bars_size(), 0);
  EXPECT_EQ(empty.first_ns(), begin);
  EXPECT_EQ(empty.last_ns(), spec.end_ns);
  replace_file_durably(folder.path / "minutes-6.json", "{}");
  EXPECT_THROW(data_pipeline::read_minute_page(input, result, query), std::invalid_argument);
}
