#include "ctp_catalog.hpp"
#include "ctp_feed.hpp"
#include "ctp_support.hpp"
#include <ThostFtdcTraderApi.h>
#include <asterion/foundation/error.hpp>
#include <condition_variable>
#include <exception>
#include <map>
#include <mutex>
#include <regex>
#include <iconv.h>
namespace asterion::ctp {
namespace {
std::string name_utf8(std::string input) {
  if (input.empty())
    return {};
  const auto converter = iconv_open("UTF-8", "GB18030");
  if (converter == reinterpret_cast<iconv_t>(-1))
    throw Error(ErrorCode::unavailable, "CTP catalog name conversion unavailable");
  struct Close {
    iconv_t converter;
    ~Close() { iconv_close(converter); }
  } close{converter};
  std::string result(input.size() * 4 + 4, '\0');
  char* in = input.data();
  char* out = result.data();
  auto remaining = input.size(), space = result.size();
  if (iconv(converter, &in, &remaining, &out, &space) == static_cast<std::size_t>(-1) || remaining)
    throw Error(ErrorCode::operation_failed, "invalid CTP catalog name encoding");
  result.resize(result.size() - space);
  return result;
}
enum class Step { connecting, authenticate, login, query, done };
struct Reader final : CThostFtdcTraderSpi {
  std::mutex mutex;
  std::condition_variable_any wake;
  Step step = Step::connecting;
  bool completed = false;
  std::exception_ptr failure;
  Catalog result;
  std::map<std::string, CatalogContract> contracts;
  template <class F> void callback(F action) noexcept {
    try {
      std::lock_guard lock(mutex);
      if (!failure)
        action();
    } catch (...) {
      std::lock_guard lock(mutex);
      failure = std::current_exception();
    }
    wake.notify_all();
  }
  static void check(CThostFtdcRspInfoField* info) {
    if (info && info->ErrorID)
      throw Error(ErrorCode::operation_failed,
                  "CTP catalog request rejected: " + std::to_string(info->ErrorID));
  }
  void OnFrontConnected() override {
    callback([&] {
      if (step == Step::connecting)
        completed = true;
    });
  }
  void OnFrontDisconnected(int reason) override {
    callback([&] {
      throw Error(ErrorCode::unavailable, "CTP catalog disconnected: " + std::to_string(reason));
    });
  }
  void OnRspError(CThostFtdcRspInfoField* info, int, bool) override {
    callback([&] { check(info); });
  }
  void OnRspAuthenticate(CThostFtdcRspAuthenticateField*, CThostFtdcRspInfoField* info, int id,
                         bool last) override {
    callback([&] {
      if (step != Step::authenticate || id != 1)
        return;
      check(info);
      completed = last;
    });
  }
  void OnRspUserLogin(CThostFtdcRspUserLoginField* login, CThostFtdcRspInfoField* info, int id,
                      bool last) override {
    callback([&] {
      if (step != Step::login || id != 2)
        return;
      check(info);
      if (login)
        result.trading_day = field(login->TradingDay);
      if (last && !std::regex_match(result.trading_day, std::regex("[0-9]{8}")))
        throw Error(ErrorCode::operation_failed, "CTP catalog login has no trading day");
      completed = last;
    });
  }
  void OnRspQryInstrument(CThostFtdcInstrumentField* item, CThostFtdcRspInfoField* info, int id,
                          bool last) override {
    callback([&] {
      if (step != Step::query || id != 3)
        return;
      check(info);
      if (item && item->ProductClass == THOST_FTDC_PC_Futures && item->IsTrading) {
        CatalogContract entry;
        entry.instrument = {field(item->ExchangeID), field(item->InstrumentID)};
        entry.name = name_utf8(field(item->InstrumentName));
        validate_instruments({entry.instrument});
        entry.product = field(item->ProductID);
        entry.expiry = field(item->ExpireDate);
        entry.multiplier = item->VolumeMultiple;
        const auto tick = price(item->PriceTick);
        if (!tick || *tick <= Decimal{} || entry.multiplier <= 0 || entry.product.empty() ||
            !std::regex_match(entry.expiry, std::regex("[0-9]{8}")))
          throw Error(ErrorCode::operation_failed, "invalid CTP catalog contract metadata");
        entry.price_tick = *tick;
        if (contracts.size() >= 20000)
          throw Error(ErrorCode::resource_exhausted, "CTP catalog exceeds contract limit");
        const auto key = entry.instrument.venue + "." + entry.instrument.symbol;
        if (!contracts.emplace(key, std::move(entry)).second)
          throw Error(ErrorCode::operation_failed, "duplicate CTP catalog contract");
      }
      completed = last;
    });
  }
  void begin(Step next) {
    std::lock_guard lock(mutex);
    if (failure)
      std::rethrow_exception(failure);
    step = next;
    completed = false;
  }
  void wait(std::stop_token stop, std::chrono::steady_clock::time_point deadline) {
    std::unique_lock lock(mutex);
    const bool ready = wake.wait_until(lock, stop, deadline, [&] { return completed || failure; });
    if (stop.stop_requested())
      throw Error(ErrorCode::cancelled, "CTP catalog cancelled");
    if (failure)
      std::rethrow_exception(failure);
    if (!ready)
      throw Error(ErrorCode::unavailable, "CTP catalog timed out");
  }
};
} // namespace
Catalog read_catalog(const std::filesystem::path& library, const std::filesystem::path& flow,
                     CatalogConfiguration config, std::stop_token stop,
                     std::chrono::milliseconds timeout) {
  struct Secrets {
    CatalogConfiguration& config;
    ~Secrets() {
      erase(config.password);
      erase(config.auth_code);
    }
  } secrets{config};
  if (!config.front.starts_with("tcp://") || config.front.find('\0') != std::string::npos ||
      config.app_id.empty() != config.auth_code.empty() || timeout.count() <= 0)
    throw std::invalid_argument("invalid CTP catalog configuration");
  if (stop.stop_requested())
    throw Error(ErrorCode::cancelled, "CTP catalog cancelled");
  CThostFtdcReqUserLoginField login{};
  copy(login.BrokerID, config.broker);
  copy(login.UserID, config.user);
  copy(login.Password, config.password);
  struct LoginSecret {
    CThostFtdcReqUserLoginField& value;
    ~LoginSecret() { erase(value.Password); }
  } login_secret{login};
  CThostFtdcReqAuthenticateField auth{};
  struct AuthSecret {
    CThostFtdcReqAuthenticateField& value;
    ~AuthSecret() { erase(value.AuthCode); }
  } auth_secret{auth};
  if (!config.app_id.empty()) {
    copy(auth.BrokerID, config.broker);
    copy(auth.UserID, config.user);
    copy(auth.AppID, config.app_id);
    copy(auth.AuthCode, config.auth_code);
  }
  SharedLibrary sdk(library, "?CreateFtdcTraderApi@CThostFtdcTraderApi@@SAPEAV1@PEBD@Z",
                    "_ZN19CThostFtdcTraderApi19CreateFtdcTraderApiEPKc");
  const auto factory = sdk.symbol<CThostFtdcTraderApi* (*)(const char*)>();
  if (!factory)
    throw Error(ErrorCode::unavailable, "CTP catalog SDK unavailable");
  std::filesystem::create_directories(flow);
  const auto raw = flow.u8string();
  const auto path = std::string(raw.begin(), raw.end()) + "/";
  Reader reader;
  auto* api = factory(path.c_str());
  if (!api)
    throw Error(ErrorCode::unavailable, "CTP catalog factory failed");
  struct Release {
    CThostFtdcTraderApi* api;
    ~Release() {
      api->RegisterSpi(nullptr);
      api->Release();
    }
  } release{api};
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  auto accepted = [](int code) {
    if (code)
      throw Error(ErrorCode::operation_failed,
                  "CTP catalog request failed: " + std::to_string(code));
  };
  api->RegisterSpi(&reader);
  api->RegisterFront(config.front.data());
  api->Init();
  reader.wait(stop, deadline);
  if (!config.app_id.empty()) {
    reader.begin(Step::authenticate);
    accepted(api->ReqAuthenticate(&auth, 1));
    erase(auth.AuthCode);
    reader.wait(stop, deadline);
  }
  reader.begin(Step::login);
#ifdef __APPLE__
  TThostFtdcClientSystemInfoType system{};
  accepted(api->ReqUserLogin(&login, 2, 0, system));
#else
  accepted(api->ReqUserLogin(&login, 2));
#endif
  erase(login.Password);
  reader.wait(stop, deadline);
  reader.begin(Step::query);
  CThostFtdcQryInstrumentField query{};
  accepted(api->ReqQryInstrument(&query, 3));
  reader.wait(stop, deadline);
  std::lock_guard lock(reader.mutex);
  reader.step = Step::done;
  for (auto& [key, contract] : reader.contracts)
    reader.result.contracts.push_back(std::move(contract));
  return std::move(reader.result);
}
} // namespace asterion::ctp
