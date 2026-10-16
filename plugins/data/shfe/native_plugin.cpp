#include "margins.hpp"
#include <asterion/foundation/serialization.hpp>
#include <asterion/plugin/history_tables.h>
#include <asterion/plugin/history_response.h>
#include <asterion/plugin/sdk.hpp>
#include <httplib.h>
#include <chrono>
#include <memory>

using namespace asterion;

namespace {

struct Instance
{
    bool running = false;
    std::optional<SourceResponse> response;
};

AstStatus tables(void*, void* context, AstStatus (*emit)(void*, const AstHistoryTable*)) noexcept
{
    return sdk::boundary([&] {
        if (!emit)
            throw AstStatus(AST_INVALID);
        const char* venues[]{"SHFE"};
        const AstHistoryTable row{shfe::table, 1, shfe::source, venues, 1};
        sdk::check(emit(context, &row));
    });
}

AstStatus read(
    void* object,
    const char* table,
    const char* const* columns,
    uint32_t count,
    const char* venue,
    const char* day,
    AstCancellation cancel,
    AstRequestBudget budget,
    AstDiagnostics diagnostics,
    void* context,
    AstStatus (*emit)(void*, const char* const*, uint32_t)) noexcept
{
    return sdk::boundary([&] {
        auto* self = static_cast<Instance*>(object);
        if (!self || !self->running || !table || std::string_view(table) != shfe::table || !columns
            || count != 4 || !venue || !day || !budget.acquire || !emit)
            throw AstStatus(AST_INVALID);
        self->response.reset();
        const auto& schema = history_table(shfe::table);
        for (unsigned i = 0; i < count; ++i)
            if (!columns[i] || columns[i] != schema.columns[i].name)
                throw AstStatus(AST_INVALID);
        const auto report_error = [&](const char* text) {
            if (diagnostics.report)
                diagnostics.report(diagnostics.context, text);
        };
        try {
            shfe::validate_scope(venue, day);
        } catch (const std::invalid_argument& e) {
            report_error(e.what());
            throw;
        }
        const auto cancelled = [&] {
            return cancel.requested && cancel.requested(cancel.context);
        };
        httplib::SSLClient client("www.shfe.com.cn", 443);
        client.enable_server_certificate_verification(true);
        client.set_follow_location(false);
        client.set_connection_timeout(10);
        client.set_read_timeout(20);
        client.set_max_timeout(30000);
        Json bodies = Json::object();
        for (bool settlement : {false, true}) {
            sdk::check(budget.acquire(budget.context));
            if (cancelled())
                throw AstStatus(AST_CANCELLED);
            std::string body;
            const auto response = client.Get(
                shfe::report_path(day, settlement),
                [&](const char* data, std::size_t size) {
                    if (cancelled() || size > max_source_response_bytes / 2 - body.size())
                        return false;
                    body.append(data, size);
                    return true;
                });
            if (cancelled())
                throw AstStatus(AST_CANCELLED);
            if (!response || response->status != 200) {
                report_error("SHFE HTTPS margin report is unavailable");
                throw AstStatus(AST_UNAVAILABLE);
            }
            bodies[settlement ? "settlement" : "trading_day_start"] = std::move(body);
        }
        SourceResponse response{
            shfe::response_format,
            bodies.dump(),
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count()
        };
        std::vector<TableSourceRow> rows;
        try {
            rows = shfe::rows(response, day);
        } catch (const std::exception&) {
            report_error("invalid SHFE margin report");
            throw AstStatus(AST_INVALID);
        }
        for (const auto& row : rows) {
            if (cancelled())
                throw AstStatus(AST_CANCELLED);
            const char* cells[]{row[0]->c_str(), row[1]->c_str(), row[2]->c_str(), row[3]->c_str()};
            sdk::check(emit(context, cells, 4));
        }
        self->response = std::move(response);
    });
}

AstStatus response(
    void* object,
    void* context,
    AstStatus (*emit)(void*, const char*, const uint8_t*, uint32_t, int64_t)) noexcept
{
    return sdk::boundary([&] {
        const auto* self = static_cast<Instance*>(object);
        if (!self || !emit)
            throw AstStatus(AST_INVALID);
        if (self->response) {
            const auto& r = *self->response;
            sdk::check(emit(
                context,
                r.format.c_str(),
                reinterpret_cast<const uint8_t*>(r.body.data()),
                static_cast<uint32_t>(r.body.size()),
                r.received_at_ns));
        }
    });
}

const AstHistoryTablesV1 table_api{sizeof(AstHistoryTablesV1), 1, tables, read};
const AstHistoryResponseV1 response_api{sizeof(AstHistoryResponseV1), 1, response};

AstStatus
create(const char* capability, const AstSetting* settings, uint32_t count, void** out) noexcept
{
    if (!out)
        return AST_INVALID;
    *out = nullptr;
    return sdk::boundary([&] {
        if (!capability || std::string_view(capability) != AST_HISTORY_TABLES_V1)
            throw AstStatus(AST_UNSUPPORTED);
        if (count > 1
            || (count
                && (!settings || !settings[0].key || !settings[0].value
                    || std::string_view(settings[0].key) != "credential" || *settings[0].value)))
            throw AstStatus(AST_INVALID);
        *out = new Instance;
    });
}

AstStatus start(void* object) noexcept
{
    if (!object)
        return AST_INVALID;
    static_cast<Instance*>(object)->running = true;
    return AST_OK;
}

void stop(void* object) noexcept
{
    if (object)
        static_cast<Instance*>(object)->running = false;
}

void destroy(void* object) noexcept
{
    delete static_cast<Instance*>(object);
}

AstStatus query(void*, const char* id, uint32_t version, uint32_t size, const void** out) noexcept
{
    if (!out)
        return AST_INVALID;
    *out = nullptr;
    if (!id || version != 1)
        return AST_UNSUPPORTED;
    if (std::string_view(id) == AST_HISTORY_TABLES_V1 && size == sizeof(table_api))
        *out = &table_api;
    else if (std::string_view(id) == AST_HISTORY_RESPONSE_V1 && size == sizeof(response_api))
        *out = &response_api;
    return *out ? AST_OK : AST_UNSUPPORTED;
}

const AstCapability capabilities[]{
    {AST_HISTORY_TABLES_V1, 1, "data"},
    {AST_HISTORY_RESPONSE_V1, 1, "data"}
};
const AstPluginV1 plugin{
    sizeof(AstPluginV1),
    ASTERION_PLUGIN_ABI_VERSION,
    shfe::provider,
    "1.0.0",
    ASTERION_PLUGIN_PLATFORM,
    capabilities,
    2,
    create,
    start,
    stop,
    destroy,
    query
};

} // namespace

extern "C" ASTERION_PLUGIN_EXPORT const AstPluginV1*
asterion_plugin_entry_v1(uint32_t abi, uint32_t size) noexcept
{
    return abi == ASTERION_PLUGIN_ABI_VERSION && size == sizeof(AstPluginV1) ? &plugin : nullptr;
}
