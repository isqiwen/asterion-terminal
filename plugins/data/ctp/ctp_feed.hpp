#pragma once
#include <asterion/kernel/thread_pool.hpp>
#include <asterion/domain/live_market.hpp>
#include <filesystem>
#include <map>
#include <memory>
namespace asterion::ctp {
struct Configuration {
  std::string front, broker, user, password;
};
void validate_instruments(const std::vector<InstrumentId>&);
std::optional<Decimal> price(double value);
std::optional<Decimal> open_interest_change(double current, double previous);
MarketDepthLevel depth_level(double price, int quantity);
std::int64_t source_time(const std::string& day, const std::string& time, int millisecond);
// CTP reports CZCE AveragePrice per unit and other exchanges as turnover /
// volume, which includes the contract multiplier.
std::optional<Decimal> average_price(double value, const std::string& venue,
                                     std::optional<int> multiplier);
class Feed final {
public:
  // The shared SDK executor has exactly one worker and outlives this adapter.
  Feed(ThreadPool& sdk_owner, const std::filesystem::path& library,
       const std::filesystem::path& flow, std::size_t event_capacity = 4096);
  ~Feed();
  void start();
  void stop() noexcept;
  void connect(Configuration config, const std::vector<InstrumentId>& instruments);
  // All public state operations belong to one owner. SDK callbacks only enqueue
  // owned provider observations; poll applies at most 1,024 per turn.
  void poll();
  void subscribe(const std::vector<InstrumentId>& instruments);
  // Contract multipliers from the trading catalog; required to normalize
  // non-CZCE average prices. Unknown contracts report no average price.
  void set_multipliers(std::map<InstrumentId, int> multipliers);
  LiveMarketSnapshot snapshot(std::optional<std::uint64_t> after = {},
                              std::span<const InstrumentId> forced = {}) const;
  MarketPhase phase() const;
  MarketEventBatch events_after(const std::string& stream_id, std::uint64_t cursor,
                                std::size_t limit) const;
  void disconnect();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace asterion::ctp
