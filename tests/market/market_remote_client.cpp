#include "market_client.hpp"
#include <asterion/kernel/ipc/tls_channel.hpp>
#include <asterion/protocol/market.hpp>
#include <chrono>
#include <iostream>
#include <thread>
int main(int argc, char** argv) {
  if (argc != 3)
    return 2;
  try {
    const std::string root = argv[2];
    asterion::terminal::ServiceEndpoint endpoint{
        "127.0.0.1",
        "market.test",
        static_cast<std::uint16_t>(std::stoi(argv[1])),
        {root + "/ca.crt", root + "/client.crt", root + "/client.key"},
        {}};
    asterion::terminal::ServiceIo io;
    auto client = asterion::terminal::MarketClient::open(io, endpoint).get();
    client
        ->connect({{"front", "tcp://127.0.0.1:1"},
                   {"broker", "test"},
                   {"user", "fixture"},
                   {"password", "tls-fixture"},
                   {"instruments", {{{"venue", "SHFE"}, {"symbol", "rb2610"}}}}})
        .get();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
      const auto state = client->snapshot().get();
      if (state.at("transport_online") == true && state.at("subscriptions").size() == 1 &&
          !state["subscriptions"][0]["quote"].is_null()) {
        if (state["subscriptions"][0]["quote"]["last"] != "3510")
          return 3;
        break;
      }
      if (std::chrono::steady_clock::now() > deadline)
        return 4;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    {
      auto channel = asterion::ipc::TlsChannel::connect(
          "127.0.0.1", static_cast<std::uint16_t>(std::stoi(argv[1])),
          {root + "/ca.crt", root + "/client.crt", root + "/client.key"}, std::chrono::seconds(2));
      asterion::market::v1::Request request;
      request.set_version(1);
      request.set_service_id("market.test");
      request.set_correlation_id("events.test");
      request.mutable_events()->set_limit(1024);
      channel.send(request.SerializeAsString(), std::chrono::seconds(2));
      asterion::market::v1::Response response;
      if (!response.ParseFromString(channel.receive(std::chrono::seconds(2))) ||
          !response.has_events())
        return 6;
      const auto& batch = response.events();
      if (batch.gap() || batch.failed() || batch.stream_id().empty())
        return 7;
      unsigned quotes = 0, reordered = 0;
      std::uint64_t sequence = 0;
      for (const auto& event : batch.events()) {
        if (event.sequence() != ++sequence)
          return 8;
        if (event.has_quote()) {
          ++quotes;
          reordered += event.quote().out_of_order();
        }
      }
      if (quotes < 2 || reordered < 1)
        return 9;
      if (response.SerializeAsString().find("tls-fixture") != std::string::npos)
        return 10;
    }
    client->disconnect().get();
    return client->snapshot().get().at("phase") == "disconnected" ? 0 : 5;
  } catch (const std::exception& e) {
    std::cerr << e.what();
    return 1;
  }
}
