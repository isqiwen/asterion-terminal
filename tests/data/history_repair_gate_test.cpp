#include "data/history_fixture.hpp"
#include "history_update.hpp"
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/data.hpp>
#include <gtest/gtest.h>
using namespace asterion;
namespace {
struct Folder {
  std::filesystem::path path = std::filesystem::temp_directory_path() / unique_process_id();
  Folder() { std::filesystem::create_directory(path); }
  ~Folder() { std::filesystem::remove_all(path); }
};
data::v1::HistoryUpdateQuery repair(const Json& seed) {
  return protocol::encode_history_update_query({{"dataset_id", seed.at("source_dataset_ids").at(0)},
                                                {"daily", seed.at("daily")},
                                                {"mode", "repair"},
                                                {"end_day", ""},
                                                {"requests_per_minute", 60}});
}
std::map<std::string, std::string> immutable_files(const std::filesystem::path& root) {
  std::map<std::string, std::string> result;
  for (const auto& file : std::filesystem::recursive_directory_iterator(root))
    if (file.is_regular_file() &&
        (file.path().extension() == ".parquet" || file.path().filename() == "minutes.json"))
      result[file.path().string()] = sha256_file(file.path());
  return result;
}
Json calendar(history_files::TableStore& tables, Json daily,
              const std::vector<TableSourceRow>& rows) {
  const auto partition = table_partition(history_table("futures.calendar"), "SHFE", "2026", rows);
  (void)tables.record(partition,
                      {"futures.calendar", 1, daily.at("calendar_source").get<std::string>()},
                      tables.write(partition), std::string(64, 'c'), 1790000000000000002);
  daily["snapshot"] = tables.head();
  return daily;
}
} // namespace
TEST(HistoryRepairGate,
     ReviewedClosureConflictsBlockBeforeMissingDayPlanningAndPreserveOriginalData) {
  Folder folder;
  const auto seeded =
      test::seed_history(folder.path, {100, 101, 102, 103}, "blocked", {"2026-09-23", "2026-09-25"},
                         {"2026-09-23", "2026-09-24", "2026-09-25"});
  history_files::Archive archive(folder.path / "history");
  history_files::TableStore tables(folder.path / "tables");
  const auto request = repair(seeded);
  const auto before = archive.get(request.dataset_id()).SerializeAsString();
  const auto head = tables.head();
  const auto hashes = immutable_files(folder.path);
  const auto result = data::history_update_plan(archive, tables, request,
                                                parse_shanghai_time("2026-10-09 00:00:00"));
  ASSERT_EQ(result.conflicts_size(), 1);
  const auto& conflict = result.conflicts(0);
  EXPECT_EQ(conflict.day(), "2026-09-25");
  EXPECT_TRUE(conflict.observed_minutes());
  EXPECT_TRUE(conflict.calendar_open());
  EXPECT_TRUE(conflict.daily_auction());
  EXPECT_EQ(conflict.closure().version(), 1);
  EXPECT_EQ(conflict.closure().published_day(), "2026-09-21");
  EXPECT_NE(conflict.closure().source_url().find("833503"), std::string::npos);
  EXPECT_TRUE(result.missing_days().empty());
  EXPECT_EQ(result.session_rules_version(), 1);
  const auto json = protocol::decode_history_update_plan(result);
  EXPECT_EQ(json.at("conflicts").at(0).at("day"), "2026-09-25");
  EXPECT_EQ(json.at("query").at("daily"), seeded.at("daily"));
  EXPECT_EQ(archive.get(request.dataset_id()).SerializeAsString(), before);
  EXPECT_EQ(tables.head(), head);
  EXPECT_EQ(immutable_files(folder.path), hashes);
  EXPECT_EQ(data::history_update_plan(archive, tables, request,
                                      parse_shanghai_time("2026-10-09 00:00:00"))
                .id(),
            result.id());
}
TEST(HistoryRepairGate, CalendarOnlyConflictIsExplainedEvenWhenDailyReferenceIsMissing) {
  Folder folder;
  auto seeded =
      test::seed_history(folder.path, {100, 101}, "calendar-only", {"2026-09-23", "2026-09-28"},
                         {"2026-09-23", "2026-09-24", "2026-09-28"});
  history_files::Archive archive(folder.path / "history");
  history_files::TableStore tables(folder.path / "tables");
  seeded["daily"] = calendar(tables, seeded.at("daily"),
                             {{"2026-09-23", "1", std::nullopt},
                              {"2026-09-24", "1", std::nullopt},
                              {"2026-09-25", "1", std::nullopt},
                              {"2026-09-26", "0", std::nullopt},
                              {"2026-09-27", "0", std::nullopt},
                              {"2026-09-28", "1", std::nullopt}});
  const auto result = data::history_update_plan(archive, tables, repair(seeded),
                                                parse_shanghai_time("2026-10-09 00:00:00"));
  ASSERT_EQ(result.conflicts_size(), 1);
  EXPECT_EQ(result.conflicts(0).day(), "2026-09-25");
  EXPECT_TRUE(result.conflicts(0).calendar_open());
  EXPECT_FALSE(result.conflicts(0).observed_minutes());
  EXPECT_FALSE(result.conflicts(0).daily_auction());
}
TEST(HistoryRepairGate, ClosedCalendarWithoutContradictoryFactsDoesNotBlockLegitimateOtherGaps) {
  Folder folder;
  auto seeded =
      test::seed_history(folder.path, {100, 101}, "closed-no-conflict",
                         {"2026-09-23", "2026-09-28"}, {"2026-09-23", "2026-09-24", "2026-09-28"});
  history_files::Archive archive(folder.path / "history");
  history_files::TableStore tables(folder.path / "tables");
  seeded["daily"] = calendar(tables, seeded.at("daily"),
                             {{"2026-09-23", "1", std::nullopt},
                              {"2026-09-24", "1", std::nullopt},
                              {"2026-09-25", "0", std::nullopt},
                              {"2026-09-26", "0", std::nullopt},
                              {"2026-09-27", "0", std::nullopt},
                              {"2026-09-28", "1", std::nullopt}});
  const auto result = data::history_update_plan(archive, tables, repair(seeded),
                                                parse_shanghai_time("2026-10-09 00:00:00"));
  EXPECT_TRUE(result.conflicts().empty());
  ASSERT_EQ(result.missing_days_size(), 1);
  EXPECT_EQ(result.missing_days(0), "2026-09-24")
      << "a cancelled evening does not close that civil daytime";
  EXPECT_EQ(protocol::decode_history_update_plan(result).at("conflicts"), Json::array());
}
TEST(HistoryRepairGate,
     DatesOutsideReviewedScopeRetainExistingWholeDayPlanningWithoutSessionClaims) {
  Folder folder;
  const auto seeded =
      test::seed_history(folder.path, {100, 101}, "unreviewed", {"2026-09-07", "2026-09-09"},
                         {"2026-09-07", "2026-09-08", "2026-09-09"});
  history_files::Archive archive(folder.path / "history");
  history_files::TableStore tables(folder.path / "tables");
  const auto before = immutable_files(folder.path);
  const auto result = data::history_update_plan(archive, tables, repair(seeded),
                                                parse_shanghai_time("2026-10-09 00:00:00"));
  EXPECT_TRUE(result.conflicts().empty());
  ASSERT_EQ(result.missing_days_size(), 1);
  EXPECT_EQ(result.missing_days(0), "2026-09-08");
  EXPECT_EQ(immutable_files(folder.path), before);
}
