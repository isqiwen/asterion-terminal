#include "daily_factor_source.hpp"
#include "history_daily.hpp"
#include <asterion/protocol/data.hpp>
#include <asterion/protocol/factor.hpp>
#include <stdexcept>
namespace asterion::data {
factor::v1::DailyFactorDataset daily_factor_dataset(const data::v1::HistoryRecord& source) {
  protocol::validate_message(source);
  const auto& result = source.daily_result();
  protocol::validate_message(result);
  if (source.version() != 1 || !source.has_daily() || !source.has_daily_result())
    throw std::invalid_argument("daily factor requires a published daily dataset");
  const auto range = history_files::daily_range(source.daily());
  const auto directory =
      std::filesystem::path(std::u8string(result.directory().begin(), result.directory().end()));
  // Read metadata and every bar under the same shared dataset lock. Checking
  // metadata first and reopening the files would allow a changed source between reads.
  const auto stored = history_files::read_daily(directory);
  const auto& meta = stored.info;
  if (!meta.complete || meta.range.instrument != range.instrument ||
      meta.range.begin != range.begin || meta.range.end != range.end || result.version() != 2 ||
      result.manifest_sha256() != meta.manifest_sha256 || result.rows() != meta.rows ||
      result.pages() != meta.pages)
    throw std::invalid_argument("daily dataset result does not match request");
  factor::v1::DailyFactorDataset dataset;
  dataset.set_version(2);
  auto* evidence = dataset.mutable_history_evidence();
  evidence->set_dataset_id(meta.manifest_sha256);
  evidence->set_acquired_at_ns(meta.acquired_at_ns);
  evidence->set_source_availability(data::v1::SOURCE_AVAILABILITY_UNKNOWN);
  dataset.set_source_dataset_id(result.manifest_sha256());
  dataset.set_source(source.daily().source());
  dataset.set_contract_id(source.daily().contract_id());
  dataset.set_manifest_sha256(meta.manifest_sha256);
  for (const auto& bar : stored.bars)
    *dataset.add_bars() = protocol::encode_daily_bar(bar);
  (void)protocol::daily_factor_bars(dataset);
  return dataset;
}
} // namespace asterion::data
