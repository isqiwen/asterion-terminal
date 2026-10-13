#include "dominant_plan.hpp"
#include <optional>
#include <set>
#include <stdexcept>
namespace asterion::data {
DominantPlan plan_dominant(std::span<const DominantMonth> months, SeriesUse use) {
  std::set<std::string> all;
  for (std::size_t i = 0; i < months.size(); ++i) {
    if (i && months[i].delivery_month <= months[i - 1].delivery_month)
      throw std::invalid_argument("a dominant series lists each month once");
    for (const auto& [day, price] : months[i].settlement)
      all.insert(day);
  }
  DominantPlan plan;
  plan.days.assign(all.begin(), all.end());
  const auto& days = plan.days;
  if (days.size() < 2)
    throw std::invalid_argument("a dominant series requires at least two trading days");
  // New over old settlement on the day before each roll; 1 for the first segment.
  std::vector<Decimal> ratios;
  for (std::size_t d = 1; d < days.size(); ++d) {
    const auto &day = days[d], &previous = days[d - 1];
    const auto first = plan.segments.empty() ? 0 : plan.segments.back().month;
    std::optional<std::size_t> chosen, ending;
    for (std::size_t m = first; m < months.size(); ++m) {
      const auto interest = months[m].open_interest.find(previous);
      if (!months[m].settlement.contains(day) || interest == months[m].open_interest.end())
        continue;
      // Held past its last day, a month could not be left. The series may end in it.
      if (use == SeriesUse::traded && day == months[m].settlement.rbegin()->first &&
          d + 1 < days.size()) {
        ending = m;
        continue;
      }
      if (!chosen || interest->second > months[*chosen].open_interest.at(previous))
        chosen = m;
    }
    if (!chosen)
      throw std::invalid_argument(
          "no month contract of the series has data on " + day + " and open interest on " +
          previous + (ending ? ", apart from " + months[*ending].name + " on its last day" : ""));
    // The term structure the day begins with: the dominant month against the
    // later month most held the day before, where both settled then.
    const auto& dominant = months[*chosen];
    std::optional<std::size_t> later;
    for (std::size_t m = *chosen + 1; m < months.size(); ++m) {
      const auto interest = months[m].open_interest.find(previous);
      if (!months[m].settlement.contains(previous) || interest == months[m].open_interest.end())
        continue;
      if (!later || interest->second > months[*later].open_interest.at(previous))
        later = m;
    }
    if (later && dominant.settlement.contains(previous)) {
      auto& term = plan.terms.emplace_back();
      term.set_trading_day(day);
      term.mutable_near()->set_units(dominant.settlement.at(previous).raw());
      term.mutable_far()->set_units(months[*later].settlement.at(previous).raw());
      term.set_far_month(months[*later].delivery_month);
    }
    if (!plan.segments.empty() && *chosen == plan.segments.back().month)
      continue;
    Decimal ratio = Decimal::parse("1");
    if (!plan.segments.empty()) {
      const auto& old = months[plan.segments.back().month];
      // What is held of it is closed on its next bar: it needs one.
      if (use == SeriesUse::traded && old.last_bar < day)
        throw std::invalid_argument(
            "the outgoing month has no bars from the roll day on: " + old.name + " from " + day);
      if (!old.settlement.contains(previous) || !dominant.settlement.contains(previous))
        throw std::invalid_argument("the roll lacks both settlement prices on the previous day: " +
                                    old.name + " on " + day);
      ratio =
          divide(dominant.settlement.at(previous), old.settlement.at(previous), Rounding::half_up);
      if (ratio <= Decimal{})
        throw std::invalid_argument("invalid roll adjustment ratio on " + day);
    }
    plan.segments.push_back({*chosen, day, previous, Decimal::parse("1")});
    ratios.push_back(ratio);
  }
  // Back-adjustment: the last month keeps its prices; each earlier month is
  // scaled by the ratios of every roll after it.
  for (std::size_t i = plan.segments.size() - 1; i > 0; --i) {
    plan.segments[i - 1].factor = multiply(plan.segments[i].factor, ratios[i], Rounding::half_up);
    if (plan.segments[i - 1].factor <= Decimal{})
      throw std::invalid_argument("invalid roll adjustment ratio on " + plan.segments[i - 1].begin);
  }
  return plan;
}
} // namespace asterion::data
