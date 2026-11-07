#include "market_client.hpp"
#include "service_relay.hpp"
#include <asterion/market.pb.h>
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
            "Market",
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
        for (bool quoted = false; !quoted;) {
            const auto taken = take();
            for (const auto& update : taken.at("frames")) {
                const auto& board =
                    update.contains("full") ? update.at("full") : update.at("changed");
                const auto& rows = board.value("rows", asterion::Json::array());
                if (rows.size() == 1 && rows[0].contains("quote")) {
                    if (rows[0]["quote"]["last"]["text"] != "3510")
                        return 3;
                    quoted = taken.at("online") == true;
                }
            }
            if (std::chrono::steady_clock::now() > deadline)
                return 4;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        const auto call = [&](asterion::Json request) {
            return asterion::terminal::call_market(io, endpoint, std::move(request)).get();
        };
        const auto status = call({{"getStatus", asterion::Json::object()}}).at("getStatus");
        if (status.at("feed").at("state") != "FEED_STATE_CONNECTED" || status.at("subscribed") != 1
            || std::stoi(status.at("outOfOrderQuotes").get<std::string>()) < 1)
            return 11;
        try {
            asterion::terminal::relay_call<::market::Request, ::market::Response>(
                io,
                endpoint,
                {{"getStatus", asterion::Json::object()}},
                [](const ::market::Request&) {},
                "another protocol")
                .get();
            return 12;
        } catch (const asterion::Error& error) {
            if (std::string(error.what()) != asterion::terminal::service_protocol_differs)
                return 13;
        }
        watch.reset();
        auto again = asterion::terminal::MarketWatch::open(io, endpoint).get();
        return call({{"getStatus", asterion::Json::object()}}).contains("getStatus") ? 0 : 5;
    } catch (const std::exception& e) {
        std::cerr << e.what();
        return 1;
    }
}
