/* Synthetic data ONLY for contract tests. Never installed in production plugins/. */
#include "fixture.h"
#include <asterion/plugin/history.h>
#include <asterion/plugin/history_connection.h>
#include <asterion/plugin/history_tables.h>
#include <asterion/plugin/history_response.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

typedef struct
{
    int running, fail_start, refuse;
    char response[2048];
    int64_t received_at_ns;
} Instance;

static uint32_t destroyed_count, stopped_count;

static int32_t running(void* p)
{
    return ((Instance*)p)->running;
}

static uint32_t destroyed(void)
{
    return destroyed_count;
}

static uint32_t stopped(void)
{
    return stopped_count;
}

static const TestLifetime lifetime = {sizeof(TestLifetime), 1, running, destroyed, stopped};

static AstStatus sources(void* self, void* ctx, AstStatus (*emit)(void*, const AstHistorySource*))
{
    (void)self;
    const char* venues[] = {"SHFE"};
    const uint32_t mins[] = {1, 5};
    const AstHistorySource m = {
        "fixture.minutes",
        "Third-party test minutes",
        "fixture.v1",
        "Asia/Shanghai",
        "bar_end",
        venues,
        1,
        mins,
        2,
        60,
        0
    };
    if (!emit)
        return AST_INVALID;
    return emit(ctx, &m);
}

static AstStatus minutes(
    void* self,
    const AstHistoryQuery* q,
    AstCancellation cancel,
    AstRequestBudget budget,
    void* ctx,
    AstStatus (*emit)(void*, const AstMinute*))
{
    if (!self || !q || !emit)
        return AST_INVALID;
    if (!((Instance*)self)->running)
        return AST_UNAVAILABLE;
    if (cancel.requested && cancel.requested(cancel.context))
        return AST_CANCELLED;
    if (!budget.acquire)
        return AST_INVALID;
    AstStatus permit = budget.acquire(budget.context);
    if (permit != AST_OK)
        return permit;
    const AstMinute row = {
        q->begin_ns,
        100000000,
        200000000,
        100000000,
        200000000,
        300000000,
        400000000,
        500000000,
        NULL
    };
    return emit(ctx, &row);
}

static const AstHistory history = {sizeof(AstHistory), 3, sources, minutes};

static AstStatus describe_connection(
    void* self,
    const char* source,
    void* ctx,
    AstStatus (*emit)(void*, const AstHistoryConnectionSchema*))
{
    (void)self;
    if (!source || !emit)
        return AST_INVALID;
    const AstHistoryConnectionSchema schema = {"Test credential", "测试凭据", 0, 256, 1, 30, 60};
    return emit(ctx, &schema);
}

static AstStatus verify_connection(
    void* self,
    const char* source,
    const char* credential,
    AstCancellation cancel,
    void* ctx,
    AstStatus (*emit)(void*, const AstConnectionCheck*))
{
    (void)self;
    (void)source;
    if (!credential || !emit)
        return AST_INVALID;
    if (cancel.requested && cancel.requested(cancel.context))
        return AST_CANCELLED;
    uint32_t state = !strcmp(credential, "invalid") ? AST_CHECK_INVALID_CREDENTIAL
        : !strcmp(credential, "limited")            ? AST_CHECK_RATE_LIMIT
                                                    : AST_CHECK_VERIFIED;
    const AstConnectionCheck catalog = {"catalog", state},
                             history = {
                                 "history",
                                 state == AST_CHECK_VERIFIED ? AST_CHECK_VERIFIED
                                                             : AST_CHECK_NOT_CHECKED
                             };
    AstStatus result = emit(ctx, &catalog);
    return result == AST_OK ? emit(ctx, &history) : result;
}

static const AstHistoryConnection connection =
    {sizeof(AstHistoryConnection), 1, describe_connection, verify_connection};

/* Market-wide tables of one exchange with one contract. The first week of March 2024 holds every
 * outcome a reader meets: 03-01 and 03-04 are ordinary days, 03-05 carries a volume no table can
 * hold, 03-06 is a day this source cannot use, and 03-07 is not published yet. */
static AstStatus tables(void* self, void* ctx, AstStatus (*emit)(void*, const AstHistoryTable*))
{
    (void)self;
    if (!emit)
        return AST_INVALID;
    static const char* venues[] = {"SHFE"};
    const AstHistoryTable rows[] = {
        {"futures.contracts", 1, "fixture.contracts.v1", venues, 1},
        {"futures.calendar", 1, "fixture.calendar.v1", venues, 1},
        {"futures.daily", 1, "fixture.daily.v1", venues, 1}
    };
    for (int i = 0; i < 3; ++i) {
        AstStatus result = emit(ctx, &rows[i]);
        if (result != AST_OK)
            return result;
    }
    return AST_OK;
}

typedef struct
{
    const char *name, *value;
} Cell;

static AstStatus emit_row(
    const Cell* known,
    int count,
    const char* const* columns,
    uint32_t width,
    void* ctx,
    AstStatus (*emit)(void*, const char* const*, uint32_t))
{
    const char* cells[64];
    if (width > 64)
        return AST_INVALID;
    for (uint32_t c = 0; c < width; ++c) {
        cells[c] = NULL;
        for (int k = 0; k < count; ++k)
            if (columns[c] && !strcmp(columns[c], known[k].name))
                cells[c] = known[k].value;
    }
    return emit(ctx, cells, width);
}

static AstStatus read_table(
    void* self,
    const char* table,
    const char* const* columns,
    uint32_t width,
    const char* venue,
    const char* period,
    AstCancellation cancel,
    AstRequestBudget budget,
    AstDiagnostics diagnostics,
    void* ctx,
    AstStatus (*emit)(void*, const char* const*, uint32_t))
{
    if (!self || !table || !columns || !venue || !period || !emit)
        return AST_INVALID;
    Instance* p = self;
    p->response[0] = 0;
    p->received_at_ns = 0;
    if (!p->running)
        return AST_UNAVAILABLE;
    if (cancel.requested && cancel.requested(cancel.context))
        return AST_CANCELLED;
    if (!budget.acquire)
        return AST_INVALID;
    AstStatus permit = budget.acquire(budget.context);
    if (permit != AST_OK)
        return permit;
    if (p->refuse) {
        if (diagnostics.report)
            diagnostics.report(
                diagnostics.context,
                "fixture refused the request: the account has no permission");
        return AST_PERMISSION;
    }
    if (strcmp(venue, "SHFE"))
        return AST_UNSUPPORTED;
    if (!strcmp(table, "futures.contracts")) {
        const Cell row[] = {
            {"contract", "SHFE/cu/2024-03"},
            {"source_instrument", "vendor/cu2403"},
            {"name", "Fixture copper"},
            {"list_date", "2024-02-29"},
            {"delist_date", "2024-03-15"},
            {"per_unit", "5"},
            {"trade_unit", "tonnes"}
        };
        return emit_row(row, 7, columns, width, ctx, emit);
    }
    if (!strcmp(table, "futures.calendar")) {
        static const char* days[][3] = {
            {"2024-02-28", "1", NULL},
            {"2024-02-29", "1", "2024-02-28"},
            {"2024-03-01", "1", "2024-02-29"},
            {"2024-03-02", "0", NULL},
            {"2024-03-03", "0", NULL},
            {"2024-03-04", "1", "2024-03-01"},
            {"2024-03-05", "1", "2024-03-04"},
            {"2024-03-06", "1", "2024-03-05"},
            {"2024-03-07", "1", "2024-03-06"}
        };
        if (strcmp(period, "2024"))
            return AST_OK;
        for (int i = 0; i < 9; ++i) {
            const Cell row[] =
                {{"day", days[i][0]}, {"open", days[i][1]}, {"previous_trading_day", days[i][2]}};
            AstStatus result = emit_row(row, 3, columns, width, ctx, emit);
            if (result != AST_OK)
                return result;
        }
        return AST_OK;
    }
    if (strcmp(table, "futures.daily"))
        return AST_UNSUPPORTED;
    /* Optional control of this synthetic source, inherited only by isolated tests. */
    char mode[32] = {0};
    const char* control = getenv("ASTERION_TABLE_FIXTURE_CONTROL");
    if (control) {
        FILE* f = fopen(control, "r");
        if (f) {
            fgets(mode, sizeof(mode), f);
            fclose(f);
        }
    }
    if (!strcmp(mode, "fail-once")) {
        FILE* f = fopen(control, "w");
        if (f) {
            fputs("corrected", f);
            fclose(f);
        }
        if (diagnostics.report)
            diagnostics.report(diagnostics.context, "fixture transient source failure");
        return AST_UNAVAILABLE;
    }
    if (!strcmp(mode, "empty"))
        return AST_OK;
    if (!strcmp(mode, "hold")) {
        FILE* f = fopen(control, "w");
        if (f) {
            fputs("holding", f);
            fclose(f);
        }
        for (int i = 0; i < 1000; ++i) {
            if (cancel.requested && cancel.requested(cancel.context))
                return AST_CANCELLED;
            char next[32] = {0};
            f = fopen(control, "r");
            if (f) {
                fgets(next, sizeof(next), f);
                fclose(f);
            }
            if (!strcmp(next, "release"))
                break;
            struct timespec delay = {0, 10000000};
            nanosleep(&delay, NULL);
        }
    }
    if (!strcmp(mode, "wait")) {
        FILE* f = fopen(control, "w");
        if (f) {
            fputs("waiting", f);
            fclose(f);
        }
        for (int i = 0; i < 1000; ++i) {
            if (cancel.requested && cancel.requested(cancel.context))
                return AST_CANCELLED;
            struct timespec delay = {0, 10000000};
            nanosleep(&delay, NULL);
        }
        return AST_UNAVAILABLE;
    }
    if (!strcmp(period, "2024-03-07"))
        return AST_OK;
    if (!strcmp(period, "2024-03-06")) {
        if (diagnostics.report)
            diagnostics.report(
                diagnostics.context,
                "futures.daily SHFE 2024-03-06: vendor/cu2499 is not in the contract list");
        return AST_INVALID;
    }
    const Cell row[] = {
        {"contract", "SHFE/cu/2024-03"},
        {"close", !strcmp(mode, "corrected") ? "72001.5" : "72000.5"},
        {"settlement", "71990"},
        {"volume", !strcmp(period, "2024-03-05") ? "1200.5" : "1200"},
        {"amount", "432000000000.25"},
        {"open_interest", "3400"}
    };
    const AstStatus result = emit_row(row, 6, columns, width, ctx, emit);
    if (result == AST_OK) {
        char compact[9];
        int n = 0;
        for (const char* c = period; *c && n < 8; ++c)
            if (*c != '-')
                compact[n++] = *c;
        compact[n] = 0;
        snprintf(
            p->response,
            sizeof(p->response),
            "{\"code\":0,\"msg\":null,\"data\":{\"fields\":[\"ts_code\",\"trade_date\",\"pre_close\",\"pre_settle\",\"open\",\"high\",\"low\",\"close\",\"settle\",\"vol\",\"amount\",\"oi\",\"oi_chg\",\"delv_settle\"],\"items\":[[\"CU2403.SHF\",\"%s\",null,null,null,null,null,%s,71990,1200,43200000.000025,3400,0,null]]}}",
            compact,
            !strcmp(mode, "corrected") ? "72001.5" : "72000.5");
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        p->received_at_ns = (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
    }
    return result;
}

static AstStatus response_get(
    void* self,
    void* context,
    AstStatus (*emit)(void*, const char*, const uint8_t*, uint32_t, int64_t))
{
    Instance* p = self;
    if (!p || !emit)
        return AST_INVALID;
    return p->response[0] ? emit(
                                context,
                                "tushare.fut_daily.raw.v1",
                                (const uint8_t*)p->response,
                                (uint32_t)strlen(p->response),
                                p->received_at_ns)
                          : AST_OK;
}

static const AstHistoryResponse response_evidence = {sizeof(AstHistoryResponse), 1, response_get};
static const AstHistoryTables history_tables = {sizeof(AstHistoryTables), 1, tables, read_table};

static AstStatus
create(const char* capability, const AstSetting* settings, uint32_t count, void** out)
{
    if (!out)
        return AST_INVALID;
    *out = NULL;
    if (!capability
        || (strcmp(capability, AST_TEST_LIFETIME) && strcmp(capability, AST_HISTORY)
            && strcmp(capability, AST_HISTORY_CONNECTION)
            && strcmp(capability, AST_HISTORY_TABLES)))
        return AST_UNSUPPORTED;
    if (count > 64 || (count && !settings))
        return AST_INVALID;
    Instance* p = calloc(1, sizeof(Instance));
    if (!p)
        return AST_LIMIT;
    for (uint32_t i = 0; i < count; ++i) {
        if (!settings[i].key || !settings[i].value) {
            free(p);
            return AST_INVALID;
        }
        if (!strcmp(settings[i].key, "fail_create")) {
            free(p);
            return AST_PERMISSION;
        }
        if (!strcmp(settings[i].key, "fail_start"))
            p->fail_start = 1;
        if (!strcmp(settings[i].key, "credential"))
            p->refuse = !strcmp(settings[i].value, "refused");
    }
    *out = p;
    return AST_OK;
}

static AstStatus start(void* object)
{
    if (!object)
        return AST_INVALID;
    Instance* p = object;
    p->running = 1;
    return p->fail_start ? AST_UNAVAILABLE : AST_OK;
}

static void stop(void* object)
{
    if (object) {
        ((Instance*)object)->running = 0;
        ++stopped_count;
    }
}

static void destroy(void* object)
{
    if (object) {
        free(object);
        ++destroyed_count;
    }
}

static AstStatus
query(void* self, const char* id, uint32_t version, uint32_t size, const void** out)
{
    if (!out)
        return AST_INVALID;
    *out = NULL;
    if (!self || !id)
        return AST_UNSUPPORTED;
#ifdef EMPTY_TABLE
    return AST_OK;
#endif
    if (!strcmp(id, AST_HISTORY_RESPONSE) && version == 1 && size == sizeof(AstHistoryResponse)) {
        *out = &response_evidence;
        return AST_OK;
    }
    if (!strcmp(id, AST_TEST_LIFETIME) && version == 1 && size == sizeof(TestLifetime)) {
        *out = &lifetime;
        return AST_OK;
    }
    if (!strcmp(id, AST_HISTORY) && version == 3 && size == sizeof(AstHistory)) {
        *out = &history;
        return AST_OK;
    }
    if (!strcmp(id, AST_HISTORY_CONNECTION) && version == 1
        && size == sizeof(AstHistoryConnection)) {
        *out = &connection;
        return AST_OK;
    }
    if (!strcmp(id, AST_HISTORY_TABLES) && version == 1 && size == sizeof(AstHistoryTables)) {
        *out = &history_tables;
        return AST_OK;
    }
    return AST_UNSUPPORTED;
}

static const AstCapability capabilities[] = {
    {AST_TEST_LIFETIME, 1, "tool"},
    {AST_HISTORY, 3, "data"},
    {AST_HISTORY_CONNECTION, 1, "data"},
    {AST_HISTORY_TABLES, 1, "data"},
    {AST_HISTORY_RESPONSE, 1, "data"}
};
#ifndef BAD_ABI
#define BAD_ABI ASTERION_PLUGIN_ABI_VERSION
#endif
#ifndef FIXTURE_VERSION
#define FIXTURE_VERSION "1.0.0"
#endif
static const AstPlugin plugin = {
    sizeof(AstPlugin),
    BAD_ABI,
    "test.independent.c",
    FIXTURE_VERSION,
    ASTERION_PLUGIN_PLATFORM,
    capabilities,
    5,
    create,
    start,
    stop,
    destroy,
    query
};
#ifdef MISSING_ENTRY
ASTERION_PLUGIN_EXPORT int unrelated_entry(void)
{
    return 1;
}
#else
ASTERION_PLUGIN_EXPORT const AstPlugin* asterion_plugin_entry(uint32_t abi, uint32_t size)
{
    return abi == ASTERION_PLUGIN_ABI_VERSION && size == sizeof(AstPlugin) ? &plugin : NULL;
}
#endif
