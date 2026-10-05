#include "bar_parquet.hpp"
#include <asterion/kernel/process/child.hpp>
#include <fstream>
#include <gtest/gtest.h>
using namespace asterion;
namespace {
struct Folder {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() / ("asterion-parquet-" + unique_process_id());
  Folder() { std::filesystem::create_directory(path); }
  ~Folder() { std::filesystem::remove_all(path); }
};
Decimal d(const char* text) {
  return Decimal::parse(text);
}
} // namespace
TEST(BarParquet, MinuteBarsRoundTripExactDecimalsAndOrder) {
  Folder folder;
  const std::vector<HistoricalBar> bars{
      {120000000000, d("100.00000001"), d("102"), d("99"), d("101"), d("1"),
       d("92233720368.54775807"), d("10"), "2026-09-25"},
      {60000000000, d("0.00000001"), d("0.00000001"), d("0.00000001"), d("0.00000001"), d("0"),
       d("0"), d("0"), ""}};
  const auto file = folder.path / "minutes.parquet";
  parquet::write_minute_bars(file, bars);
  EXPECT_FALSE(std::filesystem::exists(folder.path / "minutes.parquet.tmp"));
  const auto read = parquet::read_minute_bars(file);
  ASSERT_EQ(read.size(), 2U);
  EXPECT_EQ(read[0], bars[1]) << "rows are stored in timestamp order";
  EXPECT_EQ(read[1], bars[0]);
  EXPECT_EQ(read[1].amount.str(), "92233720368.54775807");
}
TEST(BarParquet, DailyBarsKeepMissingReferencePricesAndRejectForeignSchemas) {
  Folder folder;
  using namespace std::chrono;
  const std::vector<HistoricalDailyBar> bars{{year(2024) / January / 2, d("100"), d("101"), d("99"),
                                              d("100.5"), d("20"), d("2000.5"), d("300"),
                                              std::nullopt, d("99.5"), std::nullopt}};
  const auto file = folder.path / "daily.parquet";
  parquet::write_daily_bars(file, bars);
  const auto read = parquet::read_daily_bars(file);
  ASSERT_EQ(read.size(), 1U);
  EXPECT_EQ(read[0], bars[0]);
  EXPECT_FALSE(read[0].previous_close);
  EXPECT_THROW(parquet::read_minute_bars(file), std::invalid_argument);
  std::ofstream(folder.path / "broken.parquet") << "not parquet";
  EXPECT_ANY_THROW(parquet::read_daily_bars(folder.path / "broken.parquet"));
  EXPECT_THROW(parquet::write_daily_bars("relative.parquet", bars), std::invalid_argument);
}
TEST(BarParquet, MaximumStoragePagesRoundTripWithinTheEngineBudget) {
  Folder folder;
  // A minute segment spans at most 31 days. Unique minute-aligned timestamps
  // bound each day to 1440 rows, even though the provider page cap is higher.
  constexpr std::size_t minute_count = 31 * 24 * 60;
  std::vector<HistoricalBar> minutes;
  minutes.reserve(minute_count);
  for (std::size_t i = 0; i < minute_count; ++i)
    minutes.push_back({static_cast<std::int64_t>(i + 1) * 60000000000, d("100"), d("102"), d("99"),
                       d("101"), d("20"), d("2020"), d("300"), ""});
  const auto minute_file = folder.path / "minutes.parquet";
  parquet::write_minute_bars(minute_file, minutes);
  EXPECT_EQ(parquet::read_minute_bars(minute_file), minutes);

  using namespace std::chrono;
  std::vector<HistoricalDailyBar> daily;
  for (int i = 0; i < 366; ++i)
    daily.push_back({year_month_day(sys_days(year(2024) / January / 1) + days(i)), d("100"),
                     d("102"), d("99"), d("101"), d("20"), d("2020"), d("300"), d("100"), d("100"),
                     d("101")});
  const auto daily_file = folder.path / "daily.parquet";
  parquet::write_daily_bars(daily_file, daily);
  EXPECT_EQ(parquet::read_daily_bars(daily_file), daily);
}
