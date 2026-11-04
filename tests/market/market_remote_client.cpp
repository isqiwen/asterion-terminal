#include "market_client.hpp"
#include "support/market_channel.hpp"
#include <asterion/system/ipc/tcp_channel.hpp>
#include <asterion/protocol/market.hpp>
#include <chrono>
#include <iostream>
#include <thread>

int main(int argc, char** argv)
{
    if (argc != 3)
        return 2;
    try {
        const std::filesystem::path tls = argv[2];
        const asterion::ipc::TlsIdentity identity{
            (tls / "reader.crt").string(),
            (tls / "reader.key").string(),
            {asterion::ipc::certificate_fingerprint((tls / "server.crt").string())}
        };
        asterion::terminal::ServiceEndpoint endpoint{
            "127.0.0.1",
            "market-data",
            static_cast<std::uint16_t>(std::stoi(argv[1])),
            {},
            identity
        };
        asterion::terminal::ServiceIo io;
        auto watch = asterion::terminal::MarketWatch::open(io, endpoint).get();
        const auto take = [&] {
            return io
                .submit<asterion::Json>(
                    [&](std::stop_token) -> asterion::PolledTask<asterion::Json> {
                        co_return watch->take(false);
                    })
                .get();
        };
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        std::string instance;
        for (bool quoted = false; !quoted;) {
            const auto taken = take();
            for (const auto& update : taken.at("frames")) {
                const auto& frame =
                    update.contains("snapshot") ? update.at("snapshot") : update.at("delta");
                instance = frame.at("instanceId");
                const auto& rows = frame.value("quotes", asterion::Json::array());
                if (rows.size() == 1 && rows[0].contains("quote")) {
                    if (rows[0]["quote"]["last"] != "3510")
                        return 3;
                    quoted = taken.at("online") == true;
                }
            }
            if (std::chrono::steady_clock::now() > deadline)
                return 4;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        {
            asterion::testing_support::MarketChannel channel(
                static_cast<std::uint16_t>(std::stoi(argv[1])),
                identity);
            asterion::market::proto::Request request;
            request.set_version(1);
            request.set_service_id("market-data");
            request.set_correlation_id("events.test");
            request.mutable_read_feed_events()->set_limit(1024);
            channel.send(request.SerializeAsString(), std::chrono::seconds(2));
            asterion::market::proto::Response response;
            if (!response.ParseFromString(channel.receive(std::chrono::seconds(2)))
                || !response.has_events())
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
        const auto call = [&](asterion::Json request) {
            return asterion::terminal::call_market(io, endpoint, std::move(request)).get();
        };
        const auto recording =
            call({{"getRecordingStatus", asterion::Json::object()}}).at("recordingStatus");
        if (recording.at("instance") != instance || recording.at("phase") != "connected"
            || recording.at("subscribed") != 1)
            return 11;
        watch.reset();
        auto again = asterion::terminal::MarketWatch::open(io, endpoint).get();
        return call({{"getLiveSnapshot", asterion::Json::object()}}).at("snapshot").at("instanceId")
                == instance
            ? 0
            : 5;
    } catch (const std::exception& e) {
        std::cerr << e.what();
        return 1;
    }
}
