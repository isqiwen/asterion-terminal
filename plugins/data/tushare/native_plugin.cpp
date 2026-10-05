#include "tushare.hpp"
#include <asterion/plugin/history.h>
#include <asterion/plugin/history_connection.h>
#include <asterion/plugin/sdk.hpp>
#include <algorithm>
#include <thread>
#include <condition_variable>
using namespace asterion;
namespace {
struct Instance {
  std::string source, credential;
  std::unique_ptr<tushare::Minutes> minutes;
  std::unique_ptr<tushare::Daily> daily;
  bool running = false;
  AstRequestBudget budget{};
};
struct BudgetScope {
  Instance& instance;
  BudgetScope(Instance& instance, AstRequestBudget budget) : instance(instance) {
    if (!budget.acquire)
      throw AstStatus(AST_INVALID);
    instance.budget = budget;
  }
  ~BudgetScope() { instance.budget = {}; }
};
struct Cancellation {
  std::stop_source source;
  std::jthread thread;
  explicit Cancellation(AstCancellation external) {
    if (!external.requested)
      return;
    if (external.requested(external.context))
      source.request_stop();
    thread = std::jthread([this, external](std::stop_token done) {
      while (!done.stop_requested() && !source.stop_requested()) {
        if (external.requested(external.context)) {
          source.request_stop();
          return;
        }
        std::mutex mutex;
        std::condition_variable_any cv;
        std::unique_lock lock(mutex);
        cv.wait_for(lock, done, std::chrono::milliseconds(10), [] { return false; });
      }
    });
  }
};
void active(Instance* self) {
  if (!self || !self->running)
    throw AstStatus(AST_UNAVAILABLE);
}
AstStatus sources(void*, void* context,
                  AstStatus (*emit)(void*, const AstHistorySource*)) noexcept {
  return sdk::boundary([&] {
    if (!emit)
      throw AstStatus(AST_INVALID);
    static const char* venues[] = {"SHFE", "DCE", "CZCE", "CFFEX", "INE", "GFEX"};
    static const uint32_t minutes[] = {1, 5, 15, 30, 60}, daily[] = {0};
    const AstHistorySource m{"tushare.ft_mins",
                             "Tushare",
                             "tushare.minutes.v3",
                             "Asia/Shanghai",
                             "bar_end",
                             venues,
                             6,
                             minutes,
                             5,
                             500,
                             1};
    const AstHistorySource d{"tushare.fut_daily",
                             "Tushare",
                             "tushare.daily.v2",
                             "Asia/Shanghai",
                             "trading_day",
                             venues,
                             6,
                             daily,
                             1,
                             500,
                             1};
    sdk::check(emit(context, &m));
    sdk::check(emit(context, &d));
  });
}
AstStatus catalog(void* object, const char* venue, const char* product, AstCancellation cancel,
                  void* context, AstStatus (*emit)(void*, const AstListing*)) noexcept {
  return sdk::boundary([&] {
    if (!venue || !product || !emit)
      throw AstStatus(AST_INVALID);
    auto* self = static_cast<Instance*>(object);
    active(self);
    Cancellation cancellation(cancel);
    std::string code(product);
    std::ranges::transform(code, code.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    auto date = [](const std::string& value) {
      return value.substr(0, 4) + "-" + value.substr(4, 2) + "-" + value.substr(6, 2);
    };
    const auto rows =
        tushare::contracts(self->credential, venue, code, cancellation.source.get_token());
    for (const auto& item : rows) {
      if (cancellation.source.stop_requested())
        throw AstStatus(AST_CANCELLED);
      const auto begin = date(item.list_date), end = date(item.delist_date);
      AstListing row{{item.identity.venue.c_str(), item.identity.product.c_str(),
                      item.identity.delivery_month.c_str()},
                     item.name.c_str(),
                     begin.c_str(),
                     end.c_str(),
                     item.ts_code.c_str(),
                     item.multiplier ? 1u : 0u,
                     item.per_unit ? 1u : 0u,
                     item.multiplier ? item.multiplier->raw() : 0,
                     item.per_unit ? item.per_unit->raw() : 0,
                     item.trade_unit ? item.trade_unit->c_str() : nullptr,
                     item.quote_unit ? item.quote_unit->c_str() : nullptr};
      sdk::check(emit(context, &row));
    }
  });
}
HistoryIdentity identity(const AstContract& c) {
  if (!c.venue || !c.product || !c.delivery_month)
    throw AstStatus(AST_INVALID);
  HistoryIdentity result{c.venue, c.product, c.delivery_month};
  result.validate();
  return result;
}
AstStatus minutes(void* object, const AstHistoryQuery* query, AstCancellation cancel,
                  AstRequestBudget budget, void* context,
                  AstStatus (*emit)(void*, const AstMinute*)) noexcept {
  return sdk::boundary([&] {
    if (!query || !query->source_instrument || !emit)
      throw AstStatus(AST_INVALID);
    auto* self = static_cast<Instance*>(object);
    active(self);
    if (!self->minutes)
      throw AstStatus(AST_UNSUPPORTED);
    BudgetScope admission(*self, budget);
    Cancellation cancellation(cancel);
    const auto rows =
        self->minutes->read({identity(query->contract), query->interval_minutes, query->begin_ns,
                             query->end_ns, self->source, query->source_instrument},
                            cancellation.source.get_token());
    for (const auto& item : rows) {
      if (cancellation.source.stop_requested())
        throw AstStatus(AST_CANCELLED);
      AstMinute row{item.timestamp_ns,
                    item.open.raw(),
                    item.high.raw(),
                    item.low.raw(),
                    item.close.raw(),
                    item.volume.raw(),
                    item.amount.raw(),
                    item.open_interest.raw(),
                    item.trading_day.empty() ? nullptr : item.trading_day.c_str()};
      sdk::check(emit(context, &row));
    }
  });
}
AstStatus daily(void* object, const AstHistoryQuery* query, AstCancellation cancel,
                AstRequestBudget budget, void* context,
                AstStatus (*emit)(void*, const AstDaily*)) noexcept {
  return sdk::boundary([&] {
    if (!query || !query->source_instrument || !query->begin_day || !query->end_day || !emit)
      throw AstStatus(AST_INVALID);
    auto* self = static_cast<Instance*>(object);
    active(self);
    if (!self->daily)
      throw AstStatus(AST_UNSUPPORTED);
    BudgetScope admission(*self, budget);
    Cancellation cancellation(cancel);
    const auto rows = self->daily->read(
        {identity(query->contract), parse_trading_date(query->begin_day),
         parse_trading_date(query->end_day), self->source, query->source_instrument},
        cancellation.source.get_token());
    for (const auto& item : rows) {
      if (cancellation.source.stop_requested())
        throw AstStatus(AST_CANCELLED);
      const auto day = format_trading_date(item.trading_day);
      AstDaily row{day.c_str(),
                   item.open.raw(),
                   item.high.raw(),
                   item.low.raw(),
                   item.close.raw(),
                   item.volume.raw(),
                   item.amount.raw(),
                   item.open_interest.raw(),
                   item.previous_close ? 1u : 0u,
                   item.previous_settlement ? 1u : 0u,
                   item.settlement ? 1u : 0u,
                   item.previous_close ? item.previous_close->raw() : 0,
                   item.previous_settlement ? item.previous_settlement->raw() : 0,
                   item.settlement ? item.settlement->raw() : 0};
      sdk::check(emit(context, &row));
    }
  });
}
AstStatus describe_connection(void*, const char* source, void* context,
                              AstStatus (*emit)(void*,
                                                const AstHistoryConnectionSchema*)) noexcept {
  return sdk::boundary([&] {
    if (!source || !emit ||
        (std::string_view(source) != "tushare.ft_mins" &&
         std::string_view(source) != "tushare.fut_daily"))
      throw AstStatus(AST_INVALID);
    const AstHistoryConnectionSchema schema{"Tushare Token", "Tushare Token", 1, 256, 1, 60, 500};
    sdk::check(emit(context, &schema));
  });
}
AstStatus verify_connection(void*, const char* source, const char* credential,
                            AstCancellation cancel, void* context,
                            AstStatus (*emit)(void*, const AstConnectionCheck*)) noexcept {
  return sdk::boundary([&] {
    if (!source || !credential || !emit ||
        (std::string_view(source) != "tushare.ft_mins" &&
         std::string_view(source) != "tushare.fut_daily"))
      throw AstStatus(AST_INVALID);
    Cancellation cancellation(cancel);
    uint32_t catalog_state = AST_CHECK_NOT_CHECKED, history_state = AST_CHECK_NOT_CHECKED;
    auto check = [&](auto&& operation) -> uint32_t {
      try {
        operation();
        return AST_CHECK_VERIFIED;
      } catch (const tushare::RequestError& error) {
        return static_cast<uint32_t>(error.reason);
      } catch (const std::invalid_argument&) {
        return AST_CHECK_FAILED;
      } catch (...) {
        return AST_CHECK_FAILED;
      }
    };
    const std::string token(credential);
    if (token.empty() || token.size() > 256 || token.find_first_of(" \r\n\t") != std::string::npos)
      catalog_state = AST_CHECK_INVALID_CREDENTIAL;
    else {
      std::vector<tushare::FuturesListing> contracts;
      catalog_state = check([&] {
        contracts = tushare::contracts(token, "SHFE", "CU", cancellation.source.get_token());
      });
      if (catalog_state == AST_CHECK_VERIFIED) {
        const auto cutoff =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch() - std::chrono::hours(24))
                .count();
        auto day = tushare::format_time(cutoff).substr(0, 10);
        auto compact = day;
        std::erase(compact, '-');
        std::erase_if(contracts,
                      [&](const auto& contract) { return contract.list_date > compact; });
        std::ranges::sort(contracts, [](const auto& left, const auto& right) {
          return left.delist_date > right.delist_date;
        });
        if (!contracts.empty()) {
          const auto& contract = contracts.front();
          if (contract.delist_date < compact)
            day = contract.delist_date.substr(0, 4) + "-" + contract.delist_date.substr(4, 2) +
                  "-" + contract.delist_date.substr(6, 2);
          history_state = check([&] {
            if (std::string_view(source) == "tushare.fut_daily") {
              tushare::Daily provider(token);
              provider.start();
              (void)provider.read({contract.identity, parse_trading_date(day),
                                   parse_trading_date(day), source, contract.ts_code},
                                  cancellation.source.get_token());
            } else {
              tushare::Minutes provider(token);
              provider.start();
              const auto begin = tushare::parse_time(day + " 09:00:00");
              (void)provider.read(
                  {contract.identity, 1, begin, begin + 60000000000LL, source, contract.ts_code},
                  cancellation.source.get_token());
            }
          });
        }
      }
    }
    if (cancellation.source.stop_requested())
      throw AstStatus(AST_CANCELLED);
    const AstConnectionCheck catalog{"catalog", catalog_state}, history{"history", history_state};
    sdk::check(emit(context, &catalog));
    sdk::check(emit(context, &history));
  });
}
const AstHistoryConnectionV1 connection{sizeof(AstHistoryConnectionV1), 1, describe_connection,
                                        verify_connection};
const AstHistoryV2 history{sizeof(AstHistoryV2), 2, sources, catalog, minutes, daily};
AstStatus create(const char* capability, const AstSetting* settings, uint32_t count,
                 void** out) noexcept {
  if (!out)
    return AST_INVALID;
  *out = nullptr;
  return sdk::boundary([&] {
    if (!capability || (std::string_view(capability) != AST_HISTORY_V2 &&
                        std::string_view(capability) != AST_HISTORY_CONNECTION_V1))
      throw AstStatus(AST_UNSUPPORTED);
    if (count > 64 || (count && !settings))
      throw AstStatus(AST_INVALID);
    for (uint32_t i = 0; i < count; ++i)
      if (!settings[i].key || !settings[i].value ||
          (std::string_view(settings[i].key) != "source" &&
           std::string_view(settings[i].key) != "credential"))
        throw AstStatus(AST_INVALID);
    auto result = std::make_unique<Instance>();
    result->source = sdk::setting(settings, count, "source");
    result->credential = sdk::setting(settings, count, "credential");
    if (!result->source.empty() && result->source != "tushare.ft_mins" &&
        result->source != "tushare.fut_daily")
      throw AstStatus(AST_UNSUPPORTED);
    auto post = [instance = result.get(), transport = tushare::https_transport()](
                    const std::string& body, std::stop_token stop) {
      sdk::check(instance->budget.acquire(instance->budget.context));
      if (stop.stop_requested())
        throw AstStatus(AST_CANCELLED);
      return transport(body, stop);
    };
    if (result->source == "tushare.ft_mins")
      result->minutes = std::make_unique<tushare::Minutes>(result->credential, post);
    if (result->source == "tushare.fut_daily")
      result->daily = std::make_unique<tushare::Daily>(result->credential, post);
    *out = result.release();
  });
}
AstStatus start(void* object) noexcept {
  return sdk::boundary([&] {
    auto* self = static_cast<Instance*>(object);
    if (!self)
      throw AstStatus(AST_INVALID);
    if (self->minutes)
      self->minutes->start();
    if (self->daily)
      self->daily->start();
    self->running = true;
  });
}
void stop(void* object) noexcept {
  auto* self = static_cast<Instance*>(object);
  if (!self)
    return;
  if (self->minutes)
    self->minutes->stop();
  if (self->daily)
    self->daily->stop();
  self->running = false;
}
void destroy(void* object) noexcept {
  delete static_cast<Instance*>(object);
}
AstStatus query(void*, const char* id, uint32_t version, uint32_t size, const void** out) noexcept {
  if (!out)
    return AST_INVALID;
  *out = nullptr;
  if (id && std::string_view(id) == AST_HISTORY_CONNECTION_V1 && version == 1 &&
      size == sizeof(AstHistoryConnectionV1)) {
    *out = &connection;
    return AST_OK;
  }
  if (!id || std::string_view(id) != AST_HISTORY_V2 || version != 2 || size != sizeof(AstHistoryV2))
    return AST_UNSUPPORTED;
  *out = &history;
  return AST_OK;
}
const AstCapability capabilities[] = {{AST_HISTORY_V2, 2, "data"},
                                      {AST_HISTORY_CONNECTION_V1, 1, "data"}};
const AstPluginV1 plugin{sizeof(AstPluginV1),
                         ASTERION_PLUGIN_ABI_VERSION,
                         "asterion.data.tushare",
                         "1.0.0",
                         ASTERION_PLUGIN_PLATFORM,
                         capabilities,
                         2,
                         create,
                         start,
                         stop,
                         destroy,
                         query};
} // namespace
extern "C" ASTERION_PLUGIN_EXPORT const AstPluginV1*
asterion_plugin_entry_v1(uint32_t abi, uint32_t size) noexcept {
  return abi == ASTERION_PLUGIN_ABI_VERSION && size == sizeof(AstPluginV1) ? &plugin : nullptr;
}
