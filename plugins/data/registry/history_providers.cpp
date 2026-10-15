#include "history_providers.hpp"
#include "response_evidence.hpp"
#include <asterion/plugin/history_response.h>
#include <asterion/kernel/native_plugin.hpp>
#include <asterion/plugin/history.h>
#include <asterion/plugin/history_connection.h>
#include <asterion/plugin/history_tables.h>
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
const AstHistoryV3& table(const NativeInstance& instance) {
  const auto* t =
      static_cast<const AstHistoryV3*>(instance.query(AST_HISTORY_V3, 3, sizeof(AstHistoryV3)));
  if (t->size != sizeof(AstHistoryV3) || t->version != 3 || !t->sources || !t->minutes)
    throw std::invalid_argument("invalid native history capability");
  return *t;
}
template <class T> struct Output {
  std::size_t limit = 10000;
  std::vector<T> rows;
  std::exception_ptr error;
  template <class F> AstStatus emit(F&& f) noexcept {
    if (error)
      return AST_FAILED;
    try {
      if (rows.size() >= limit)
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
            return c.id == AST_HISTORY_V3 && c.version == 3;
          }))
        continue;
      auto instance = library.create(AST_HISTORY_V3, {});
      Output<Source> out;
      const auto status = table(*instance).sources(
          instance->handle(), &out,
          [](void* context, const AstHistorySource* row) noexcept -> AstStatus {
            return static_cast<Output<Source>*>(context)->emit([&] {
              if (!row || !row->venues || !row->venue_count || row->venue_count > 64 ||
                  !row->intervals || !row->interval_count || row->interval_count > 1440 ||
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
                if (!row->intervals[i] || row->intervals[i] > 1440 ||
                    !intervals.insert(row->intervals[i]).second)
                  throw std::invalid_argument("invalid native history interval");
                s.intervals.push_back(row->intervals[i]);
              }
              if (s.semantics.timestamp_semantics == "trading_day")
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
              AST_HISTORY_V3, {{"source", id.c_str()}, {"credential", credential.c_str()}});
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
    AstHistoryQuery query{contract(range.instrument), range.source_instrument.c_str(),
                          range.interval_minutes, range.begin_ns, range.end_ns};
    // A stored segment: every minute of 31 days.
    Output<HistoricalBar> out{.limit = 31 * 1440};
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
namespace {
const AstHistoryTablesV1& tables_table(const NativeInstance& instance) {
  const auto* result = static_cast<const AstHistoryTablesV1*>(
      instance.query(AST_HISTORY_TABLES_V1, 1, sizeof(AstHistoryTablesV1)));
  if (result->size != sizeof(AstHistoryTablesV1) || result->version != 1 || !result->tables ||
      !result->read)
    throw std::invalid_argument("invalid native history table capability");
  return *result;
}
struct InstalledTables {
  NativeLibrary library;
  std::string plugin_id;
  std::vector<TableSource> tables;
};
const std::vector<InstalledTables>& table_registry() {
  static const auto all = [] {
    std::vector<InstalledTables> result;
    for (auto& library : discover_native_plugins(native_plugin_directory())) {
      const auto& descriptor = library.descriptor();
      if (std::ranges::none_of(descriptor.capabilities, [](const auto& c) {
            return c.id == AST_HISTORY_TABLES_V1 && c.version == 1;
          }))
        continue;
      auto instance = library.create(AST_HISTORY_TABLES_V1, {});
      Output<TableSource> out;
      const auto status = tables_table(*instance).tables(
          instance->handle(), &out,
          [](void* context, const AstHistoryTable* row) noexcept -> AstStatus {
            return static_cast<Output<TableSource>*>(context)->emit([&] {
              if (!row || !row->venues || !row->venue_count || row->venue_count > 64)
                throw std::invalid_argument("invalid native history table");
              TableSource table{text(row->table), row->schema, text(row->source), {}, {}};
              validate_history_source(table.source);
              std::set<std::string> venues;
              for (uint32_t i = 0; i < row->venue_count; ++i) {
                auto venue = text(row->venues[i]);
                validate_history_venue(venue);
                if (!venues.insert(venue).second)
                  throw std::invalid_argument("invalid native history table");
                table.venues.push_back(std::move(venue));
              }
              return table;
            });
          });
      InstalledTables installed{library, descriptor.id, {}};
      std::set<std::string> offered;
      for (auto& table : out.finish(status)) {
        table.plugin_id = descriptor.id;
        if (!offered.insert(table.table).second)
          throw std::invalid_argument("duplicate native history table");
        // A table this host defines otherwise, or not at all, is not offered.
        const auto known =
            std::ranges::find(history_tables(), std::string_view(table.table), &HistoryTable::id);
        if (known != history_tables().end() && known->schema == table.schema)
          installed.tables.push_back(std::move(table));
      }
      result.push_back(std::move(installed));
    }
    return result;
  }();
  return all;
}
const InstalledTables& table_provider(const std::string& plugin_id) {
  for (const auto& installed : table_registry())
    if (installed.plugin_id == plugin_id)
      return installed;
  throw std::invalid_argument("history table source is unavailable");
}
class Tables final : public TableReader {
public:
  Tables(const std::string& plugin_id, const std::string& credential, RequestBudget budget)
      : installed_(table_provider(plugin_id)), budget_(std::move(budget)) {
    if (credential.empty() || credential.size() > 256)
      throw std::invalid_argument("invalid native plugin credential");
    instance_ =
        installed_.library.create(AST_HISTORY_TABLES_V1, {{"credential", credential.c_str()}});
    instance_->start();
  }
  std::optional<SourceResponse> response() const override { return response_; }
  std::vector<TableSourceRow> read(const HistoryTable& table, const std::string& venue,
                                   const std::string& period, std::stop_token stop) override {
    response_.reset();
    const auto offered = std::ranges::find(installed_.tables, table.id, &TableSource::table);
    if (offered == installed_.tables.end() ||
        std::ranges::find(offered->venues, venue) == offered->venues.end())
      throw std::invalid_argument("history table is unavailable from this source");
    validate_table_period(table, period);
    if (stop.stop_requested())
      check_plugin_status(AST_CANCELLED);
    std::vector<std::string> names;
    std::vector<const char*> columns;
    for (const auto& column : table.columns)
      names.emplace_back(column.name);
    for (const auto& name : names)
      columns.push_back(name.c_str());
    struct Rows {
      std::size_t width;
      std::vector<TableSourceRow> rows;
      std::exception_ptr error;
    } out{columns.size(), {}, {}};
    std::string diagnostic;
    Admission admission{budget_, stop, {}};
    const std::string id(table.id);
    const auto status =
        tables_table(*instance_)
            .read(
                instance_->handle(), id.c_str(), columns.data(),
                static_cast<uint32_t>(columns.size()), venue.c_str(), period.c_str(),
                cancellation(stop), admission.callback(),
                {&diagnostic,
                 [](void* context, const char* message) {
                   // One bounded line, whatever the plugin passed.
                   auto& kept = *static_cast<std::string*>(context);
                   if (!message || !kept.empty())
                     return;
                   kept.assign(message, strnlen(message, 512));
                   for (auto& c : kept)
                     if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f)
                       c = ' ';
                 }},
                &out,
                [](void* context, const char* const* cells, uint32_t count) noexcept -> AstStatus {
                  auto& out = *static_cast<Rows*>(context);
                  if (out.error)
                    return AST_FAILED;
                  try {
                    if (!cells || count != out.width)
                      throw std::invalid_argument("invalid native history table row");
                    if (out.rows.size() >= max_table_partition_rows)
                      throw Error(ErrorCode::resource_exhausted,
                                  "native plugin resource limit exceeded");
                    auto& row = out.rows.emplace_back();
                    row.reserve(count);
                    for (uint32_t i = 0; i < count; ++i)
                      row.push_back(cells[i] ? std::optional(text(cells[i])) : std::nullopt);
                    return AST_OK;
                  } catch (...) {
                    out.error = std::current_exception();
                    return AST_FAILED;
                  }
                });
    admission.finish();
    if (out.error)
      std::rethrow_exception(out.error);
    if (stop.stop_requested())
      check_plugin_status(AST_CANCELLED);
    if (status == AST_INVALID)
      throw TableRejected(!diagnostic.empty() ? diagnostic
                                              : id + " " + venue + " " + period +
                                                    ": the source could not use its data");
    try {
      check_plugin_status(status);
    } catch (const Error& error) {
      if (diagnostic.empty())
        throw;
      throw Error(error.code(), diagnostic);
    }
    const auto& caps = installed_.library.descriptor().capabilities;
    if (std::ranges::any_of(caps, [](const auto& cap) {
          return cap.id == AST_HISTORY_RESPONSE_V1 && cap.version == 1;
        })) {
      const auto* api = static_cast<const AstHistoryResponseV1*>(
          instance_->query(AST_HISTORY_RESPONSE_V1, 1, sizeof(AstHistoryResponseV1)));
      if (!api || api->size != sizeof(AstHistoryResponseV1) || api->version != 1 || !api->get)
        throw std::invalid_argument("invalid source response evidence");
      struct Observation {
        std::optional<SourceResponse> value;
        std::exception_ptr error;
      } observation;
      const auto result = api->get(
          instance_->handle(), &observation,
          [](void* context, const char* format, const uint8_t* bytes, uint32_t count,
             int64_t received) noexcept -> AstStatus {
            auto& observed = *static_cast<Observation*>(context);
            try {
              if (observed.value || !bytes || count == 0 || count > max_source_response_bytes ||
                  received <= 0)
                throw std::invalid_argument("invalid source response evidence");
              observed.value = SourceResponse{
                  text(format), std::string(reinterpret_cast<const char*>(bytes), count), received};
              return AST_OK;
            } catch (...) {
              observed.error = std::current_exception();
              return AST_INVALID;
            }
          });
      if (observation.error)
        std::rethrow_exception(observation.error);
      check_plugin_status(result);
      if (observation.value)
        validate_response(id, venue, period, *observation.value);
      response_ = std::move(observation.value);
    }
    return std::move(out.rows);
  }

private:
  std::optional<SourceResponse> response_;
  const InstalledTables& installed_;
  RequestBudget budget_;
  std::unique_ptr<NativeInstance> instance_;
};
} // namespace
std::vector<TableSource> table_sources() {
  std::vector<TableSource> result;
  for (const auto& installed : table_registry())
    result.insert(result.end(), installed.tables.begin(), installed.tables.end());
  return result;
}
std::string table_artifact(const std::string& plugin_id) {
  return table_provider(plugin_id).library.sha256();
}
void validate_response(const std::string& table, const std::string& venue,
                       const std::string& period, const SourceResponse& response) {
  if (table != "futures.daily" || response.format != tushare::daily_response_format ||
      response.received_at_ns <= 0 || !tushare::allow_daily_response(response.body, venue, period))
    throw std::invalid_argument("invalid source response evidence");
}
std::unique_ptr<TableReader> tables(const std::string& plugin_id, const std::string& credential,
                                    RequestBudget budget) {
  return std::make_unique<Tables>(plugin_id, credential, std::move(budget));
}
} // namespace asterion::history_providers
