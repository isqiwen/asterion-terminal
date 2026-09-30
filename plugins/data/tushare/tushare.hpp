#pragma once
#include <asterion/domain/historical_bars.hpp>
#include <asterion/domain/daily_bars.hpp>
#include <asterion/foundation/serialization.hpp>
#include <functional>
#include <string_view>
namespace asterion::tushare {
enum class AccessFailure {
  invalid_credential = 2,
  permission = 3,
  rate_limit = 4,
  network = 5,
  failed = 6
};
class RequestError : public std::runtime_error {
public:
  RequestError(AccessFailure reason, const std::string& diagnostic)
      : std::runtime_error(diagnostic), reason(reason) {}
  AccessFailure reason;
};
// The only production transport posts to https://api.tushare.pro, verifies the
// server certificate and never follows redirects. Injection is for tests.
using Post = std::function<std::string(const std::string&, std::stop_token)>;
Post https_transport();
std::int64_t parse_time(std::string_view); // explicit Asia/Shanghai wall time
std::string format_time(std::int64_t);
InstrumentId instrument(std::string_view ts_code);
std::string code(const HistoryIdentity&);
void validate(const HistoricalBarRange&);
struct FuturesListing {
  std::string ts_code, name, exchange, product, list_date, delist_date;
  HistoryIdentity identity = {};
  // Preserve provider semantics; multiplier and per-unit size are not interchangeable.
  std::optional<Decimal> multiplier = {}, per_unit = {};
  std::optional<std::string> trade_unit = {}, quote_unit = {};
};
std::vector<FuturesListing> contracts(const std::string& token, const std::string& exchange,
                                      const std::string& product, std::stop_token = {},
                                      Post = https_transport());
HistoricalBarRange contract_range(const FuturesListing&, unsigned interval, std::int64_t cutoff_ns);
HistoricalDailyRange daily_contract_range(const FuturesListing&, std::int64_t cutoff_ns);
class Minutes final : public HistoricalBarPort {
public:
  explicit Minutes(std::string token, Post post = https_transport());
  PluginDescriptor descriptor() const override;
  HistorySemantics semantics() const override;
  void start() override;
  void stop() noexcept override;
  std::vector<HistoricalBar> read(const HistoricalBarRange&, std::stop_token) override;

private:
  std::string token_;
  Post post_;
  bool started_ = false;
};
class Daily final : public HistoricalDailyPort {
public:
  explicit Daily(std::string token, Post post = https_transport());
  PluginDescriptor descriptor() const override;
  HistorySemantics semantics() const override;
  void start() override;
  void stop() noexcept override;
  std::vector<HistoricalDailyBar> read(const HistoricalDailyRange&, std::stop_token) override;

private:
  std::string token_;
  Post post_;
  bool started_ = false;
};
} // namespace asterion::tushare
