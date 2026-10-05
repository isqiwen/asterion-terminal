#include "ctp_catalog.hpp"
#include "ctp_support.hpp"
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/protocol/market.hpp>
#include <gtest/gtest.h>
#include <thread>
using namespace asterion;
using namespace std::chrono_literals;
namespace {
class CtpCatalog : public ::testing::Test {
protected:
  std::filesystem::path flow =
      std::filesystem::temp_directory_path() / ("asterion-catalog-" + unique_process_id());
  ctp::CatalogConfiguration config{"tcp://127.0.0.1:1", "9999", "catalog", "secret", "", ""};
  void TearDown() override {
    std::error_code error;
    std::filesystem::remove_all(flow, error);
  }
  ctp::Catalog read(std::stop_token stop = {}, std::chrono::milliseconds timeout = 2s) {
    ctp::CatalogQuery query(ASTERION_TEST_CTP_TRADER, flow, config, timeout);
    for (;;) {
      if (auto result = query.poll(stop))
        return std::move(*result);
      std::this_thread::sleep_for(1ms);
    }
  }
};
} // namespace
TEST_F(CtpCatalog, ReturnsOnlyActiveFuturesWithoutSettlementOrOrderActions) {
  ctp::SharedLibrary control(ASTERION_TEST_CTP_TRADER, "asterion_fake_catalog_side_effects",
                             "asterion_fake_catalog_side_effects");
  const auto count = control.symbol<int (*)()>();
  const int before = count();
  const auto result = read();
  EXPECT_EQ(result.trading_day, "20260928");
  ASSERT_EQ(result.contracts.size(), 1U);
  const auto& contract = result.contracts.front();
  EXPECT_EQ(contract.instrument.venue, "SHFE");
  EXPECT_EQ(contract.instrument.symbol, "rb2610");
  EXPECT_EQ(contract.product, "rb");
  EXPECT_EQ(contract.name, "螺纹钢");
  EXPECT_EQ(contract.expiry, "20261015");
  EXPECT_EQ(contract.multiplier, 10);
  EXPECT_EQ(contract.price_tick.str(), "0.5");
  EXPECT_EQ(count(), before);
}
TEST_F(CtpCatalog, AuthAndLoginErrorsRejectRatherThanReturnAnEmptyCatalog) {
  config.app_id = "app";
  config.auth_code = "bad-auth";
  EXPECT_THROW(read(), Error);
  config.auth_code = "auth";
  config.password = "bad";
  EXPECT_THROW(read(), Error);
  config.password = "secret";
  EXPECT_EQ(read().contracts.size(), 1U);
}
TEST_F(CtpCatalog, RejectsPartialInvalidAndDuplicateResults) {
  for (const auto* mode : {"catalog-error", "catalog-invalid", "catalog-dup"}) {
    SCOPED_TRACE(mode);
    config.user = mode;
    EXPECT_THROW(read(), Error);
  }
  config.user = "catalog-empty";
  EXPECT_TRUE(read().contracts.empty());
}
TEST_F(CtpCatalog, TimeoutAndCancellationDoNotReturnPartialResults) {
  config.user = "catalog-partial";
  try {
    read({}, 150ms);
    FAIL() << "expected query timeout";
  } catch (const Error& error) {
    EXPECT_EQ(error.code(), ErrorCode::unavailable);
    EXPECT_STREQ(error.what(), "CTP catalog instrument query timed out");
  }
  config.user = "catalog-stall";
  std::stop_source stop;
  std::jthread cancel([&] {
    std::this_thread::sleep_for(120ms);
    stop.request_stop();
  });
  try {
    read(stop.get_token());
    FAIL() << "expected cancellation";
  } catch (const Error& error) {
    EXPECT_EQ(error.code(), ErrorCode::cancelled);
  }
}
TEST_F(CtpCatalog, FilteredResponsesKeepQueryAliveUntilFinalResponse) {
  config.user = "catalog-stream";
  const auto result = read({}, 300ms);
  ASSERT_EQ(result.contracts.size(), 1U);
  EXPECT_EQ(result.contracts.front().instrument.symbol, "rb2610");
}

TEST(CtpCatalogService, EmptyWatchlistLoadsMarketAndQueriesRemainResponsive) {
  const auto directory =
      std::filesystem::path("/tmp") / ("ast-cat-" + unique_process_id().substr(0, 12));
  std::filesystem::create_directory(directory);
  std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code e;
      std::filesystem::remove_all(path, e);
    }
  } cleanup{directory};
  const auto endpoint = (directory / "market.sock").string();
  ChildProcess service(ASTERION_MARKET_PATH,
                       {"--directory", directory.string(), "--endpoint", endpoint, "--session",
                        "catalog-test", "--ctp-library", ASTERION_TEST_CTP, "--ctp-catalog-library",
                        ASTERION_TEST_CTP_TRADER});
  namespace wire = asterion::market::v1;
  auto call = [&](wire::Request request) {
    request.set_version(1);
    request.set_service_id("catalog-test");
    request.set_correlation_id(unique_process_id());
    auto channel = ipc::Channel::connect(endpoint, 1s);
    channel.send(request.SerializeAsString(), 1s);
    wire::Response response;
    if (!response.ParseFromString(channel.receive(1s)))
      throw std::runtime_error("invalid market response");
    if (response.has_error())
      throw std::runtime_error(response.error().message());
    return response;
  };
  auto snapshot = [&] {
    wire::Request request;
    request.mutable_snapshot();
    return call(request).snapshot();
  };
  auto wait = [&](auto condition) {
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    for (;;) {
      try {
        auto state = snapshot();
        if (condition(state))
          return state;
      } catch (const std::exception&) {
        if (service.exited())
          throw;
      }
      if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error("market test deadline");
      std::this_thread::sleep_for(20ms);
    }
  };
  wait([](const auto&) { return true; });
  wire::Request login;
  auto* connection = login.mutable_connect();
  connection->set_front("tcp://127.0.0.1:1");
  connection->set_broker("test");
  connection->set_user("fixture");
  connection->set_password("fixture-only");
  call(login);
  wait([](const auto& state) { return state.phase() == "connected"; });
  EXPECT_EQ(snapshot().subscriptions_size(), 0);
  wire::Request request;
  auto* catalog = request.mutable_catalog();
  catalog->set_front("tcp://127.0.0.1:1");
  catalog->set_broker("test");
  catalog->set_user("catalog");
  catalog->set_password("fixture-only");
  EXPECT_EQ(call(request).snapshot().catalog().phase(), "loading");
  auto ready = wait([](const auto& state) {
    return state.catalog().phase() == "ready" && state.subscriptions_size() == 1 &&
           state.subscriptions(0).has_quote();
  });
  EXPECT_EQ(ready.watchlist_size(), 0);
  EXPECT_EQ(ready.subscriptions(0).instrument().symbol(), "rb2610");
  EXPECT_EQ(ready.subscriptions(0).quote().last(), "3510");
  const auto json = protocol::decode_market(ready);
  EXPECT_EQ(json.at("catalog").at("contracts").size(), 1U);
  EXPECT_TRUE(json.at("watchlist").empty());
  wire::Request watchlist;
  auto* item = watchlist.mutable_subscribe()->add_instruments();
  item->set_venue("SHFE");
  item->set_symbol("rb2610");
  EXPECT_EQ(call(watchlist).snapshot().watchlist_size(), 1);
  watchlist.mutable_subscribe()->clear_instruments();
  ready = call(watchlist).snapshot();
  EXPECT_EQ(ready.watchlist_size(), 0);
  EXPECT_EQ(ready.subscriptions_size(), 1);
  catalog->set_user("catalog-stall");
  call(request);
  wait([&](const auto& state) {
    return state.catalog().phase() == "loading" &&
           std::filesystem::exists(directory / "ctp-catalog-flow/catalog-query-started");
  });
  const auto begin = std::chrono::steady_clock::now();
  // Waiting for the catalog's last response must yield the shared SDK owner.
  auto* added = watchlist.mutable_subscribe()->add_instruments();
  added->set_venue("SHFE");
  added->set_symbol("rb2710");
  call(watchlist);
  const auto while_waiting = wait([](const auto& state) {
    return state.subscriptions_size() == 1 && state.subscriptions(0).has_quote() &&
           state.subscriptions(0).instrument().symbol() == "rb2710";
  });
  EXPECT_EQ(while_waiting.catalog().phase(), "loading");
  wire::Request disconnect;
  disconnect.mutable_disconnect();
  // The catalog loaded earlier in this test remains available offline.
  EXPECT_EQ(call(disconnect).snapshot().catalog().phase(), "cached");
  EXPECT_LT(std::chrono::steady_clock::now() - begin, 1s);
  // A cancelled query may finish releasing its SDK later; its result cannot
  // replace the cached catalog or restart subscriptions after disconnect.
  std::this_thread::sleep_for(200ms);
  const auto disconnected = snapshot();
  EXPECT_EQ(disconnected.phase(), "disconnected");
  EXPECT_EQ(disconnected.catalog().phase(), "cached");
}
