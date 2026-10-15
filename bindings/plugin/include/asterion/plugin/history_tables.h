#ifndef ASTERION_PLUGIN_HISTORY_TABLES_H
#define ASTERION_PLUGIN_HISTORY_TABLES_H
#include "history.h"
#ifdef __cplusplus
extern "C" {
#endif
#define AST_HISTORY_TABLES_V1 "asterion.history.tables.v1"

/* Market-wide tables, read one partition at a time: one venue and one period
 * ("all", a year "YYYY" or a trading day "YYYY-MM-DD", as the table defines).
 * The host defines every table's columns and alone types their values. A
 * provider delivers each cell as text, in the order of the column names it is
 * given:
 *   text      as published, without control characters
 *   contract  VENUE/product/YYYY-MM, for example SHFE/cu/2024-03
 *   date      YYYY-MM-DD
 *   boolean   0 or 1
 *   decimal   plain decimal notation without an exponent, never rounded
 * A NULL cell is a missing value: never zero, never a neighbouring row's value.
 * Amounts are in the quote currency and quantities in contracts. */
typedef struct
{
    const char* table; /* for example "futures.daily" */
    uint32_t schema; /* the column definition this provider implements */
    const char* source; /* names the provider and its mapping rules; changes with either */
    const char* const* venues;
    uint32_t venue_count;
} AstHistoryTable;

/* Why a read did not succeed: one English sentence written by the plugin, at
 * most 512 bytes, reported at most once per read and copied before the call
 * returns. It may name the request and identifiers from the market data; it
 * must not contain credentials or any text the vendor returned. */
typedef struct
{
    void* context;
    void (*report)(void* context, const char* message);
} AstDiagnostics;

typedef struct
{
    uint32_t size, version;
    /* Enumerable without credentials. Reading instances are created with the
     * setting "credential" (possibly empty). */
    AstStatus (
        *tables)(void* instance, void* context, AstStatus (*emit)(void*, const AstHistoryTable*));
    /* Emits every row of the partition as `column_count` cells, in any order and
     * each key once. A partition the vendor has not published is zero rows and
     * AST_OK. AST_INVALID means the vendor answered but the partition cannot be
     * used; any other failure means the request did not complete. The budget is
     * acquired before EACH external request, as for AstHistoryV3. */
    AstStatus (*read)(
        void* instance,
        const char* table,
        const char* const* columns,
        uint32_t column_count,
        const char* venue,
        const char* period,
        AstCancellation,
        AstRequestBudget,
        AstDiagnostics,
        void* context,
        AstStatus (*emit)(void*, const char* const* cells, uint32_t count));
} AstHistoryTablesV1;
#ifdef __cplusplus
}
#endif
#endif
