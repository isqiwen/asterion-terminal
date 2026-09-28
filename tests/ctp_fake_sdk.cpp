// Test-only CTP ABI double. Never staged as a product resource.
#include <ThostFtdcMdApi.h>
#include <chrono>
#include <cstring>
#include <ctime>
#include <limits>
#include <mutex>
#include <thread>
class Fake final : public CThostFtdcMdApi {
  CThostFtdcMdSpi* spi = nullptr;
  std::recursive_mutex mutex;
  std::jthread worker;

public:
  void Release() override {
    worker.request_stop();
    if (worker.joinable())
      worker.join();
    delete this;
  }
  void Init() override {
    worker = std::jthread([this](std::stop_token stop) {
      int n = 0;
      while (!stop.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::lock_guard lock(mutex);
        if (!spi)
          continue;
        if (n == 0 || n == 14)
          spi->OnFrontConnected();
        if (n == 9)
          spi->OnFrontDisconnected(4097);
        ++n;
      }
    });
  }
  int Join() override { return 0; }
  const char* GetTradingDay() override { return "20260928"; }
  void RegisterFront(char*) override {}
  void RegisterNameServer(char*) override {}
  void RegisterFensUserInfo(CThostFtdcFensUserInfoField*) override {}
  void RegisterSpi(CThostFtdcMdSpi* value) override {
    std::lock_guard lock(mutex);
    spi = value;
  }
  int SubscribeMarketData(char* names[], int count) override {
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
      tick.Volume = 5;
      tick.OpenInterest = 100;
      tick.PreSettlementPrice = std::numeric_limits<double>::max();
      tick.UpdateMillisec = 500;
      spi->OnRtnDepthMarketData(&tick);
      tick.UpdateMillisec = 0;
      tick.LastPrice = 1;
      spi->OnRtnDepthMarketData(&tick); // Must be discarded as out of order.
    }
    return 0;
  }
  int UnSubscribeMarketData(char*[], int) override { return 0; }
  int SubscribeForQuoteRsp(char*[], int) override { return 0; }
  int UnSubscribeForQuoteRsp(char*[], int) override { return 0; }
  int ReqUserLogin(CThostFtdcReqUserLoginField* login, int id) override {
    std::lock_guard lock(mutex);
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
CThostFtdcMdApi* CThostFtdcMdApi::CreateFtdcMdApi(const char*, bool, bool) {
  return new Fake;
}
const char* CThostFtdcMdApi::GetApiVersion() {
  return "TEST-ONLY";
}
