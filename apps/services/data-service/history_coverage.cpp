#include "history_coverage.hpp"
#include "history_daily.hpp"
#include "history_minutes.hpp"
#include <map>
#include <set>
namespace asterion::data {
namespace {
std::filesystem::path directory(const std::string& text) {
  return std::filesystem::path(std::u8string(text.begin(), text.end()));
}
} // namespace
data::v1::HistoryCoverages history_coverage(const history_files::Archive& archive,
                                            const data::v1::HistoryFilter& filter) {
  struct Version {
    HistoryDataset info;
    std::set<std::string> days;
  };
  std::map<std::string, std::vector<Version>> minutes, daily;
  const HistoryStorePort& store = archive;
  for (const auto& item :
       store.datasets({filter.venue(), filter.product(), filter.contract_id(), filter.source()})) {
    const auto record = archive.get(item.id);
    Version version{item, {}};
    if (record.has_minute_result()) {
      for (auto& day : history_files::minute_trading_days(
               directory(record.minute_result().directory()), item.id))
        version.days.insert(std::move(day));
      minutes[item.contract.key()].push_back(std::move(version));
    } else {
      for (auto& day :
           history_files::daily_trading_days(directory(record.daily_result().directory()), item.id))
        version.days.insert(std::move(day));
      daily[item.contract.key()].push_back(std::move(version));
    }
  }
  data::v1::HistoryCoverages result;
  const auto append = [&](const std::string& contract, const Version* minute, const Version* day) {
    if (result.items_size() >= 10000)
      throw std::invalid_argument("historical archive query exceeds limit; narrow filters");
    auto* row = result.add_items();
    row->set_contract_id(contract);
    if (minute) {
      row->set_minute_dataset_id(minute->info.id);
      row->set_minute_source(minute->info.source);
      row->set_interval_minutes(minute->info.interval_minutes);
      row->set_minute_days(static_cast<unsigned>(minute->days.size()));
      if (!minute->days.empty()) {
        row->set_minute_first(*minute->days.begin());
        row->set_minute_last(*minute->days.rbegin());
      }
    }
    if (day) {
      row->set_daily_dataset_id(day->info.id);
      row->set_daily_source(day->info.source);
      row->set_daily_days(static_cast<unsigned>(day->days.size()));
      if (!day->days.empty()) {
        row->set_daily_first(*day->days.begin());
        row->set_daily_last(*day->days.rbegin());
      }
    }
    // This is a comparison of two exact versions, not proof of intraday completeness.
    if (!minute || !day || minute->days.empty())
      return;
    unsigned uncovered = 0;
    for (const auto& date : day->days)
      if (date >= row->minute_first() && date <= row->minute_last() &&
          !minute->days.contains(date)) {
        if (++uncovered <= 50)
          row->add_uncovered_days(date);
      }
    row->set_uncovered(uncovered);
  };
  for (const auto& [contract, versions] : minutes)
    for (const auto& version : versions) {
      const auto found = daily.find(contract);
      if (found == daily.end())
        append(contract, &version, nullptr);
      else
        for (const auto& day : found->second)
          append(contract, &version, &day);
    }
  for (const auto& [contract, versions] : daily)
    if (!minutes.contains(contract))
      for (const auto& version : versions)
        append(contract, nullptr, &version);
  return result;
}
} // namespace asterion::data
