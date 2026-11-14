// Test-only CTP ABI double. Never staged as a product resource.
#include <ThostFtdcMdApi.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <ctime>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

class Fake final : public CThostFtdcMdApi
{
public:
    explicit Fake(const char* path)
        : flow(path)
    {}

    void Release() override
    {
        require_owner();
        worker.request_stop();
        if (worker.joinable())
            worker.join();
        if (gate("hold-release") && retiring_spi)
            retiring_spi->OnRspUserLogin(nullptr, nullptr, 1, true);
        delete this;
    }

    void Init() override
    {
        require_owner();
        gate("hold-init");
        worker = std::jthread([this](std::stop_token stop) {
            int n = 0;
            while (!stop.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                std::lock_guard lock(mutex);
                if (!spi)
                    continue;
                if (std::filesystem::exists(flow / "hold-front"))
                    continue;
                if (n == 0 || (reconnect && n == 14))
                    spi->OnFrontConnected();
                if (reconnect && n == 9)
                    spi->OnFrontDisconnected(4097);
                if (publication && publication_tick) {
                    auto& tick = *publication_tick;
                    // Explicit test observations advance a minute per callback from the
                    // current one, so delayed intraday publication is exercised without
                    // a minute-long test sleep.
                    const int minute = (login_minute + n) % (24 * 60);
                    std::snprintf(
                        tick.UpdateTime,
                        sizeof(tick.UpdateTime),
                        "%02d:%02d:00",
                        minute / 60,
                        minute % 60);
                    tick.LastPrice = 3510 + n;
                    tick.Volume += 1;
                    spi->OnRtnDepthMarketData(&tick);
                    gate("hold-publication");
                }
                // A `disconnect` file in the flow directory takes the front down until it is removed.
                if (strategy_stream && publication_tick
                    && std::filesystem::exists(flow / "disconnect") != offline) {
                    offline = !offline;
                    if (offline)
                        spi->OnFrontDisconnected(4097);
                    else
                        spi->OnFrontConnected();
                }
                if (strategy_stream && publication_tick && !offline) {
                    auto& tick = *publication_tick;
                    const auto now = std::chrono::system_clock::now();
                    auto epoch = std::chrono::system_clock::to_time_t(now) + 8 * 3600;
                    std::tm tm{};
                    gmtime_r(&epoch, &tm);
                    std::strftime(tick.ActionDay, sizeof(tick.ActionDay), "%Y%m%d", &tm);
                    auto day = epoch + (next_day && n > 20 ? 86400 : 0);
                    std::tm trading{};
                    gmtime_r(&day, &trading);
                    std::strftime(tick.TradingDay, sizeof(tick.TradingDay), "%Y%m%d", &trading);
                    std::strftime(tick.UpdateTime, sizeof(tick.UpdateTime), "%H:%M:%S", &tm);
                    if (!night.empty()) {
                        // DCE names the trading day in both dates; CZCE the civil day.
                        const auto day = beijing_date(night == "DCE" ? 1 : 0);
                        std::strcpy(tick.ActionDay, day.c_str());
                        std::strcpy(tick.TradingDay, day.c_str());
                    }
                    tick.UpdateMillisec = std::chrono::duration_cast<std::chrono::milliseconds>(
                                              now.time_since_epoch())
                                              .count()
                        % 1000;
                    tick.LastPrice = 3510 + n % 20;
                    tick.BidPrice1 = tick.LastPrice - 1;
                    tick.AskPrice1 = tick.LastPrice + 1;
                    ++tick.Volume;
                    spi->OnRtnDepthMarketData(&tick);
                }
                ++n;
            }
        });
    }

    int Join() override
    {
        return 0;
    }

    const char* GetTradingDay() override
    {
        return "20260928";
    }

    void RegisterFront(char*) override
    {
        require_owner();
    }

    void RegisterNameServer(char*) override {}

    void RegisterFensUserInfo(CThostFtdcFensUserInfoField*) override {}

    void RegisterSpi(CThostFtdcMdSpi* value) override
    {
        require_owner();
        std::lock_guard lock(mutex);
        if (!value && std::filesystem::exists(flow / "hold-release"))
            retiring_spi = spi;
        spi = value;
    }

    int SubscribeMarketData(char* names[], int count) override
    {
        require_owner();
        if (count > 50)
            return -3;
        std::lock_guard lock(mutex);
        for (int i = 0; i < count; ++i) {
            CThostFtdcSpecificInstrumentField instrument{};
            std::strncpy(instrument.InstrumentID, names[i], sizeof(instrument.InstrumentID) - 1);
            CThostFtdcRspInfoField info{};
            if (std::string(names[i]).starts_with("bad"))
                info.ErrorID = 31;
            // Refused once, then accepted.
            static std::set<std::string> refused;
            if (std::string(names[i]).starts_with("fla") && refused.insert(names[i]).second)
                info.ErrorID = 31;
            if (spi)
                spi->OnRspSubMarketData(&instrument, &info, 1, i == count - 1);
            if (info.ErrorID || !spi)
                continue;
            CThostFtdcDepthMarketDataField tick{};
            std::strncpy(tick.InstrumentID, names[i], sizeof(tick.InstrumentID) - 1);
            std::strcpy(tick.ExchangeID, night.empty() ? "SHFE" : night.c_str());
            auto epoch = std::time(nullptr) + 8 * 3600;
            std::tm tm{};
            gmtime_r(&epoch, &tm);
            std::strftime(tick.ActionDay, sizeof(tick.ActionDay), "%Y%m%d", &tm);
            std::strftime(tick.TradingDay, sizeof(tick.TradingDay), "%Y%m%d", &tm);
            std::strftime(tick.UpdateTime, sizeof(tick.UpdateTime), "%H:%M:%S", &tm);
            tick.LastPrice = 3510;
            tick.BidPrice1 = 3509;
            tick.AskPrice1 = 3511;
            tick.BidVolume1 = 2;
            tick.AskVolume1 = 3;
            tick.BidPrice2 = 3508.25;
            tick.BidVolume2 = 4;
            tick.AskPrice2 = 3512.5;
            tick.AskVolume2 = 5;
            tick.BidPrice3 = std::numeric_limits<double>::max();
            tick.BidVolume3 = 8;
            tick.AskPrice3 = 3513;
            tick.AskVolume3 = 0;
            tick.BidPrice4 = 3506;
            tick.BidVolume4 = -1;
            tick.BidPrice5 = 3505;
            tick.BidVolume5 = 10;
            tick.AskPrice5 = 3515;
            tick.AskVolume5 = 11;
            tick.Volume = 5;
            tick.OpenInterest = 100;
            tick.PreOpenInterest = 125.25;
            tick.PreClosePrice = 3480.125;
            tick.OpenPrice = 3490.25;
            tick.UpperLimitPrice = 3800.5;
            tick.LowerLimitPrice = std::numeric_limits<double>::max();
            tick.PreSettlementPrice = std::numeric_limits<double>::max();
            tick.UpdateMillisec = 500;
            if (strategy_stream) {
                // A resubscription after a reconnect keeps the day's cumulative volume.
                if (!publication_tick)
                    publication_tick = tick;
                continue; // The worker emits only fresh, ordered observations.
            }
            if (publication) {
                std::snprintf(
                    tick.UpdateTime,
                    sizeof(tick.UpdateTime),
                    "%02d:%02d:00",
                    login_minute / 60,
                    login_minute % 60);
                if (!publication_tick)
                    publication_tick = tick;
            }
            spi->OnRtnDepthMarketData(&tick);
            tick.UpdateMillisec = 0;
            tick.LastPrice = 1;
            spi->OnRtnDepthMarketData(&tick); // Must be discarded as out of order.
            if (flood) {
                for (int n = 0; n < 100000; ++n)
                    spi->OnRtnDepthMarketData(&tick);
                std::ofstream(flow / "burst.done").put('1');
            }
        }
        return 0;
    }

    int UnSubscribeMarketData(char*[], int) override
    {
        require_owner();
        return 0;
    }

    int SubscribeForQuoteRsp(char*[], int) override
    {
        return 0;
    }

    int UnSubscribeForQuoteRsp(char*[], int) override
    {
        return 0;
    }

    int ReqUserLogin(CThostFtdcReqUserLoginField* login, int id) override
    {
        require_owner();
        if (std::filesystem::exists(flow / "hold-login"))
            return 0;
        std::lock_guard lock(mutex);
        const std::string user(login->UserID);
        publication = user == "publication" || user == "concurrency";
        flood = user == "flood";
        next_day = user == "next-day";
        night = user == "dce-night" ? "DCE" : user == "czce-night" ? "CZCE" : "";
        login_minute = static_cast<int>((std::time(nullptr) + 8 * 3600) % 86400 / 60);
        strategy_stream = user == "strategy" || next_day || !night.empty();
        reconnect = user != "steady" && user != "catalog-large" && !publication && !flood
            && !strategy_stream;
        CThostFtdcRspInfoField info{};
        if (std::string(login->Password) == "reject-test-only")
            info.ErrorID = 3;
        CThostFtdcRspUserLoginField session{};
        std::strcpy(session.TradingDay, beijing_date(1).c_str());
        if (spi)
            spi->OnRspUserLogin(night.empty() ? nullptr : &session, &info, id, true);
        return 0;
    }

    int ReqUserLogout(CThostFtdcUserLogoutField*, int) override
    {
        return 0;
    }

    int ReqQryMulticastInstrument(CThostFtdcQryMulticastInstrumentField*, int) override
    {
        return 0;
    }

private:
    void require_owner() const
    {
        if (owner != std::this_thread::get_id())
            std::abort();
    }

    bool gate(const char* name)
    {
        const auto held = flow / name;
        if (!std::filesystem::exists(held))
            return false;
        std::ofstream(held.string() + ".entered").put('1');
        while (std::filesystem::exists(held))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return true;
    }

    static std::string beijing_date(int days_after)
    {
        auto epoch = std::time(nullptr) + 8 * 3600 + days_after * 86400;
        std::tm tm{};
        gmtime_r(&epoch, &tm);
        char text[9];
        std::strftime(text, sizeof(text), "%Y%m%d", &tm);
        return text;
    }

    CThostFtdcMdSpi* spi = nullptr;
    std::recursive_mutex mutex;
    std::jthread worker;
    const std::thread::id owner = std::this_thread::get_id();
    const std::filesystem::path flow;
    CThostFtdcMdSpi* retiring_spi = nullptr;

    bool reconnect = true;
    bool publication = false, flood = false;
    bool strategy_stream = false, offline = false;
    std::optional<CThostFtdcDepthMarketDataField> publication_tick;
    // The connection stays up while its quotes move on to the next trading day.
    bool next_day = false;
    // Now is a night session of tomorrow's trading day, dated as DCE or CZCE date it.
    std::string night;

    // Minutes past Beijing midnight when the connection logged in.
    int login_minute = 0;
};

CThostFtdcMdApi* CThostFtdcMdApi::CreateFtdcMdApi(const char* flow, bool, bool)
{
    return new Fake(flow);
}

const char* CThostFtdcMdApi::GetApiVersion()
{
    return "TEST-ONLY";
}
