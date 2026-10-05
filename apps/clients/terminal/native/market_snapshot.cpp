#include "market_snapshot.hpp"
#include <atomic>
#include <set>
namespace asterion::terminal {
namespace {
// Replacing a client never reuses a revision held by a Terminal window.
std::atomic<std::uint64_t> sync_revision{0};
std::string key(const Json& row) {
  return row.at("venue").get<std::string>() + "." + row.at("symbol").get<std::string>();
}
[[noreturn]] void unavailable() {
  throw Error(ErrorCode::unavailable, "market subscription revision is unavailable");
}
} // namespace
MarketUpdate::MarketUpdate(const market::v1::Snapshot& state)
    : value(protocol::decode_market(state)), base_sequence(state.base_sequence()),
      catalog_revision(state.catalog_revision()), delta(state.subscriptions_delta()),
      catalog_omitted(state.catalog_omitted()) {
  if (catalog_omitted == state.has_catalog())
    throw Error(ErrorCode::unavailable, "market catalog revision is unavailable");
  std::set<std::string> unique;
  keys.reserve(value.at("subscriptions").size());
  for (const auto& row : value.at("subscriptions")) {
    InstrumentId{row.at("venue").get<std::string>(), row.at("symbol").get<std::string>()}
        .validate();
    auto id = key(row);
    if (!unique.insert(id).second)
      unavailable();
    keys.push_back(std::move(id));
  }
}
void MarketWatchCursor::accept(const MarketUpdate& update) {
  const auto& instance = update.value.at("instance_id").get_ref<const std::string&>();
  const auto sequence = update.value.at("sequence").get<std::uint64_t>();
  if (instance.empty() || (instance_ == instance && sequence < sequence_) ||
      (update.delta &&
       (instance_ != instance || update.base_sequence != sequence_ || sequence < sequence_)) ||
      (!update.delta && update.base_sequence))
    unavailable();
  instance_ = instance;
  sequence_ = sequence;
}
void MarketSnapshot::annotate(Json& row, const std::string& id) {
  const auto found = indices_.find(id);
  const Json* previous = found == indices_.end() ? nullptr : rows_[found->second].get();
  // Compare the payload against the one owned row, without retaining another
  // complete quote tree just to exclude its local revision annotation.
  const bool same = previous && previous->size() == row.size() + 1 &&
                    std::ranges::all_of(row.items(), [&](const auto& item) {
                      const auto old = previous->find(item.key());
                      return old != previous->end() && *old == item.value();
                    });
  const auto revision = same ? previous->at("revision").get<std::uint64_t>() : ++sync_revision;
  row["revision"] = revision;
  rows_revision_ = std::max(rows_revision_, revision);
}
bool MarketSnapshot::apply(MarketUpdate update, std::uint64_t observed_generation) {
  auto& next = update.value;
  const auto& instance = next.at("instance_id").get_ref<const std::string&>();
  const auto sequence = next.at("sequence").get<std::uint64_t>();
  const bool fresh = header_.is_null() || header_.at("instance_id") != instance;
  if (instance.empty())
    unavailable();
  // Preparation may finish after another channel publishes a new process.
  if (fresh && !header_.is_null() && observed_generation != generation_)
    return false;
  if ((update.delta &&
       (fresh || update.base_sequence > header_.at("sequence").get<std::uint64_t>() ||
        update.base_sequence > sequence)) ||
      (!update.delta && update.base_sequence))
    unavailable();
  if (!fresh && header_.at("sequence").get<std::uint64_t>() > sequence)
    return false;
  const bool changed = fresh || service_catalog_revision_ != update.catalog_revision;
  if (update.catalog_omitted && changed)
    throw Error(ErrorCode::unavailable, "market catalog revision is unavailable");
  auto& subscriptions = next.at("subscriptions");
  auto& keys = update.keys;
  // Membership is checked against the latest owner state before changing rows.
  if (update.delta)
    for (const auto& id : keys)
      if (!indices_.contains(id))
        unavailable();
  std::vector<std::shared_ptr<const Json>> prepared;
  prepared.reserve(keys.size());
  for (std::size_t i = 0; i < keys.size(); ++i) {
    auto& row = subscriptions[i];
    annotate(row, keys[i]);
    const auto found = indices_.find(keys[i]);
    if (found != indices_.end() && rows_[found->second]->at("revision") == row.at("revision"))
      prepared.push_back(rows_[found->second]);
    else
      prepared.push_back(std::make_shared<const Json>(std::move(row)));
  }
  if (update.delta) {
    for (std::size_t i = 0; i < keys.size(); ++i) {
      const auto index = indices_.at(keys[i]);
      auto& current = rows_[index];
      if (current == prepared[i])
        continue;
      current = std::move(prepared[i]);
    }
  } else {
    if (fresh || keys != keys_ || !set_revision_) {
      keys_ = std::move(keys);
      set_revision_ = ++sync_revision;
      indices_.clear();
      for (std::size_t i = 0; i < keys_.size(); ++i)
        indices_.emplace(keys_[i], i);
    }
    rows_ = std::move(prepared);
  }
  if (changed || !catalog_revision_) {
    catalog_revision_ = ++sync_revision;
    auto catalog = std::move(next.at("catalog"));
    catalog["revision"] = catalog_revision_;
    catalog_ = std::make_shared<const Json>(std::move(catalog));
  }
  service_catalog_revision_ = update.catalog_revision;
  next.erase("catalog");
  next.erase("subscriptions");
  header_ = std::move(next);
  header_["subscription_set"] = set_revision_;
  if (fresh)
    ++generation_;
  return true;
}
MarketProjection MarketSnapshot::capture() const {
  return {header_, catalog_, rows_};
}
Json MarketProjection::render(std::optional<MarketCursor> held) const {
  if (header.is_null())
    return nullptr;
  auto out = header;
  auto& subscriptions = out["subscriptions"] = Json::array();
  const bool delta = held && held->subscriptions == header.at("subscription_set");
  for (const auto& row : rows)
    if (!delta || row->at("revision").get<std::uint64_t>() > held->rows)
      subscriptions.push_back(*row);
  if (delta)
    out["delta"] = true;
  if (held && held->catalog == catalog->at("revision")) {
    auto& summary = out["catalog"] = Json::object();
    for (const auto& [name, value] : catalog->items())
      if (name != "contracts")
        summary[name] = value;
    summary["contracts"] = Json::array();
    summary["omitted"] = true;
  } else
    out["catalog"] = *catalog;
  return out;
}

} // namespace asterion::terminal
