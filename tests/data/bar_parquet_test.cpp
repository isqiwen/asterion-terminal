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
TEST(BarParquet, AFileThatIsNotParquetIsRejected) {
  Folder folder;
  std::ofstream(folder.path / "broken.parquet") << "not parquet";
  EXPECT_ANY_THROW(parquet::read_minute_bars(folder.path / "broken.parquet"));
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
}
