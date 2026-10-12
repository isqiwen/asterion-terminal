#include "data_store.hpp"
#include "table_series.hpp"
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/protocol/data.hpp>
#include <asterion/protocol/data_client.hpp>
#include <asterion/protocol/factor.hpp>
#include <asterion/protocol/task.hpp>
#include <asterion/v1/task.pb.h>
#include <gtest/gtest.h>
#include <thread>
using namespace asterion;
using history_files::TableSeries;
using history_files::TableStore;
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
const TableSeries daily{"futures.daily", 1, "fixture.daily.v1"},
    calendar{"futures.calendar", 1, "fixture.calendar.v1"},
    contracts{"futures.contracts", 1, "fixture.contracts.v1"};
const std::string artifact(64, 'a');
std::string keep(TableStore& store, const TableSeries& series, const std::string& period,
                 const std::vector<TableSourceRow>& rows, std::int64_t acquired) {
  const auto partition = table_partition(history_table(series.table), "SHFE", period, rows);
  return store.record(partition, series, store.write(partition), artifact, acquired)
      .revision.snapshot;
}
// A day of one contract: what it closed and settled at, and how much was held.
TableSourceRow traded(const char* contract, const char* close, const std::string& settlement,
                      const std::string& open_interest) {
  TableSourceRow cells(history_table("futures.daily").columns.size());
  cells[0] = contract;
  if (close)
    cells[4] = close;
  cells[5] = settlement;
  cells[10] = open_interest;
  return cells;
}
// The first week of March 2026 and the Monday after it.
const char* const days[] = {"2026-03-02", "2026-03-03", "2026-03-04",
                            "2026-03-05", "2026-03-06", "2026-03-09"};
void keep_calendar(TableStore& store) {
  std::vector<TableSourceRow> year;
  for (const auto* day : days)
    year.push_back({day, "1", std::nullopt});
  year.push_back({"2026-03-07", "0", std::nullopt});
  year.push_back({"2026-03-08", "0", std::nullopt});
  (void)keep(store, calendar, "2026", year, 900);
}
// Rebar's May month loses its open interest to October on the third day.
std::string keep_rebar(TableStore& store, int except = -1) {
  const char* near[] = {"3100", "3110", "3120", "3130", nullptr, nullptr};
  const char* held[][2] = {{"500", "100"}, {"400", "300"}, {"200", "600"},
                           {"100", "700"}, {"50", "800"},  {"10", "900"}};
  std::string snapshot;
  for (int d = 0; d < 6; ++d) {
    if (d == except)
      continue;
    const auto settled = std::to_string(3100 + 10 * d), far = std::to_string(3200 + 10 * d);
    snapshot = keep(store, daily, days[d],
                    {traded("SHFE/rb/2026-05", near[d], settled, held[d][0]),
                     traded("SHFE/rb/2026-10", far.c_str(), far, held[d][1]),
                     traded("SHFE/cu/2026-04", nullptr, "70000", "0")},
                    1000 + d);
  }
  return snapshot;
}
data::v1::TableSeriesRequest request(const std::string& snapshot,
                                     std::vector<std::string> instruments,
                                     const char* begin = "2026-03-02",
                                     const char* end = "2026-03-09") {
  data::v1::TableSeriesRequest result;
  result.set_snapshot(snapshot);
  result.set_daily_source(daily.source);
  result.set_calendar_source(calendar.source);
  result.set_begin_day(begin);
  result.set_end_day(end);
  for (auto& instrument : instruments)
    result.add_instruments(std::move(instrument));
  return result;
}
std::vector<std::pair<std::string, std::string>> closes(const data::v1::TableMonth& month) {
  std::vector<std::pair<std::string, std::string>> result;
  for (const auto& close : month.closes())
    result.emplace_back(close.trading_day(), Decimal::from_raw(close.close().units()).str());
  return result;
}
template <class Call> std::string refusal(Call&& call) {
  try {
    call();
  } catch (const std::exception& error) {
    return error.what();
  }
  return {};
}
} // namespace
TEST(TableSeries, AProductIsReadThroughItsDominantMonthsUnderOneSnapshot) {
  Folder folder("ast-ts-");
  TableStore store(folder.path / "tables");
  keep_calendar(store);
  const auto snapshot = keep_rebar(store);
  const auto group = data::table_series(store, request(snapshot, {"SHFE/rb", "SHFE/rb/2026-05"}));
  ASSERT_EQ(group.series_size(), 2);
  const auto& product = group.series(0);
  EXPECT_EQ(product.instrument(), "SHFE/rb");
  EXPECT_EQ(product.acquired_at_ns(), 1005) << "the most recent of the days read";
  // The first day only says what was held; May leads until October holds more.
  ASSERT_EQ(product.months_size(), 2);
  EXPECT_EQ(product.months(0).contract(), "SHFE/rb/2026-05");
  EXPECT_EQ(closes(product.months(0)), (std::vector<std::pair<std::string, std::string>>{
                                           {"2026-03-03", "3110"}, {"2026-03-04", "3120"}}));
  EXPECT_EQ(product.months(1).contract(), "SHFE/rb/2026-10");
  EXPECT_EQ(closes(product.months(1)),
            (std::vector<std::pair<std::string, std::string>>{
                {"2026-03-05", "3230"}, {"2026-03-06", "3240"}, {"2026-03-09", "3250"}}));
  const auto& schedule = product.schedule();
  ASSERT_EQ(schedule.rolls_size(), 2);
  EXPECT_EQ(schedule.rolls(0).trading_day(), "2026-03-03");
  // October over May at the settlements of the day before the roll: 3220 / 3120.
  EXPECT_EQ(Decimal::from_raw(schedule.rolls(0).factor().units()).str(), "1.03205128");
  EXPECT_EQ(schedule.rolls(1).trading_day(), "2026-03-05");
  EXPECT_EQ(Decimal::from_raw(schedule.rolls(1).factor().units()).str(), "1");
  ASSERT_EQ(schedule.terms_size(), 2) << "October has no later month to compare with";
  EXPECT_EQ(schedule.terms(1).trading_day(), "2026-03-04");
  EXPECT_EQ(Decimal::from_raw(schedule.terms(1).near().units()).str(), "3110");
  EXPECT_EQ(Decimal::from_raw(schedule.terms(1).far().units()).str(), "3210");
  EXPECT_EQ(schedule.terms(1).far_month(), "2026-10");
  // A contract is its own closes; the days it did not trade are not there.
  const auto& contract = group.series(1);
  ASSERT_EQ(contract.months_size(), 1);
  EXPECT_FALSE(contract.has_schedule());
  EXPECT_EQ(closes(contract.months(0)).size(), 4U);
  EXPECT_EQ(closes(contract.months(0)).back(),
            (std::pair<std::string, std::string>{"2026-03-05", "3130"}));

  // What a factor reads of them: the product at the level of its last month.
  factor::v1::FactorInput input;
  input.set_version(8);
  input.set_factor(protocol::v1::PRICE_MOMENTUM);
  *input.add_series()->mutable_table() = product;
  const auto observed = protocol::factor_observations(input);
  ASSERT_EQ(observed.closes.at(0).size(), 5U);
  EXPECT_EQ(observed.closes[0][0]->str(), "3209.6794808") << "3110 x 1.03205128";
  EXPECT_EQ(observed.closes[0][2]->str(), "3230");
  input.set_factor(protocol::v1::TERM_STRUCTURE);
  const auto carried = protocol::factor_observations(input);
  ASSERT_EQ(carried.terms.at(0).size(), 2U) << "only the days that have a term point";
  // (3110 / 3210 - 1) x 12 / 5 months apart.
  EXPECT_EQ(carried.terms[0][1]->str(), "-0.07476636");

  // A later correction is another snapshot; the first still holds what it held.
  const auto corrected = keep(store, daily, "2026-03-03",
                              {traded("SHFE/rb/2026-05", "3111", "3110", "400"),
                               traded("SHFE/rb/2026-10", "3210", "3210", "300")},
                              2000);
  const auto again = data::table_series(store, request(snapshot, {"SHFE/rb", "SHFE/rb/2026-05"}));
  EXPECT_EQ(again.SerializeAsString(), group.SerializeAsString());
  const auto revised = data::table_series(store, request(corrected, {"SHFE/rb"})).series(0);
  EXPECT_EQ(closes(revised.months(0)).front().second, "3111");
  EXPECT_EQ(revised.acquired_at_ns(), 2000);
  EXPECT_NE(protocol::table_series_revision(revised), protocol::table_series_revision(product));
}
TEST(TableSeries, AMonthStillDominantOnItsLastDayHandsOverOnTheNext) {
  Folder folder("ast-ts-");
  TableStore store(folder.path / "tables");
  keep_calendar(store);
  // April holds the most until it is gone; nothing of it is left on the fourth day.
  std::string snapshot;
  for (int d = 0; d < 6; ++d) {
    const auto far = std::to_string(72000 + 100 * d);
    std::vector<TableSourceRow> rows{traded("SHFE/cu/2026-06", far.c_str(), far, "100")};
    if (d < 3) {
      const auto near = std::to_string(70000 + 100 * d);
      rows.push_back(traded("SHFE/cu/2026-04", near.c_str(), near, "500"));
    }
    snapshot = keep(store, daily, days[d], rows, 1000 + d);
  }
  const auto series = data::table_series(store, request(snapshot, {"SHFE/cu"})).series(0);
  ASSERT_EQ(series.months_size(), 2);
  EXPECT_EQ(closes(series.months(0)).back(),
            (std::pair<std::string, std::string>{"2026-03-04", "70200"}));
  EXPECT_EQ(series.schedule().rolls(1).trading_day(), "2026-03-05");
  // June over April at the settlements of April's last day: 72200 / 70200.
  EXPECT_EQ(Decimal::from_raw(series.schedule().rolls(0).factor().units()).str(), "1.02849003");
  EXPECT_EQ(closes(series.months(1)).front(),
            (std::pair<std::string, std::string>{"2026-03-05", "72300"}));
}
TEST(TableSeries, AHoleInTheRangeIsRefusedNotReadAround) {
  Folder folder("ast-ts-");
  TableStore store(folder.path / "tables");
  keep_calendar(store);
  const auto snapshot = keep_rebar(store, 2);
  const auto ask = [&](data::v1::TableSeriesRequest request) {
    return refusal([&] { (void)data::table_series(store, request); });
  };
  EXPECT_EQ(ask(request(snapshot, {"SHFE/rb"})),
            "futures.daily is missing trading days of the range: SHFE 2026-03-04");
  // Without the missing day the range is whole, and each instrument answers for itself.
  EXPECT_EQ(ask(request(snapshot, {"SHFE/rb"}, "2026-03-05")), "");
  EXPECT_EQ(ask(request(snapshot, {"SHFE/rb", "SHFE/ag"}, "2026-03-05")),
            "the instrument has no closes in the range: SHFE/ag");
  EXPECT_EQ(ask(request(snapshot, {"SHFE/cu/2026-04"}, "2026-03-05")),
            "the instrument has no closes in the range: SHFE/cu/2026-04");
  EXPECT_EQ(ask(request(snapshot, {"SHFE/rb"}, "2026-03-09")),
            "the dominant series cannot be read: SHFE/rb: a dominant series requires at least "
            "two trading days");
  EXPECT_EQ(ask(request(snapshot, {"SHFE/rb"}, "2026-12-30", "2027-01-05")),
            "the calendar of the range is not stored: futures.calendar SHFE 2027");
  EXPECT_EQ(ask(request(std::string(64, 'b'), {"SHFE/rb"})),
            "the table snapshot is not in this store");
  auto twice = request(snapshot, {"SHFE/rb", "SHFE/rb"});
  EXPECT_EQ(ask(twice), "invalid table series request");
  EXPECT_EQ(ask(request(snapshot, {"SHFE/rb/2026"})), "invalid table instrument");
  // One request asks for at most a hundred instruments, and a factor compares
  // at most as many series however its request is put together.
  std::vector<std::string> many;
  for (int month = 0; month < 101; ++month) {
    char name[24];
    std::snprintf(name, sizeof name, "SHFE/rb/%04d-%02d", 2030 + month / 12, month % 12 + 1);
    many.emplace_back(name);
  }
  EXPECT_EQ(ask(request(snapshot, many)), "invalid table series request");
  many.pop_back();
  EXPECT_EQ(ask(request(snapshot, many, "2026-03-05")),
            "the instrument has no closes in the range: SHFE/rb/2030-01");
  const auto sixty = [&](std::size_t first) {
    return Json{{"tables",
                 {{"snapshot", snapshot},
                  {"daily_source", daily.source},
                  {"calendar_source", calendar.source},
                  {"begin_day", "2026-03-05"},
                  {"end_day", "2026-03-09"},
                  {"instruments", std::vector<std::string>(
                                      many.begin() + static_cast<std::ptrdiff_t>(first),
                                      many.begin() + static_cast<std::ptrdiff_t>(first) + 60)}}}};
  };
  EXPECT_EQ(refusal([&] {
              (void)protocol::encode_factor_request({{"series", {sixty(0), sixty(40)}},
                                                     {"factor", "momentum"},
                                                     {"lookbacks", {5}},
                                                     {"horizon", 1},
                                                     {"evaluation", {{"mode", "full_sample"}}}});
            }),
            "factor analysis studies one series or 3..100 contracts");
}
TEST(TableSeries, ProductsAreThoseOfTheContractListsOfASnapshot) {
  Folder folder("ast-ts-");
  TableStore store(folder.path / "tables");
  const auto listing = [](const char* contract, const char* listed, const char* delisted) {
    TableSourceRow cells(history_table("futures.contracts").columns.size());
    cells[0] = contract;
    cells[3] = listed;
    cells[4] = delisted;
    return cells;
  };
  const auto snapshot = keep(store, contracts, "all",
                             {listing("SHFE/rb/2026-10", "2025-10-16", "2026-10-15"),
                              listing("SHFE/rb/2026-05", "2025-05-16", "2026-05-15"),
                              listing("SHFE/cu/2026-04", "2025-04-16", "2026-04-15")},
                             1000);
  data::v1::TableProductsQuery query;
  query.set_snapshot(snapshot);
  query.set_contracts_source(contracts.source);
  EXPECT_EQ(protocol::decode_table_products(data::table_products(store, query)),
            (Json::array({{{"exchange", "SHFE"},
                           {"product", "cu"},
                           {"contracts", 1},
                           {"first_listed", "2025-04-16"},
                           {"last_delisted", "2026-04-15"}},
                          {{"exchange", "SHFE"},
                           {"product", "rb"},
                           {"contracts", 2},
                           {"first_listed", "2025-05-16"},
                           {"last_delisted", "2026-10-15"}}})));
}
// The real data service, task service and factor worker over a store that
// holds nine weeks of three products.
TEST(TableSeries, AFactorIsEvaluatedAcrossProductsOfTheTable) {
  using namespace std::chrono_literals;
  Folder warehouse("ast-tf-"), tasks("ast-tt-"), sockets("ast-tk-");
  // The data service's own directory layout, then the tables as an update leaves them.
  {
    data::Store layout(warehouse.path, "table-data", "table-tasks");
  }
  std::string snapshot;
  std::vector<std::string> open;
  {
    TableStore store(warehouse.path / "tables");
    std::vector<TableSourceRow> year;
    for (auto day = std::chrono::sys_days(parse_trading_date("2026-03-02")); open.size() < 45;
         day += std::chrono::days(1)) {
      const auto text = format_trading_date(std::chrono::year_month_day(day));
      const bool weekday = std::chrono::weekday(day).iso_encoding() < 6;
      year.push_back({text, weekday ? "1" : "0", std::nullopt});
      if (weekday)
        open.push_back(text);
    }
    (void)keep(store, calendar, "2026", year, 900);
    for (int d = 0; d < 45; ++d) {
      const auto price = [&](int value) { return std::to_string(value); };
      const auto rebar = price(3000 + 7 * d + d * d % 13),
                 copper = price(70000 - 11 * d + d * 37 % 29 * 10),
                 aluminium = price(20000 + d * 17 % 23 * 15 - 3 * d);
      // Rebar rolls from May to October in the fourth week.
      const auto near = std::to_string(d < 18 ? 500 : 100),
                 far = std::to_string(d < 18 ? 100 : 500);
      snapshot = keep(store, daily, open[static_cast<std::size_t>(d)],
                      {traded("SHFE/rb/2026-05", rebar.c_str(), rebar, near),
                       traded("SHFE/rb/2026-10", rebar.c_str(), rebar, far),
                       traded("SHFE/cu/2026-06", copper.c_str(), copper, "300"),
                       traded("SHFE/al/2026-06", aluminium.c_str(), aluminium, "300")},
                      1000 + d);
    }
  }
  const auto data_endpoint = (sockets.path / "data.sock").string(),
             data_workers = (sockets.path / "data.workers").string(),
             task_endpoint = (sockets.path / "task.sock").string();
  ChildProcess data_process(ASTERION_DATA_SERVICE_PATH,
                            {"--directory", warehouse.path.string(), "--session", "table-data",
                             "--task-instance", "table-tasks", "--endpoint", data_endpoint,
                             "--worker-endpoint", data_workers});
  ChildProcess task_process(ASTERION_TASK_SERVICE_PATH,
                            {"--directory", tasks.path.string(), "--session", "table-tasks",
                             "--endpoint", task_endpoint, "--data-instance", "table-data",
                             "--data-endpoint", data_workers});
  const auto call = [&](task::v1::TaskRequest request) {
    request.set_version(1);
    request.set_service_id("table-tasks");
    request.set_correlation_id(unique_process_id());
    auto channel = ipc::Channel::connect(task_endpoint, 2s);
    channel.send(request.SerializeAsString(), 2s);
    task::v1::TaskResponse reply;
    if (!reply.ParseFromString(channel.receive(10s)))
      throw std::runtime_error("bad response");
    return reply;
  };
  for (const auto deadline = std::chrono::steady_clock::now() + 10s;;) {
    try {
      task::v1::TaskRequest ping;
      ping.mutable_heartbeat();
      data::v1::DataRequest data_ping;
      data_ping.mutable_heartbeat();
      if (call(ping).health().initialized() &&
          protocol::DataClient(data_endpoint, "table-data").call(data_ping).health().initialized())
        break;
    } catch (const std::exception&) {
      ASSERT_LT(std::chrono::steady_clock::now(), deadline);
    }
    std::this_thread::sleep_for(20ms);
  }
  task::v1::TaskRequest submit;
  submit.mutable_submit()->set_id("across");
  auto* factor = submit.mutable_submit()->mutable_factor_request();
  *factor->add_series()->mutable_tables() = request(snapshot, {"SHFE/rb", "SHFE/cu", "SHFE/al"},
                                                    open.front().c_str(), open.back().c_str());
  factor->set_factor(protocol::v1::PRICE_MOMENTUM);
  factor->add_lookbacks(5);
  factor->set_horizon(1);
  factor->set_full_sample(true);
  const auto submitted = call(submit);
  ASSERT_TRUE(submitted.has_task()) << submitted.DebugString();
  EXPECT_EQ(submitted.task().kind(), task::v1::FACTOR);
  EXPECT_EQ(submitted.task().instrument(), "SHFE/rb + SHFE/cu + SHFE/al");
  EXPECT_EQ(submitted.task().source_name(), snapshot) << "one snapshot, named once";
  // Each series begins on the second day: 44 shared observations.
  EXPECT_EQ(submitted.task().total(), 44U);
  // The worker is handed what to read, not the rows, and reads the same from Data.
  ChildProcess worker(ASTERION_FACTOR_PATH, {"--endpoint", task_endpoint, "--session",
                                             "table-tasks", "--task", "across"});
  ASSERT_TRUE(worker.wait(20s));
  ASSERT_EQ(worker.exit_code(), 0);
  task::v1::TaskRequest read;
  read.mutable_result()->set_id("across");
  const auto result = call(read);
  ASSERT_EQ(result.result_task().state(), task::v1::SUCCEEDED) << result.DebugString();
  EXPECT_EQ(result.factor().input_count(), 44U);
  EXPECT_EQ(result.factor().cross_sections_size(), 38) << "44 less the window and the label";
  const auto& input = result.result_task().factor();
  ASSERT_EQ(input.series_size(), 3);
  EXPECT_EQ(input.series(0).table().months_size(), 2) << "rebar rolled";
  EXPECT_EQ(input.series(0).table().schedule().rolls(1).trading_day(), open[19]);
  const auto decoded = protocol::decode_task_result(result, "across");
  EXPECT_EQ(decoded.at("experiment").at("series").at(1).at("kind"), "table");
  EXPECT_EQ(decoded.at("experiment").at("series").at(1).at("data").at("instrument"), "SHFE/cu");
  EXPECT_EQ(decoded.at("result").at("cross_sections").at(0).at("observed"), open[6])
      << "observations are shown by trading day";
}
