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
#include <thread>
class Fake final : public CThostFtdcMdApi {
  CThostFtdcMdSpi* spi = nullptr;
  std::recursive_mutex mutex;
  std::jthread worker;
  const std::thread::id owner = std::this_thread::get_id();
  const std::filesystem::path flow;
  CThostFtdcMdSpi* retiring_spi = nullptr;
  void require_owner() const {
    if (owner != std::this_thread::get_id())
      std::abort();
  }
  bool gate(const char* name) {
    const auto held = flow / name;
    if (!std::filesystem::exists(held))
      return false;
    std::ofstream(held.string() + ".entered").put('1');
    while (std::filesystem::exists(held))
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return true;
  }
  bool reconnect = true;
  bool publication = false, flood = false;
  std::optional<CThostFtdcDepthMarketDataField> publication_tick;

public:
  explicit Fake(const char* path) : flow(path) {}
  void Release() override {
    require_owner();
    worker.request_stop();
    if (worker.joinable())
      worker.join();
    if (gate("hold-release") && retiring_spi)
      retiring_spi->OnRspUserLogin(nullptr, nullptr, 1, true);
    delete this;
  }
  void Init() override {
    require_owner();
    gate("hold-init");
    worker = std::jthread([this](std::stop_token stop) {
      int n = 0;
      while (!stop.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::lock_guard lock(mutex);
        if (!spi)
          continue;
        if (n == 0 || (reconnect && n == 14))
          spi->OnFrontConnected();
        if (reconnect && n == 9)
          spi->OnFrontDisconnected(4097);
        if (publication && publication_tick) {
          auto& tick = *publication_tick;
          // Explicit test observations advance a minute per callback so delayed
          // intraday publication is exercised without a minute-long test sleep.
          const int minute = n % (24 * 60);
          std::snprintf(tick.UpdateTime, sizeof(tick.UpdateTime), "%02d:%02d:00", minute / 60,
                        minute % 60);
          tick.LastPrice = 3510 + n;
          tick.Volume += 1;
          spi->OnRtnDepthMarketData(&tick);
          gate("hold-publication");
        }
        ++n;
      }
    });
  }
  int Join() override { return 0; }
  const char* GetTradingDay() override { return "20260928"; }
  void RegisterFront(char*) override { require_owner(); }
  void RegisterNameServer(char*) override {}
  void RegisterFensUserInfo(CThostFtdcFensUserInfoField*) override {}
  void RegisterSpi(CThostFtdcMdSpi* value) override {
    require_owner();
    std::lock_guard lock(mutex);
    if (!value && std::filesystem::exists(flow / "hold-release"))
      retiring_spi = spi;
    spi = value;
  }
  int SubscribeMarketData(char* names[], int count) override {
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
      if (spi)
        spi->OnRspSubMarketData(&instrument, &info, 1, i == count - 1);
      if (info.ErrorID || !spi)
        continue;
      CThostFtdcDepthMarketDataField tick{};
      std::strncpy(tick.InstrumentID, names[i], sizeof(tick.InstrumentID) - 1);
      std::strcpy(tick.ExchangeID, "SHFE");
      auto epoch = std::time(nullptr) + 8 * 3600;
      std::tm tm{};
#ifdef _WIN32
      gmtime_s(&tm, &epoch);
#else
      gmtime_r(&epoch, &tm);
#endif
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
      if (publication) {
        std::strcpy(tick.UpdateTime, "00:00:00");
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
  int UnSubscribeMarketData(char*[], int) override {
    require_owner();
    return 0;
  }
  int SubscribeForQuoteRsp(char*[], int) override { return 0; }
  int UnSubscribeForQuoteRsp(char*[], int) override { return 0; }
  int ReqUserLogin(CThostFtdcReqUserLoginField* login, int id) override {
    require_owner();
    std::lock_guard lock(mutex);
    const std::string user(login->UserID);
    publication = user == "publication";
    flood = user == "flood";
    reconnect = user != "steady" && !publication && !flood;
    CThostFtdcRspInfoField info{};
    if (std::string(login->Password) == "reject-test-only")
      info.ErrorID = 3;
    if (spi)
      spi->OnRspUserLogin(nullptr, &info, id, true);
    return 0;
  }
  int ReqUserLogout(CThostFtdcUserLogoutField*, int) override { return 0; }
  int ReqQryMulticastInstrument(CThostFtdcQryMulticastInstrumentField*, int) override { return 0; }
};
CThostFtdcMdApi* CThostFtdcMdApi::CreateFtdcMdApi(const char* flow, bool, bool) {
  return new Fake(flow);
}
const char* CThostFtdcMdApi::GetApiVersion() {
  return "TEST-ONLY";
}
