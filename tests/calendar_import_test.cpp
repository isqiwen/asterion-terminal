#include "calendar.hpp"
#include "csv_settlement_calendar.hpp"
#include "engine.hpp"
#include "task_store.hpp"
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <fstream>
#include <gtest/gtest.h>
using namespace asterion;
namespace fs = std::filesystem;
namespace {
Decimal d(const char* v) {
  return Decimal::parse(v);
}
Instrument instrument() {
  return {{"SHFE", "rb2610"}, AssetClass::futures, "CNY", d("1"), d("1"), d("10")};
}
const std::string header = "trading_day,session_begin,session_end,settlement_"
                           "price,schedule_source,settlement_source\n";
std::string csv() {
  return header + "2026-09-25,2026-09-24T21:00:00+08:00,2026-09-24T21:00:03+08:"
                  "00,105,\"时段,公告\",\"结算\"\"资料\"\n"
                  "2026-09-25,2026-09-25T09:00:00+08:00,2026-09-25T09:00:04+08:"
                  "00,105,\"时段,公告\",\"结算\"\"资料\"\n"
                  "2026-09-28,2026-09-28T09:00:00+08:00,2026-09-28T09:00:04+08:"
                  "00,110,fixture,settlement\n";
}
std::string replace(std::string text, const std::string& a, const std::string& b) {
  const auto i = text.find(a);
  if (i == std::string::npos)
    throw std::logic_error("missing fixture text");
  text.replace(i, a.size(), b);
  return text;
}
struct Directory {
  fs::path path = fs::temp_directory_path() / ("asterion-calendar-" + unique_process_id());
  Directory() { fs::create_directory(path); }
  ~Directory() {
    std::error_code e;
    fs::remove_all(path, e);
  }
};
void write(const fs::path& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary);
  out << bytes;
}
data::v1::CsvImport spec(const fs::path& path, const std::string& bytes) {
  write(path, bytes);
  data::v1::CsvImport input;
  input.set_version(1);
  const auto name = path.u8string();
  input.set_source_path(std::string(name.begin(), name.end()));
  input.set_source_sha256(sha256_bytes(bytes));
  *input.mutable_contract() = protocol::encode_contract({{"venue", "SHFE"},
                                                         {"symbol", "rb2610"},
                                                         {"product", "rb"},
                                                         {"delivery_month", "2026-10"},
                                                         {"currency", "CNY"},
                                                         {"price_increment", "1"},
                                                         {"quantity_increment", "1"},
                                                         {"multiplier", "10"}});
  return input;
}
} // namespace
TEST(CalendarCsv, ExplicitOffsetsQuotedSourcesAndLifecycle) {
  CsvSettlementCalendar provider(instrument(), csv());
  EXPECT_THROW(provider.read(), std::logic_error);
  provider.start();
  EXPECT_THROW(provider.start(), std::logic_error);
  const auto days = provider.read();
  ASSERT_EQ(days.size(), 2U);
  ASSERT_EQ(days[0].schedule.sessions().size(), 2U);
  EXPECT_EQ(days[0].schedule.sessions()[0].begin_ns, 1790254800000000000LL);
  EXPECT_EQ(days[0].schedule.sessions()[1].begin_ns, 1790298000000000000LL);
  EXPECT_EQ(days[0].schedule_source, "时段,公告");
  EXPECT_EQ(days[0].settlement_source, "结算\"资料");
  EXPECT_EQ(days[1].settlement_price, d("110"));
  std::stop_source stop;
  stop.request_stop();
  EXPECT_THROW(provider.read(stop.get_token()), std::runtime_error);
  provider.stop();
  EXPECT_THROW(provider.read(), std::logic_error);
  provider.start();
  EXPECT_EQ(provider.read().size(), 2U);
}
TEST(CalendarCsv, RejectsAmbiguousConflictingOrReorderedRowsWithoutRepair) {
  const std::vector<std::string> invalid = {
      replace(csv(), "21:00:00+08:00", "21:00:00"),
      replace(csv(), "21:00:00+08:00", "21:00:00+14:01"),
      replace(csv(), "2026-09-24T21:00:00", "2263-09-24T21:00:00"),
      replace(csv(), "09:00:04+08:00,105", "09:00:04+08:00,106"),
      replace(csv(), ",105,", ",105.0,"),
      replace(csv(), "09:00:04+08:00,110", "08:00:04+08:00,110"),
      replace(csv(), "2026-09-28,", "2026-09-24,"),
      csv() + "\n",
      csv() + "bad,row\n",
      replace(csv(), "fixture,settlement", "fixture,\"unterminated")};
  for (const auto& text : invalid) {
    CsvSettlementCalendar p(instrument(), text);
    p.start();
    EXPECT_THROW(p.read(), std::exception);
  }
  CsvSettlementCalendar empty(instrument(), header);
  empty.start();
  EXPECT_THROW(empty.read(), std::invalid_argument);
  EXPECT_THROW((CsvSettlementCalendar(instrument(), std::string(1024 * 1024 + 1, 'x'))),
               std::invalid_argument);
}
TEST(CalendarPublication, StableNormalizedRevisionDistinctSourceAndRecomputedVerification) {
  Directory root;
  const auto first = data_pipeline::capture_calendar_csv(spec(root.path / "one.csv", csv()));
  auto alternate = replace(csv(), "2026-09-24T21:00:00+08:00", "2026-09-24T13:00:00Z");
  for (std::size_t i = 0; (i = alternate.find('\n', i)) != std::string::npos; i += 2)
    alternate.insert(i, "\r");
  const auto second = data_pipeline::capture_calendar_csv(spec(root.path / "two.csv", alternate));
  const auto a = data_pipeline::import_calendar_snapshot(first),
             b = data_pipeline::import_calendar_snapshot(second);
  EXPECT_EQ(a.calendar().revision(), b.calendar().revision());
  EXPECT_NE(a.source_sha256(), b.source_sha256());
  EXPECT_NE(a.id(), b.id());
  EXPECT_EQ(protocol::encode_calendar_publication(protocol::decode_calendar_publication(a))
                .SerializeAsString(),
            a.SerializeAsString());
  EXPECT_NO_THROW(data_pipeline::verify_calendar_result(first, a));
  auto wrong = a;
  wrong.mutable_calendar()->mutable_days(0)->mutable_settlement_price()->set_units(d("106").raw());
  *wrong.mutable_calendar() =
      protocol::make_settlement_calendar(wrong.calendar().contract(), wrong.calendar().days());
  wrong.set_id(protocol::calendar_publication_id(wrong));
  EXPECT_THROW(data_pipeline::verify_calendar_result(first, wrong), std::invalid_argument);
  auto changed = first;
  changed.set_contents(alternate);
  EXPECT_THROW(data_pipeline::import_calendar_snapshot(changed), std::invalid_argument);
}
TEST(CalendarPublication, DurableInspectionSurvivesSourceDeletionAndRejectsOverwrite) {
  Directory root;
  const auto source = root.path / "source.csv", output = root.path / "published";
  fs::create_directory(output);
  const auto input = spec(source, csv());
  const auto a =
      data_pipeline::import_calendar_snapshot(data_pipeline::capture_calendar_csv(input));
  EXPECT_TRUE(data_pipeline::publish_calendar(a, output));
  EXPECT_FALSE(data_pipeline::publish_calendar(a, output));
  fs::remove(source);
  EXPECT_EQ(data_pipeline::read_calendar(output).SerializeAsString(), a.SerializeAsString());
  auto other = a;
  other.set_source_name("other.csv");
  other.set_id(protocol::calendar_publication_id(other));
  EXPECT_THROW(data_pipeline::publish_calendar(other, output), std::invalid_argument);
  write(output / "pending.tmp", "incomplete");
  EXPECT_THROW(data_pipeline::read_calendar(output), std::invalid_argument);
  EXPECT_EQ(fs::file_size(output / "pending.tmp"), 10U);
  fs::remove(output / "pending.tmp");
  auto stored = Json::parse(std::ifstream(output / "00000000.json"));
  stored["publication"]["calendar"]["days"][0]["settlement_price"] = "999";
  write(output / "00000000.json", stored.dump());
  EXPECT_THROW(data_pipeline::read_calendar(output), std::invalid_argument);
}
TEST(CalendarPublication, IndependentCliPublishesAndInspectsWithOriginalFilesRemoved) {
  Directory root;
  const auto source = root.path / fs::path(u8"结算.csv"), input = root.path / "input.pb",
             output = root.path / "published";
  fs::create_directory(output);
  write(input, spec(source, csv()).SerializeAsString());
  auto run = [&](const std::vector<std::string>& args) {
    ChildProcess p(ASTERION_PIPELINE_PATH, args);
    EXPECT_TRUE(p.wait(std::chrono::seconds(10)));
    return p.exit_code();
  };
  EXPECT_EQ(
      run({"--settlement-calendar", "--input", input.string(), "--directory", output.string()}), 0);
  const auto before = data_pipeline::read_calendar(output);
  fs::remove(source);
  fs::remove(input);
  EXPECT_EQ(run({"--settlement-calendar", "--inspect", "--directory", output.string()}), 0);
  EXPECT_EQ(data_pipeline::read_calendar(output).SerializeAsString(), before.SerializeAsString());
  EXPECT_NE(run({"--inspect", "--directory", output.string()}), 0);
}

TEST(CalendarPublication, PublishedDaysDriveTheSameMultidayBacktestContract) {
  Directory root;
  const auto publication = data_pipeline::import_calendar_snapshot(
      data_pipeline::capture_calendar_csv(spec(root.path / "calendar.csv", csv())));
  research::v1::BacktestInput input;
  input.set_version(5);
  *input.mutable_days() = publication.calendar().days();
  *input.mutable_calendar_publication() = publication;
  Json ticks = Json::array();
  for (const auto& day : input.days())
    for (const auto& session : day.sessions())
      for (auto ns = session.begin_ns(); ns < session.end_ns(); ns += 1000000000LL)
        ticks.push_back(
            {{"timestamp_ns", std::to_string(ns)}, {"price", "100"}, {"quantity", "1"}});
  *input.mutable_paper() = protocol::encode_input(
      {{"version", 1},
       {"type", "historical_paper"},
       {"contract", protocol::decode_contract(publication.calendar().contract())},
       {"ticks", ticks},
       {"deposit", "10000"},
       {"costs",
        {{"margin_per_lot", "100"},
         {"open_fee", "2"},
         {"close_today_fee", "3"},
         {"close_yesterday_fee", "4"},
         {"margin_rate", "0"},
         {"open_fee_rate", "0"},
         {"close_today_fee_rate", "0"},
         {"close_yesterday_fee_rate", "0"}}},
       {"risk",
        {{"max_order_quantity", "100"},
         {"max_gross_quantity", "100"},
         {"max_working_orders", std::uint64_t{100}}}}});
  input.set_dataset_revision(protocol::dataset_revision(input.paper()));
  input.mutable_sma()->set_fast(1);
  input.mutable_sma()->set_slow(3);
  input.mutable_sma()->mutable_quantity()->set_units(d("1").raw());
  const auto roundtrip = protocol::encode_backtest(protocol::decode_backtest(input));
  EXPECT_EQ(roundtrip.SerializeAsString(), input.SerializeAsString());
  auto altered = input;
  altered.mutable_days(0)->mutable_settlement_price()->set_units(d("106").raw());
  EXPECT_THROW(backtest::run(altered), std::invalid_argument);
  altered = input;
  altered.mutable_paper()->mutable_contract()->set_symbol("rb2611");
  altered.mutable_paper()->mutable_contract()->set_delivery_month("2026-11");
  altered.set_dataset_revision(protocol::dataset_revision(altered.paper()));
  EXPECT_THROW(backtest::run(altered), std::invalid_argument);
  altered = input;
  altered.mutable_calendar_publication()->set_source_name("forged.csv");
  EXPECT_THROW(backtest::run(altered), std::invalid_argument);
  altered = input;
  altered.set_version(4);
  EXPECT_THROW(backtest::run(altered), std::invalid_argument);
  const auto result = backtest::run(input);
  const auto task_path = root.path / "backtest-tasks";
  fs::create_directory(task_path);
  {
    tasks::Store store(task_path);
    store.submit("published-calendar", input);
    const auto token = store.claim("published-calendar");
    store.finish("published-calendar", token, result);
  }
  {
    tasks::Store restored(task_path);
    const auto task = restored.get("published-calendar");
    EXPECT_EQ(task.input().calendar_publication().SerializeAsString(),
              publication.SerializeAsString());
    EXPECT_EQ(restored.result("published-calendar").SerializeAsString(),
              result.SerializeAsString());
  }
  ASSERT_EQ(result.settlements_size(), 2);
  EXPECT_EQ(result.settlements(0).price().units(), d("105").raw());
  EXPECT_EQ(result.settlements(1).price().units(), d("110").raw());
  EXPECT_EQ(result.account().equity().units(), d("10000").raw());
  EXPECT_EQ(protocol::decode_backtest(input).at("days"),
            protocol::decode_calendar(publication.calendar()).at("days"));
}

TEST(CalendarPublication, PreservesSignedAndFractionalPricesWithoutImposingBacktestRules) {
  Directory root;
  auto bytes = replace(csv(), "09:00:04+08:00,110", "09:00:04+08:00,-0.5");
  const auto publication = data_pipeline::import_calendar_snapshot(
      data_pipeline::capture_calendar_csv(spec(root.path / "signed.csv", bytes)));
  EXPECT_EQ(publication.calendar().days(1).settlement_price().units(), d("-0.5").raw());
  EXPECT_NO_THROW(protocol::decode_calendar_publication(publication));
}

TEST(CalendarTasks, DurableCancellationRetryFencingAndSourceValidation) {
  Directory root;
  const auto input = data_pipeline::capture_calendar_csv(spec(root.path / "source.csv", csv()));
  fs::remove(root.path / "source.csv");
  const auto output = data_pipeline::import_calendar_snapshot(input);
  const auto directory = root.path / "tasks";
  fs::create_directory(directory);
  {
    tasks::Store store(directory);
    const auto submitted = store.submit("calendar", input);
    EXPECT_EQ(submitted.kind(), research::v1::CALENDAR_IMPORT);
    EXPECT_EQ(store.submit("calendar", input).submission_sequence(),
              submitted.submission_sequence());
    EXPECT_FALSE(store.list().tasks(0).has_calendar());
    auto changed = input;
    changed.set_source_name("other.csv");
    EXPECT_THROW(store.submit("calendar", changed), std::invalid_argument);
    const auto token = store.claim("calendar");
    auto wrong = output;
    wrong.set_source_name("other.csv");
    wrong.set_id(protocol::calendar_publication_id(wrong));
    EXPECT_THROW(store.finish("calendar", token, wrong), std::invalid_argument);
    store.cancel("calendar");
    store.finish("calendar", token, output);
    EXPECT_EQ(store.get("calendar").state(), research::v1::CANCELLED);
    store.retry("calendar");
    const auto next = store.claim("calendar");
    EXPECT_THROW(store.finish("calendar", token, output), std::invalid_argument);
    store.finish("calendar", next, output);
    EXPECT_EQ(store.get("calendar").completed(), input.contents().size());
    EXPECT_THROW(store.publication("calendar"), std::invalid_argument);
  }
  tasks::Store restored(directory);
  EXPECT_EQ(restored.calendar_publication("calendar").SerializeAsString(),
            output.SerializeAsString());
  research::v1::TaskResponse response;
  *response.mutable_calendar_publication() = restored.calendar_publication("calendar");
  *response.mutable_result_task() = restored.get("calendar");
  const auto envelope = protocol::decode_task_result(response, "calendar");
  EXPECT_EQ(envelope.at("kind"), "calendar_import");
  EXPECT_EQ(envelope.at("result").at("id"), output.id());
  response.mutable_result_task()->mutable_calendar()->set_source_name("different.csv");
  EXPECT_THROW(protocol::decode_task_result(response, "calendar"), std::invalid_argument);
}
TEST(CalendarTasks, RestartInterruptsClaimAndRequiresNewAttempt) {
  Directory root;
  const auto input = data_pipeline::capture_calendar_csv(spec(root.path / "source.csv", csv()));
  const auto directory = root.path / "tasks";
  fs::create_directory(directory);
  std::string token;
  {
    tasks::Store store(directory);
    store.submit("interrupted", input);
    token = store.claim("interrupted");
  }
  tasks::Store restored(directory);
  EXPECT_EQ(restored.get("interrupted").state(), research::v1::INTERRUPTED);
  EXPECT_THROW(
      restored.finish("interrupted", token, data_pipeline::import_calendar_snapshot(input)),
      std::invalid_argument);
  restored.retry("interrupted");
  const auto next = restored.claim("interrupted");
  EXPECT_NE(token, next);
  restored.finish("interrupted", next, data_pipeline::import_calendar_snapshot(input));
  EXPECT_EQ(restored.get("interrupted").attempt(), 2U);
}
