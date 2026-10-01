#pragma once
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
class Feed final : public LiveMarketDataPort {
public:
  Feed(const std::filesystem::path& library, const std::filesystem::path& flow,
       std::size_t event_capacity = 4096);
  ~Feed() override;
  PluginDescriptor descriptor() const override;
  void start() override;
  void stop() noexcept override;
  void connect(Configuration config, const std::vector<InstrumentId>& instruments);
  void subscribe(const std::vector<InstrumentId>& instruments) override;
  // Contract multipliers from the trading catalog; required to normalize
  // non-CZCE average prices. Unknown contracts report no average price.
  void set_multipliers(std::map<InstrumentId, int> multipliers);
  LiveMarketSnapshot snapshot() const override;
  MarketEventBatch events_after(const std::string& stream_id, std::uint64_t cursor,
                                std::size_t limit) const override;
  void disconnect() override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace asterion::ctp
