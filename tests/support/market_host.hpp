#pragma once
#include "market_channel.hpp"
#include <asterion/system/durable_file.hpp>
#include <asterion/system/ipc/tcp_channel.hpp>
#include <asterion/system/process/child.hpp>
#include <nlohmann/json.hpp>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <thread>

namespace asterion::testing_support {

// Test-only installation. Provider fixtures and credentials stay on its host;
// clients use precisely the same pinned TLS entry point as Terminal.
struct MarketHost
{
    std::filesystem::path root;
    std::uint16_t port;
    std::string server_pin, reader_pin, manager_pin;
    std::unique_ptr<ChildProcess> process;

    explicit MarketHost(std::filesystem::path directory, std::string user = "fixture")
        : root(std::move(directory))
    {
        const int probe = ::socket(AF_INET, SOCK_STREAM, 0);
        if (probe < 0)
            throw std::runtime_error("cannot allocate Market test port");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t size = sizeof(address);
        const bool bound = ::bind(probe, reinterpret_cast<sockaddr*>(&address), size) == 0
            && ::getsockname(probe, reinterpret_cast<sockaddr*>(&address), &size) == 0;
        ::close(probe);
        if (!bound)
            throw std::runtime_error("cannot bind Market test port");
        port = ntohs(address.sin_port);
        for (const auto* name : {"bin", "config", "data", "lib", "credentials"})
            std::filesystem::create_directories(root / name);
        std::filesystem::create_symlink(ASTERION_MARKET_PATH, root / "bin/asterion_market");
        std::filesystem::create_symlink(ASTERION_TEST_CTP, root / "lib/ctp-md.so");
        std::filesystem::create_symlink(ASTERION_TEST_CTP_TRADER, root / "lib/ctp-trader.so");
        std::filesystem::create_directories(root / "config/tls");
        server_pin = ipc::create_identity(
            root / "config/tls/server.crt",
            root / "config/tls/server.key",
            "Market test");
        reader_pin = ipc::create_identity(root / "reader.crt", root / "reader.key", "Reader test");
        manager_pin =
            ipc::create_identity(root / "manager.crt", root / "manager.key", "Manager test");
        const nlohmann::json config{
            {"bind", "127.0.0.1"},
            {"port", port},
            {"data", (root / "data").string()},
            {"clients",
                {{{"fingerprint", reader_pin}, {"role", "read"}},
                    {{"fingerprint", manager_pin}, {"role", "manage"}}}},
            {"acquisition", {{"exchanges", nlohmann::json::array()}, {"tushare", nullptr}}},
            {"counter",
                {{"market_front", "tcp://127.0.0.1:1"},
                    {"trade_front", "tcp://127.0.0.1:1"},
                    {"broker", "fixture"},
                    {"user", user},
                    {"app_id", ""}}}
        };
        replace_file_durably(root / "config/market.json", config.dump());
        replace_file_durably(root / "credentials/asterion.ctp.password", "private-fixture-key");
    }

    void start()
    {
        process.reset();
        process = std::make_unique<ChildProcess>(
            "/usr/bin/env",
            std::vector<std::string>{
                "CREDENTIALS_DIRECTORY=" + (root / "credentials").string(),
                (root / "bin/asterion_market").string()
            });
    }

    ipc::TlsIdentity tls(const std::string& name = "reader") const
    {
        return {(root / (name + ".crt")).string(), (root / (name + ".key")).string(), {server_pin}};
    }

    MarketChannel open(const std::string& name = "reader")
    {
        using namespace std::chrono_literals;
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        for (;;) {
            try {
                auto probe = ipc::TcpChannel::connect("127.0.0.1", port, 100ms);
                return MarketChannel(port, tls(name));
            } catch (const std::exception&) {
                if (process->exited() || std::chrono::steady_clock::now() >= deadline)
                    throw;
                std::this_thread::sleep_for(10ms);
            }
        }
    }
};

} // namespace asterion::testing_support
