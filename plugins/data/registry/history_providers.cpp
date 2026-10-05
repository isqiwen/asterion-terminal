#include "history_providers.hpp"
#include <asterion/kernel/native_plugin.hpp>
#include <asterion/plugin/history.h>
#include <asterion/plugin/history_connection.h>
#include <algorithm>
#include <asterion/foundation/error.hpp>
#include <cstring>
#include <set>
#include <stdexcept>
namespace asterion::history_providers {
namespace {
std::string text(const char* s) {
  if (!s || strnlen(s, 4097) > 4096)
    throw std::invalid_argument("invalid native history text");
  return s;
}
const AstHistoryV2& table(const NativeInstance& instance) {
  const auto* t =
      static_cast<const AstHistoryV2*>(instance.query(AST_HISTORY_V2, 2, sizeof(AstHistoryV2)));
  if (t->size != sizeof(AstHistoryV2) || t->version != 2 || !t->sources || !t->catalog ||
      !t->minutes || !t->daily)
    throw std::invalid_argument("invalid native history capability");
  return *t;
}
template <class T> struct Output {
  std::vector<T> rows;
  std::exception_ptr error;
  template <class F> AstStatus emit(F&& f) noexcept {
    if (error)
      return AST_FAILED;
    try {
      if (rows.size() >= 10000)
        throw Error(ErrorCode::resource_exhausted, "native plugin resource limit exceeded");
      rows.push_back(f());
      return AST_OK;
    } catch (...) {
      error = std::current_exception();
      return AST_FAILED;
    }
  }
  std::vector<T> finish(AstStatus status, std::stop_token stop = {}) {
    if (error)
      std::rethrow_exception(error);
    if (stop.stop_requested())
      check_plugin_status(AST_CANCELLED);
    check_plugin_status(status);
    return std::move(rows);
  }
};
const AstHistoryConnectionV1& connection_table(const NativeInstance& instance) {
  const auto* result = static_cast<const AstHistoryConnectionV1*>(
      instance.query(AST_HISTORY_CONNECTION_V1, 1, sizeof(AstHistoryConnectionV1)));
  if (result->size != sizeof(AstHistoryConnectionV1) || result->version != 1 || !result->describe ||
      !result->verify)
    throw std::invalid_argument("invalid history connection capability");
  return *result;
}
ConnectionSchema connection_schema(NativeLibrary& library, const std::string& source) {
  auto instance = library.create(AST_HISTORY_CONNECTION_V1, {});
  Output<ConnectionSchema> output;
  const auto status = connection_table(*instance).describe(
      instance->handle(), source.c_str(), &output,
      [](void* context, const AstHistoryConnectionSchema* schema) noexcept -> AstStatus {
        return static_cast<Output<ConnectionSchema>*>(context)->emit([&] {
          if (!schema || schema->credential_required > 1 || schema->remember_allowed > 1 ||
              !schema->credential_max_length || schema->credential_max_length > 256 ||
              !schema->requests_per_minute_default || schema->requests_per_minute_max > 500 ||
              schema->requests_per_minute_default > schema->requests_per_minute_max)
            throw std::invalid_argument("invalid history connection schema");
          return ConnectionSchema{
              text(schema->credential_label_en), text(schema->credential_label_zh),
              schema->credential_required != 0,  schema->remember_allowed != 0,
              schema->credential_max_length,     schema->requests_per_minute_default,
              schema->requests_per_minute_max};
        });
      });
  auto schemas = output.finish(status);
  if (schemas.size() != 1)
    throw std::invalid_argument("invalid history connection schema");
  return schemas.front();
}
struct Installed {
  NativeLibrary library;
  std::vector<Source> sources;
};
const std::vector<Installed>& registry() {
  static const auto all = [] {
    std::vector<Installed> result;
    std::set<std::string> sources;
    for (auto& library : discover_native_plugins(native_plugin_directory())) {
      const auto& descriptor = library.descriptor();
      if (std::ranges::none_of(descriptor.capabilities, [](const auto& c) {
            return c.id == AST_HISTORY_V2 && c.version == 2;
          }))
        continue;
      auto instance = library.create(AST_HISTORY_V2, {});
      Output<Source> out;
      const auto status = table(*instance).sources(
          instance->handle(), &out,
          [](void* context, const AstHistorySource* row) noexcept -> AstStatus {
            return static_cast<Output<Source>*>(context)->emit([&] {
              if (!row || !row->venues || !row->venue_count || row->venue_count > 64 ||
                  !row->intervals || !row->interval_count || row->interval_count > 1441 ||
                  !row->max_requests_per_minute || row->max_requests_per_minute > 500 ||
                  row->credential_required > 1)
                throw std::invalid_argument("invalid native history source");
              Source s{text(row->source),
                       text(row->name),
                       {},
                       {text(row->source), text(row->normalization), text(row->timezone),
                        text(row->timestamp_semantics)},
                       {},
                       {},
                       row->max_requests_per_minute,
                       row->credential_required != 0,
                       {}};
              s.semantics.validate();
              if (s.name.empty())
                throw std::invalid_argument("invalid native history source");
              std::set<std::string> venues;
              std::set<unsigned> intervals;
              for (uint32_t i = 0; i < row->venue_count; ++i) {
                auto venue = text(row->venues[i]);
                HistoryIdentity{venue, "test", "2026-01"}.validate();
                if (!venues.insert(venue).second)
                  throw std::invalid_argument("invalid native history source");
                s.venues.push_back(venue);
              }
              for (uint32_t i = 0; i < row->interval_count; ++i) {
                if (row->intervals[i] > 1440 || !intervals.insert(row->intervals[i]).second)
                  throw std::invalid_argument("invalid native history interval");
                s.intervals.push_back(row->intervals[i]);
              }
              const bool daily = intervals.contains(0);
              if ((daily && intervals.size() != 1) ||
                  daily != (s.semantics.timestamp_semantics == "trading_day"))
                throw std::invalid_argument("invalid native history source");
              return s;
            });
          });
      auto declared = out.finish(status);
      for (auto& source : declared) {
        source.plugin_id = descriptor.id;
        if (std::ranges::any_of(descriptor.capabilities, [](const auto& capability) {
              return capability.id == AST_HISTORY_CONNECTION_V1 && capability.version == 1;
            })) {
          source.connection = connection_schema(library, source.id);
          if (source.connection->credential_required != source.credential_required ||
              source.connection->requests_per_minute_max > source.max_requests_per_minute)
            throw std::invalid_argument("invalid history connection schema");
        }
        if (!sources.insert(source.id).second)
          throw std::invalid_argument("duplicate native history source");
      }
      result.push_back({std::move(library), std::move(declared)});
    }
    return result;
  }();
  return all;
}
struct Provider {
  Source source;
  std::unique_ptr<NativeInstance> instance;
  Provider(const std::string& id, const std::string& credential) {
    for (const auto& installed : registry())
      for (const auto& item : installed.sources)
        if (item.id == id) {
          if (credential.size() > 256 || (item.credential_required && credential.empty()))
            throw std::invalid_argument("invalid native plugin credential");
          source = item;
          instance = installed.library.create(
              AST_HISTORY_V2, {{"source", id.c_str()}, {"credential", credential.c_str()}});
          return;
        }
    throw std::invalid_argument("historical data source is unavailable");
  }
  void interval(unsigned value) const {
    if (std::ranges::find(source.intervals, value) == source.intervals.end())
      throw std::invalid_argument("historical source interval is unavailable");
  }
};
AstCancellation cancellation(std::stop_token& stop) {
  return {&stop, [](void* c) noexcept -> int32_t {
            return static_cast<std::stop_token*>(c)->stop_requested() ? 1 : 0;
          }};
}
AstContract contract(const HistoryIdentity& value) {
  return {value.venue.c_str(), value.product.c_str(), value.delivery_month.c_str()};
}
struct Admission {
  const RequestBudget& acquire;
  std::stop_token stop;
  std::exception_ptr error;
  AstRequestBudget callback() {
    return {this, [](void* context) noexcept -> AstStatus {
              auto& self = *static_cast<Admission*>(context);
              if (self.error)
                return AST_FAILED;
              try {
                if (self.stop.stop_requested())
                  check_plugin_status(AST_CANCELLED);
                if (!self.acquire)
                  throw std::invalid_argument("download request budget is not configured");
                self.acquire(self.stop);
                return AST_OK;
              } catch (...) {
                self.error = std::current_exception();
                return AST_FAILED;
              }
            }};
  }
  void finish() const {
    if (error)
      std::rethrow_exception(error);
  }
};
class Minutes final : public HistoricalBarPort {
  Provider provider_;
  RequestBudget budget_;

public:
  Minutes(const std::string& source, const std::string& credential, RequestBudget budget)
      : provider_(source, credential), budget_(std::move(budget)) {
    if (provider_.source.intervals.front() == 0)
      throw std::invalid_argument("historical source interval is unavailable");
    provider_.instance->start();
  }
  HistorySemantics semantics() const override { return provider_.source.semantics; }
  std::vector<HistoricalBar> read(const HistoricalBarRange& range, std::stop_token stop) override {
    range.instrument.validate();
    provider_.interval(range.interval_minutes);
    if (range.begin_ns <= 0 || range.end_ns < range.begin_ns || range.source_instrument.empty())
      throw std::invalid_argument("invalid native history query");
    if (range.source != provider_.source.id || stop.stop_requested())
      check_plugin_status(stop.stop_requested() ? AST_CANCELLED : AST_INVALID);
    AstHistoryQuery query{contract(range.instrument),
                          range.source_instrument.c_str(),
                          range.interval_minutes,
                          range.begin_ns,
                          range.end_ns,
                          nullptr,
                          nullptr};
    Output<HistoricalBar> out;
    const auto& instance = *provider_.instance;
    Admission admission{budget_, stop, {}};
    const auto status =
        table(instance).minutes(instance.handle(), &query, cancellation(stop), admission.callback(),
                                &out, [](void* c, const AstMinute* row) noexcept -> AstStatus {
                                  return static_cast<Output<HistoricalBar>*>(c)->emit([&] {
                                    if (!row)
                                      throw std::invalid_argument("invalid native history row");
                                    HistoricalBar b{row->timestamp_ns,
                                                    Decimal::from_raw(row->open),
                                                    Decimal::from_raw(row->high),
                                                    Decimal::from_raw(row->low),
                                                    Decimal::from_raw(row->close),
                                                    Decimal::from_raw(row->volume),
                                                    Decimal::from_raw(row->amount),
                                                    Decimal::from_raw(row->open_interest),
                                                    row->trading_day ? text(row->trading_day) : ""};
                                    b.validate();
                                    return b;
                                  });
                                });
    admission.finish();
    auto rows = out.finish(status, stop);
    int64_t previous = 0;
    for (const auto& row : rows) {
      if (row.timestamp_ns < range.begin_ns || row.timestamp_ns > range.end_ns ||
          row.timestamp_ns <= previous)
        throw std::invalid_argument("invalid native history row");
      previous = row.timestamp_ns;
    }
    return rows;
  }
};
class Daily final : public HistoricalDailyPort {
  Provider provider_;
  RequestBudget budget_;

public:
  Daily(const std::string& source, const std::string& credential, RequestBudget budget)
      : provider_(source, credential), budget_(std::move(budget)) {
    provider_.interval(0);
    provider_.instance->start();
  }
  HistorySemantics semantics() const override { return provider_.source.semantics; }
  std::vector<HistoricalDailyBar> read(const HistoricalDailyRange& range,
                                       std::stop_token stop) override {
    range.instrument.validate();
    if (range.end < range.begin || range.source_instrument.empty())
      throw std::invalid_argument("invalid native history query");
    if (range.source != provider_.source.id || stop.stop_requested())
      check_plugin_status(stop.stop_requested() ? AST_CANCELLED : AST_INVALID);
    auto begin = format_trading_date(range.begin), end = format_trading_date(range.end);
    AstHistoryQuery query{contract(range.instrument),
                          range.source_instrument.c_str(),
                          0,
                          0,
                          0,
                          begin.c_str(),
                          end.c_str()};
    Output<HistoricalDailyBar> out;
    const auto& instance = *provider_.instance;
    Admission admission{budget_, stop, {}};
    const auto status = table(instance).daily(
        instance.handle(), &query, cancellation(stop), admission.callback(), &out,
        [](void* c, const AstDaily* row) noexcept -> AstStatus {
          return static_cast<Output<HistoricalDailyBar>*>(c)->emit([&] {
            if (!row || row->has_previous_close > 1 || row->has_previous_settlement > 1 ||
                row->has_settlement > 1)
              throw std::invalid_argument("invalid native history row");
            auto optional = [](uint32_t present, int64_t value) -> std::optional<Decimal> {
              return present ? std::optional(Decimal::from_raw(value)) : std::nullopt;
            };
            HistoricalDailyBar b{parse_trading_date(text(row->trading_day)),
                                 Decimal::from_raw(row->open),
                                 Decimal::from_raw(row->high),
                                 Decimal::from_raw(row->low),
                                 Decimal::from_raw(row->close),
                                 Decimal::from_raw(row->volume),
                                 Decimal::from_raw(row->amount),
                                 Decimal::from_raw(row->open_interest),
                                 optional(row->has_previous_close, row->previous_close),
                                 optional(row->has_previous_settlement, row->previous_settlement),
                                 optional(row->has_settlement, row->settlement)};
            b.validate();
            return b;
          });
        });
    admission.finish();
    auto rows = out.finish(status, stop);
    std::optional<std::chrono::year_month_day> previous;
    for (const auto& row : rows) {
      if (row.trading_day < range.begin || row.trading_day > range.end ||
          (previous && row.trading_day <= *previous))
        throw std::invalid_argument("invalid native history row");
      previous = row.trading_day;
    }
    return rows;
  }
};
} // namespace
std::vector<ConnectionCheck>
verify_connection(const std::string& source, const std::string& credential, std::stop_token stop) {
  for (const auto& installed : registry())
    for (const auto& item : installed.sources) {
      if (item.id != source)
        continue;
      if (!item.connection)
        throw std::invalid_argument("history connection verification is unsupported");
      const auto& schema = *item.connection;
      if (credential.size() > schema.credential_max_length ||
          (schema.credential_required && credential.empty()))
        throw std::invalid_argument("invalid native plugin credential");
      auto instance = installed.library.create(AST_HISTORY_CONNECTION_V1, {});
      instance->start();
      Output<ConnectionCheck> output;
      const auto status = connection_table(*instance).verify(
          instance->handle(), source.c_str(), credential.c_str(), cancellation(stop), &output,
          [](void* context, const AstConnectionCheck* check) noexcept -> AstStatus {
            return static_cast<Output<ConnectionCheck>*>(context)->emit([&] {
              if (!check || check->state > AST_CHECK_FAILED ||
                  (text(check->scope) != "catalog" && text(check->scope) != "history"))
                throw std::invalid_argument("invalid history connection verification");
              return ConnectionCheck{text(check->scope), check->state};
            });
          });
      auto checks = output.finish(status, stop);
      if (checks.size() != 2 || checks[0].scope != "catalog" || checks[1].scope != "history")
        throw std::invalid_argument("invalid history connection verification");
      return checks;
    }
  throw std::invalid_argument("historical data source is unavailable");
}
std::string artifact(const std::string& source) {
  for (const auto& installed : registry())
    for (const auto& item : installed.sources)
      if (item.id == source)
        return installed.library.sha256();
  throw std::invalid_argument("historical data source is unavailable");
}
std::vector<Source> sources() {
  std::vector<Source> result;
  for (const auto& installed : registry())
    result.insert(result.end(), installed.sources.begin(), installed.sources.end());
  return result;
}
void validate_request(const std::string& source, const HistoryIdentity& identity, unsigned interval,
                      unsigned rpm) {
  identity.validate();
  for (const auto& installed : registry())
    for (const auto& item : installed.sources)
      if (item.id == source) {
        if (!rpm || rpm > item.max_requests_per_minute ||
            std::ranges::find(item.venues, identity.venue) == item.venues.end() ||
            std::ranges::find(item.intervals, interval) == item.intervals.end())
          throw std::invalid_argument("historical request exceeds source capabilities");
        return;
      }
  throw std::invalid_argument("historical data source is unavailable");
}
std::unique_ptr<HistoricalBarPort> minutes(const std::string& source, const std::string& credential,
                                           RequestBudget budget) {
  return std::make_unique<Minutes>(source, credential, std::move(budget));
}
std::unique_ptr<HistoricalDailyPort> daily(const std::string& source, const std::string& credential,
                                           RequestBudget budget) {
  return std::make_unique<Daily>(source, credential, std::move(budget));
}
std::vector<HistoryListing> catalog(const std::string& source, const std::string& credential,
                                    const std::string& venue, const std::string& product,
                                    std::stop_token stop) {
  auto canonical_product = product;
  std::ranges::transform(canonical_product, canonical_product.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  HistoryIdentity{venue, canonical_product, "2026-01"}.validate();
  Provider provider(source, credential);
  if (std::ranges::find(provider.source.venues, venue) == provider.source.venues.end())
    throw std::invalid_argument("historical request exceeds source capabilities");
  provider.instance->start();
  Output<HistoryListing> out;
  const auto& instance = *provider.instance;
  const auto status = table(instance).catalog(
      instance.handle(), venue.c_str(), canonical_product.c_str(), cancellation(stop), &out,
      [](void* c, const AstListing* row) noexcept -> AstStatus {
        return static_cast<Output<HistoryListing>*>(c)->emit([&] {
          if (!row || row->has_multiplier > 1 || row->has_per_unit > 1)
            throw std::invalid_argument("invalid native history listing");
          HistoryListing item{{text(row->contract.venue), text(row->contract.product),
                               text(row->contract.delivery_month)},
                              text(row->name),
                              text(row->list_date),
                              text(row->delist_date),
                              text(row->source_instrument)};
          item.identity.validate();
          if (parse_trading_date(item.list_date) > parse_trading_date(item.delist_date) ||
              item.source_instrument.empty())
            throw std::invalid_argument("invalid native history listing");
          if (row->has_multiplier)
            item.multiplier = Decimal::from_raw(row->multiplier);
          if (row->has_per_unit)
            item.per_unit = Decimal::from_raw(row->per_unit);
          if (row->trade_unit)
            item.trade_unit = text(row->trade_unit);
          if (row->quote_unit)
            item.quote_unit = text(row->quote_unit);
          if ((item.multiplier && *item.multiplier <= Decimal{}) ||
              (item.per_unit && *item.per_unit <= Decimal{}))
            throw std::invalid_argument("invalid native history listing");
          return item;
        });
      });
  auto result = out.finish(status, stop);
  std::set<HistoryIdentity> identities;
  for (const auto& item : result)
    if (item.identity.venue != venue || item.identity.product != canonical_product ||
        !identities.insert(item.identity).second)
      throw std::invalid_argument("invalid native history listing");
  return result;
}
} // namespace asterion::history_providers
