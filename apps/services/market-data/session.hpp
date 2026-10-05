#pragma once
#include <asterion/kernel/rpc_host.hpp>
#include <asterion/v1/market.pb.h>
#include <filesystem>
#include <future>
namespace asterion::market_data {
// A connection retains its cursor; only the market owner reads or changes it.
struct WatchCursor {
  std::optional<std::uint64_t> catalog, feed;
  std::uint64_t intraday = 0, sequence = 0;
};
class Session {
public:
  struct Configuration {
    std::string service, instance;
    std::filesystem::path directory, sdk, catalog_sdk;
  };
  explicit Session(Configuration config);
  ~Session();
  std::future<service::RpcHost::Message> request(market::v1::Request request,
                                                 std::shared_ptr<WatchCursor> cursor = {},
                                                 bool control = false);
  void stop();
  std::shared_future<void> stopped() const;

private:
  struct Loop;
  std::unique_ptr<Loop> loop_;
};
} // namespace asterion::market_data
