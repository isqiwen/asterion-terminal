#ifndef ASTERION_PLUGIN_HISTORY_H
#define ASTERION_PLUGIN_HISTORY_H
#include "abi.h"
#ifdef __cplusplus
extern "C" {
#endif
#define AST_HISTORY_V2 "asterion.history.v2"
/* Fixed-point Decimal uses signed int64 units, scale 10^8, never double.
 * Date YYYY-MM-DD; delivery month YYYY-MM; UTC nanoseconds for minute labels. */
typedef struct {
  const char *venue, *product, *delivery_month;
} AstContract;
typedef struct {
  const char *source, *name, *normalization, *timezone, *timestamp_semantics;
  const char* const* venues;
  uint32_t venue_count;
  const uint32_t* intervals;
  uint32_t interval_count; /* 0 = daily */
  uint32_t max_requests_per_minute;
  uint32_t credential_required;
} AstHistorySource;
typedef struct {
  AstContract contract;
  const char *name, *list_date, *delist_date, *source_instrument;
  uint32_t has_multiplier, has_per_unit;
  int64_t multiplier, per_unit;
  const char *trade_unit, *quote_unit; /* NULL = missing */
} AstListing;
typedef struct {
  int64_t timestamp_ns, open, high, low, close, volume, amount, open_interest;
  const char* trading_day; /* NULL = unknown; must not infer night-session dates */
} AstMinute;
typedef struct {
  const char* trading_day;
  int64_t open, high, low, close, volume, amount, open_interest;
  uint32_t has_previous_close, has_previous_settlement, has_settlement;
  int64_t previous_close, previous_settlement, settlement;
} AstDaily;
typedef struct {
  AstContract contract;
  const char* source_instrument;
  uint32_t interval_minutes;
  int64_t begin_ns, end_ns;        /* inclusive */
  const char *begin_day, *end_day; /* daily only, inclusive */
} AstHistoryQuery;
/* Synchronous admission immediately before EACH external download request,
 * including calendar/metadata requests needed by a read. Never retain this
 * callback after minutes/daily returns or invoke it concurrently. A non-OK
 * result aborts the read; it must not trigger a retry or an unbudgeted request. */
typedef struct {
  void* context;
  AstStatus (*acquire)(void* context);
} AstRequestBudget;
typedef struct {
  uint32_t size, version;
  /* Sources are enumerable without credentials. Account access is checked when reading.
   * Empty settings create a metadata-only instance. Reading instances require source
   * and credential (possibly empty). Cancellation callbacks must be thread-safe;
   * providers may poll them from a worker thread, but must join before returning. */
  AstStatus (*sources)(void* instance, void* context,
                       AstStatus (*emit)(void*, const AstHistorySource*));
  AstStatus (*catalog)(void* instance, const char* venue, const char* product, AstCancellation,
                       void* context, AstStatus (*emit)(void*, const AstListing*));
  AstStatus (*minutes)(void* instance, const AstHistoryQuery*, AstCancellation, AstRequestBudget,
                       void* context, AstStatus (*emit)(void*, const AstMinute*));
  AstStatus (*daily)(void* instance, const AstHistoryQuery*, AstCancellation, AstRequestBudget,
                     void* context, AstStatus (*emit)(void*, const AstDaily*));
} AstHistoryV2;
#ifdef __cplusplus
}
#endif
#endif
