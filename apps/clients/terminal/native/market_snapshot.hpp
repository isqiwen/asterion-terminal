#pragma once
#include <asterion/protocol/market.hpp>
#include <map>
#include <memory>
#include <unordered_map>
namespace asterion::terminal {
struct MarketCursor {
  std::uint64_t subscriptions = 0, rows = 0, catalog = 0;
};
// Prepared on a read worker without access to the current published state.
// Carries the UI payload and the wire evidence needed by its single owner.
struct MarketUpdate {
  explicit MarketUpdate(const market::v1::Snapshot& state);
  Json value;
  std::vector<std::string> keys;
  std::uint64_t base_sequence, catalog_revision;
  bool delta, catalog_omitted;
};
// Captured by the owner; workers render it without accessing mutable state.
// Only the small header and a vector of references are copied at capture time.
struct MarketProjection {
  Json header = nullptr;
  std::shared_ptr<const Json> catalog;
  std::vector<std::shared_ptr<const Json>> rows;
  Json render(std::optional<MarketCursor> held = {}) const;
};
// Owned Terminal view. Callers serialize access; incoming watch deltas update
// only changed rows. Command replies can advance this view between watch frames.
class MarketSnapshot {
public:
  bool apply(MarketUpdate update, std::uint64_t observed_generation);
  std::uint64_t generation() const { return generation_; }
  const Json& metadata() const { return header_; }
  MarketProjection capture() const;
  MarketCursor cursor() const { return {set_revision_, rows_revision_, catalog_revision_}; }

private:
  Json header_ = nullptr;
  std::shared_ptr<const Json> catalog_;
  std::vector<std::shared_ptr<const Json>> rows_;
  std::uint64_t generation_ = 0;
  std::unordered_map<std::string, std::size_t> indices_;
  std::vector<std::string> keys_;
  std::uint64_t set_revision_ = 0, catalog_revision_ = 0, service_catalog_revision_ = 0;
  std::uint64_t rows_revision_ = 0;
  void annotate(Json& row, const std::string& key);
};
// A watch has its own baseline even when a newer command reply is published.
// Validate every frame before discarding an older one from the shared view.
class MarketWatchCursor {
public:
  void accept(const MarketUpdate& update);

private:
  std::string instance_;
  std::uint64_t sequence_ = 0;
};
} // namespace asterion::terminal
