#include "ctp_catalog.hpp"
#include <asterion/domain/history_identity.hpp>
#include "ctp_feed.hpp"
#include "ctp_support.hpp"
#include <ThostFtdcTraderApi.h>
#include <asterion/foundation/error.hpp>
#include <asterion/foundation/bounded_queue.hpp>
#include <asterion/kernel/logger.hpp>
#include <atomic>
#include <variant>
#include <exception>
#include <map>
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
const char* timeout_diagnostic(Step step) {
  switch (step) {
  case Step::connecting:
    return "CTP catalog front connection timed out";
  case Step::authenticate:
    return "CTP catalog authentication timed out";
  case Step::login:
    return "CTP catalog login timed out";
  case Step::query:
    return "CTP catalog instrument query timed out";
  case Step::done:
    throw std::logic_error("completed CTP catalog query cannot be polled");
  }
  throw std::logic_error("invalid CTP catalog step");
}
struct Reader final : CThostFtdcTraderSpi {
  struct Connected {};
  struct Disconnected {
    int reason;
  };
  struct Failure {
    int code;
  };
  struct Auth {
    int code, id;
    bool last;
  };
  struct Login {
    std::optional<CThostFtdcRspUserLoginField> value;
    int code, id;
    bool last;
  };
  struct Instrument {
    std::optional<CThostFtdcInstrumentField> value;
    int code, id;
    bool last;
  };
  using Event = std::variant<Connected, Disconnected, Failure, Auth, Login, Instrument>;
  BoundedQueue<Event> incoming{32768};
  std::atomic<bool> failed{false};
  std::atomic<unsigned> instrument_responses{0};
  Step step = Step::connecting;
  bool completed = false;
  Catalog result;
  std::map<std::string, CatalogContract> contracts;
  template <class T> void deliver(T event) noexcept {
    try {
      if (!incoming.try_push(std::move(event)))
        failed = true;
    } catch (...) {
      failed = true;
    }
  }
  static void check(int code) {
    if (code)
      throw Error(ErrorCode::operation_failed,
                  "CTP catalog request rejected: " + std::to_string(code));
  }
  void OnFrontConnected() override { deliver(Connected{}); }
  void OnFrontDisconnected(int reason) override { deliver(Disconnected{reason}); }
  void OnRspError(CThostFtdcRspInfoField* info, int, bool) override {
    if (info && info->ErrorID)
      deliver(Failure{info->ErrorID});
  }
  void OnRspAuthenticate(CThostFtdcRspAuthenticateField*, CThostFtdcRspInfoField* info, int id,
                         bool last) override {
    deliver(Auth{info ? info->ErrorID : 0, id, last});
  }
  void OnRspUserLogin(CThostFtdcRspUserLoginField* value, CThostFtdcRspInfoField* info, int id,
                      bool last) override {
    deliver(
        Login{value ? std::optional(*value) : std::nullopt, info ? info->ErrorID : 0, id, last});
  }
  void OnRspQryInstrument(CThostFtdcInstrumentField* value, CThostFtdcRspInfoField* info, int id,
                          bool last) override {
    if (id == 3)
      ++instrument_responses;
    // QueryInstrument includes options and inactive contracts. Filter those
    // provider records before admission, while retaining failures and the end marker.
    std::optional<CThostFtdcInstrumentField> future;
    if (value && value->ProductClass == THOST_FTDC_PC_Futures && value->IsTrading)
      future = *value;
    const int code = info ? info->ErrorID : 0;
    if (future || code || last)
      deliver(Instrument{std::move(future), code, id, last});
  }
  void instrument(const CThostFtdcInstrumentField* item) {
    if (item) {
      CatalogContract entry;
      entry.instrument = {field(item->ExchangeID), field(item->InstrumentID)};
      entry.name = name_utf8(field(item->InstrumentName));
      validate_instruments({entry.instrument});
      entry.product = field(item->ProductID);
      entry.expiry = field(item->ExpireDate);
      if (item->DeliveryYear >= 1990 && item->DeliveryYear <= 2100 && item->DeliveryMonth >= 1 &&
          item->DeliveryMonth <= 12) {
        auto product = entry.product;
        std::ranges::transform(product, product.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const HistoryIdentity identity{entry.instrument.venue, product,
                                       std::to_string(item->DeliveryYear) + "-" +
                                           (item->DeliveryMonth < 10 ? "0" : "") +
                                           std::to_string(item->DeliveryMonth)};
        entry.contract_id = identity.key();
      }
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
  }
  void poll() {
    if (failed)
      throw Error(ErrorCode::resource_exhausted, "CTP catalog callback queue overflow");
    for (unsigned i = 0; i < 1024; ++i) {
      auto event = incoming.try_pop();
      if (!event)
        break;
      std::visit(
          [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, Connected>) {
              if (step == Step::connecting)
                completed = true;
            } else if constexpr (std::is_same_v<T, Disconnected>) {
              throw Error(ErrorCode::unavailable,
                          "CTP catalog disconnected: " + std::to_string(value.reason));
            } else if constexpr (std::is_same_v<T, Failure>) {
              check(value.code);
            } else if constexpr (std::is_same_v<T, Auth>) {
              if (step != Step::authenticate || value.id != 1)
                return;
              check(value.code);
              completed = value.last;
            } else if constexpr (std::is_same_v<T, Login>) {
              if (step != Step::login || value.id != 2)
                return;
              check(value.code);
              if (value.value)
                result.trading_day = field(value.value->TradingDay);
              if (value.last && !std::regex_match(result.trading_day, std::regex("[0-9]{8}")))
                throw Error(ErrorCode::operation_failed, "CTP catalog login has no trading day");
              completed = value.last;
            } else if constexpr (std::is_same_v<T, Instrument>) {
              if (step != Step::query || value.id != 3)
                return;
              check(value.code);
              instrument(value.value ? &*value.value : nullptr);
              completed = value.last;
            }
          },
          *event);
    }
  }
  void begin(Step next) {
    step = next;
    completed = false;
  }
};
} // namespace
struct CatalogQuery::Impl {
  const std::chrono::milliseconds timeout;
  std::chrono::steady_clock::time_point deadline;
  unsigned observed_responses = 0;
  CatalogConfiguration config;
  Reader reader;
  SharedLibrary library;
  CThostFtdcTraderApi* api = nullptr;
  CThostFtdcReqUserLoginField login{};
  CThostFtdcReqAuthenticateField auth{};
  Impl(const std::filesystem::path& path, CatalogConfiguration configuration,
       std::chrono::milliseconds timeout)
      : timeout(timeout), deadline(std::chrono::steady_clock::now() + timeout),
        config(std::move(configuration)),
        library(path, "?CreateFtdcTraderApi@CThostFtdcTraderApi@@SAPEAV1@PEBD@Z",
                "_ZN19CThostFtdcTraderApi19CreateFtdcTraderApiEPKc") {}
  ~Impl() {
    erase(config.password);
    erase(config.auth_code);
    erase(login.Password);
    erase(auth.AuthCode);
    if (api) {
      api->RegisterSpi(nullptr);
      api->Release();
    }
  }
  void start(const std::filesystem::path& flow, std::chrono::milliseconds timeout) {
    if (!config.front.starts_with("tcp://") || config.front.find('\0') != std::string::npos ||
        config.app_id.empty() != config.auth_code.empty() || timeout.count() <= 0)
      throw std::invalid_argument("invalid CTP catalog configuration");
    copy(login.BrokerID, config.broker);
    copy(login.UserID, config.user);
    copy(login.Password, config.password);
    erase(config.password);
    if (!config.app_id.empty()) {
      copy(auth.BrokerID, config.broker);
      copy(auth.UserID, config.user);
      copy(auth.AppID, config.app_id);
      copy(auth.AuthCode, config.auth_code);
    }
    erase(config.auth_code);
    const auto factory = library.symbol<CThostFtdcTraderApi* (*)(const char*)>();
    if (!factory)
      throw Error(ErrorCode::unavailable, "CTP catalog SDK unavailable");
    std::filesystem::create_directories(flow);
    const auto raw = flow.u8string();
    const auto path = std::string(raw.begin(), raw.end()) + "/";
    api = factory(path.c_str());
    if (!api)
      throw Error(ErrorCode::unavailable, "CTP catalog factory failed");
    api->RegisterSpi(&reader);
    api->RegisterFront(config.front.data());
    api->Init();
  }
  static void accepted(int code) {
    if (code)
      throw Error(ErrorCode::operation_failed,
                  "CTP catalog request failed: " + std::to_string(code));
  }
  std::optional<Catalog> poll(std::stop_token stop) {
    if (stop.stop_requested())
      throw Error(ErrorCode::cancelled, "CTP catalog cancelled");
    // Full catalogs include many options. Even filtered responses are progress;
    // only silence times out, and the final response still gates publication.
    const auto responses = reader.instrument_responses.load();
    if (responses != observed_responses) {
      observed_responses = responses;
      deadline = std::chrono::steady_clock::now() + timeout;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      log_process_event("ctp", LogLevel::warning, "catalog.timeout",
                        {{"diagnostic", timeout_diagnostic(reader.step)},
                         {"instrument_responses", reader.instrument_responses.load()},
                         {"active_futures", reader.contracts.size()}});
      throw Error(ErrorCode::unavailable, timeout_diagnostic(reader.step));
    }
    reader.poll();
    if (!reader.completed)
      return {};
    if (reader.step == Step::connecting && !config.app_id.empty()) {
      reader.begin(Step::authenticate);
      accepted(api->ReqAuthenticate(&auth, 1));
      erase(auth.AuthCode);
    } else if (reader.step == Step::connecting || reader.step == Step::authenticate) {
      reader.begin(Step::login);
#ifdef __APPLE__
      TThostFtdcClientSystemInfoType system{};
      accepted(api->ReqUserLogin(&login, 2, 0, system));
#else
      accepted(api->ReqUserLogin(&login, 2));
#endif
      erase(login.Password);
    } else if (reader.step == Step::login) {
      reader.begin(Step::query);
      CThostFtdcQryInstrumentField query{};
      accepted(api->ReqQryInstrument(&query, 3));
    } else if (reader.step == Step::query) {
      reader.step = Step::done;
      for (auto& [key, contract] : reader.contracts)
        reader.result.contracts.push_back(std::move(contract));
      return std::move(reader.result);
    } else
      throw std::logic_error("completed CTP catalog query cannot be polled");
    deadline = std::chrono::steady_clock::now() + timeout;
    return {};
  }
};
CatalogQuery::CatalogQuery(const std::filesystem::path& library, const std::filesystem::path& flow,
                           CatalogConfiguration config, std::chrono::milliseconds timeout)
    : impl_(std::make_unique<Impl>(library, std::move(config), timeout)) {
  impl_->start(flow, timeout);
}
CatalogQuery::~CatalogQuery() = default;
std::optional<Catalog> CatalogQuery::poll(std::stop_token stop) {
  return impl_->poll(stop);
}
} // namespace asterion::ctp
