#include "session_calendar.hpp"
#include <chrono>
#include <fstream>
#include <gtest/gtest.h>
using namespace asterion;
using namespace asterion::sessions;
namespace {
SessionCatalog catalog() {
  std::ifstream file(std::string(ASTERION_SOURCE_DIR) + "/config/futures-sessions.json");
  return SessionCatalog::parse(Json::parse(file));
}
// Beijing wall clock (UTC+8) as UTC nanoseconds.
std::int64_t beijing(int y, unsigned m, unsigned d, int hour, int minute) {
  using namespace std::chrono;
  const auto local = sys_days{year{y} / month{m} / day{d}} + hours{hour} + minutes{minute};
  return duration_cast<nanoseconds>((local - hours{8}).time_since_epoch()).count();
}
} // namespace
TEST(Sessions, CatalogCoversExchangesWithSourceEvidence) {
  const auto sessions = catalog();
  EXPECT_NE(sessions.provenance().find("reviewed 2026-09-28"), std::string::npos);
  EXPECT_TRUE(sessions.find("SHFE", "rb").night.has_value());
  EXPECT_FALSE(sessions.find("GFEX", "si").night.has_value());
  EXPECT_FALSE(sessions.find("CFFEX", "IF").night.has_value());
  EXPECT_THROW(sessions.find("SHFE", "zz"), std::invalid_argument);
}
TEST(Sessions, MondayNightSessionRunsFridayEvening) {
  const auto days = generate(catalog().find("SHFE", "rb"), {"2026-09-28"}, "2026-09-25");
  ASSERT_EQ(days.size(), 1U);
  ASSERT_TRUE(days[0].night);
  ASSERT_EQ(days[0].sessions.size(), 4U);
  EXPECT_EQ(days[0].sessions[0].begin_ns, beijing(2026, 9, 25, 21, 0));
  EXPECT_EQ(days[0].sessions[0].end_ns, beijing(2026, 9, 25, 23, 0));
  EXPECT_EQ(days[0].sessions[1].begin_ns, beijing(2026, 9, 28, 9, 0));
  EXPECT_EQ(days[0].sessions[3].end_ns, beijing(2026, 9, 28, 15, 0));
}
TEST(Sessions, OvernightSessionEndsOnTheNextCalendarDay) {
  const auto days = generate(catalog().find("SHFE", "au"), {"2026-09-28"}, "2026-09-25");
  EXPECT_EQ(days[0].sessions[0].end_ns, beijing(2026, 9, 26, 2, 30))
      << "Friday night ends Saturday";
}
TEST(Sessions, NoNightSessionBeforeAStatutoryHoliday) {
  // 2026-09-30 is followed by a holiday that includes weekdays.
  const auto days =
      generate(catalog().find("DCE", "m"), {"2026-09-30", "2026-10-09"}, "2026-09-29");
  ASSERT_EQ(days.size(), 2U);
  EXPECT_TRUE(days[0].night);
  EXPECT_FALSE(days[1].night) << "the last working day before the holiday has no night session";
  EXPECT_EQ(days[1].sessions.size(), 3U);
}
TEST(Sessions, FinancialFuturesAndUnanchoredDays) {
  const auto index = generate(catalog().find("CFFEX", "IF"), {"2026-09-28"}, "2026-09-25");
  ASSERT_EQ(index[0].sessions.size(), 2U);
  EXPECT_EQ(index[0].sessions[0].begin_ns, beijing(2026, 9, 28, 9, 30));
  const auto bonds = generate(catalog().find("CFFEX", "T"), {"2026-09-28"});
  EXPECT_EQ(bonds[0].sessions.back().end_ns, beijing(2026, 9, 28, 15, 15));
  const auto first = generate(catalog().find("SHFE", "rb"), {"2026-09-28"});
  EXPECT_FALSE(first[0].night) << "without the previous trading day the night is undecidable";
  EXPECT_THROW(generate(catalog().find("SHFE", "rb"), {"2026-09-27"}), std::invalid_argument);
  EXPECT_THROW(generate(catalog().find("SHFE", "rb"), {"2026-09-29", "2026-09-28"}),
               std::invalid_argument);
}
