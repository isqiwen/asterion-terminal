#include "tables.hpp"
#include <asterion/plugin/history.h>
#include <asterion/plugin/history_connection.h>
#include <asterion/plugin/history_tables.h>
#include <asterion/plugin/history_response.h>
#include <asterion/plugin/sdk.hpp>
#include <algorithm>
#include <thread>
#include <condition_variable>
using namespace asterion;

namespace {

struct Instance
{
    std::string source, credential;
    std::unique_ptr<tushare::Minutes> minutes;
    std::unique_ptr<tushare::Tables> tables;
    bool running = false;
    AstRequestBudget budget{};
};

struct BudgetScope
{
    Instance& instance;

    BudgetScope(Instance& instance, AstRequestBudget budget)
        : instance(instance)
    {
        if (!budget.acquire)
            throw AstStatus(AST_INVALID);
        instance.budget = budget;
    }

    ~BudgetScope()
    {
        instance.budget = {};
    }
};

struct Cancellation
{
    std::stop_source source;
    std::jthread thread;

    explicit Cancellation(AstCancellation external)
    {
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
                cv.wait_for(lock, done, std::chrono::milliseconds(10), [] {
                    return false;
                });
            }
        });
    }
};

void active(Instance* self)
{
    if (!self || !self->running)
        throw AstStatus(AST_UNAVAILABLE);
}

AstStatus sources(void*, void* context, AstStatus (*emit)(void*, const AstHistorySource*)) noexcept
{
    return sdk::boundary([&] {
        if (!emit)
            throw AstStatus(AST_INVALID);
        static const char* venues[] = {"SHFE", "DCE", "CZCE", "CFFEX", "INE", "GFEX"};
        static const uint32_t minutes[] = {1, 5, 15, 30, 60};
        const AstHistorySource m{
            "tushare.ft_mins",
            "Tushare",
            "tushare.minutes.v3",
            "Asia/Shanghai",
            "bar_end",
            venues,
            6,
            minutes,
            5,
            500,
            1
        };
        sdk::check(emit(context, &m));
    });
}

HistoryIdentity identity(const AstContract& c)
{
    if (!c.venue || !c.product || !c.delivery_month)
        throw AstStatus(AST_INVALID);
    HistoryIdentity result{c.venue, c.product, c.delivery_month};
    result.validate();
    return result;
}

AstStatus minutes(
    void* object,
    const AstHistoryQuery* query,
    AstCancellation cancel,
    AstRequestBudget budget,
    void* context,
    AstStatus (*emit)(void*, const AstMinute*)) noexcept
{
    return sdk::boundary([&] {
        if (!query || !query->source_instrument || !emit)
            throw AstStatus(AST_INVALID);
        auto* self = static_cast<Instance*>(object);
        active(self);
        if (!self->minutes)
            throw AstStatus(AST_UNSUPPORTED);
        BudgetScope admission(*self, budget);
        Cancellation cancellation(cancel);
        const auto rows = self->minutes->read(
            {identity(query->contract),
                query->interval_minutes,
                query->begin_ns,
                query->end_ns,
                self->source,
                query->source_instrument},
            cancellation.source.get_token());
        for (const auto& item : rows) {
            if (cancellation.source.stop_requested())
                throw AstStatus(AST_CANCELLED);
            AstMinute row{
                item.timestamp_ns,
                item.open.raw(),
                item.high.raw(),
                item.low.raw(),
                item.close.raw(),
                item.volume.raw(),
                item.amount.raw(),
                item.open_interest.raw(),
                item.trading_day.empty() ? nullptr : item.trading_day.c_str()
            };
            sdk::check(emit(context, &row));
        }
    });
}

AstStatus tables(void*, void* context, AstStatus (*emit)(void*, const AstHistoryTable*)) noexcept
{
    return sdk::boundary([&] {
        if (!emit)
            throw AstStatus(AST_INVALID);
        for (const auto& definition : tushare::table_definitions()) {
            const AstHistoryTable row{
                definition.table,
                definition.schema,
                definition.source,
                definition.venues.data(),
                static_cast<uint32_t>(definition.venues.size())
            };
            sdk::check(emit(context, &row));
        }
    });
}

AstStatus read_table(
    void* object,
    const char* table,
    const char* const* columns,
    uint32_t column_count,
    const char* venue,
    const char* period,
    AstCancellation cancel,
    AstRequestBudget budget,
    AstDiagnostics diagnostics,
    void* context,
    AstStatus (*emit)(void*, const char* const*, uint32_t)) noexcept
{
    return sdk::boundary([&] {
        if (!table || !columns || !column_count || column_count > 64 || !venue || !period || !emit)
            throw AstStatus(AST_INVALID);
        auto* self = static_cast<Instance*>(object);
        active(self);
        if (!self->tables)
            throw AstStatus(AST_UNSUPPORTED);
        BudgetScope admission(*self, budget);
        Cancellation cancellation(cancel);
        std::vector<std::string> names;
        for (uint32_t i = 0; i < column_count; ++i) {
            if (!columns[i])
                throw AstStatus(AST_INVALID);
            names.emplace_back(columns[i]);
        }
        // Only sentences this plugin wrote cross the boundary; see tables.cpp.
        const auto report = [&](const char* message) {
            if (diagnostics.report)
                diagnostics.report(diagnostics.context, message);
        };
        std::vector<tushare::TableRow> rows;
        try {
            rows = self->tables->read(table, names, venue, period, cancellation.source.get_token());
        } catch (const tushare::TableError& error) {
            report(error.what());
            throw AstStatus(AST_INVALID);
        } catch (const tushare::RequestError& error) {
            report(error.what());
            switch (error.reason) {
                case tushare::AccessFailure::invalid_credential:
                case tushare::AccessFailure::permission:
                    throw AstStatus(AST_PERMISSION);
                case tushare::AccessFailure::rate_limit:
                    throw AstStatus(AST_LIMIT);
                case tushare::AccessFailure::network:
                    throw AstStatus(AST_UNAVAILABLE);
                default:
                    throw AstStatus(AST_FAILED);
            }
        }
        std::vector<const char*> cells(names.size());
        for (const auto& row : rows) {
            if (cancellation.source.stop_requested())
                throw AstStatus(AST_CANCELLED);
            for (std::size_t i = 0; i < cells.size(); ++i)
                cells[i] = row[i] ? row[i]->c_str() : nullptr;
            sdk::check(emit(context, cells.data(), column_count));
        }
    });
}

AstStatus describe_connection(
    void*,
    const char* source,
    void* context,
    AstStatus (*emit)(void*, const AstHistoryConnectionSchema*)) noexcept
{
    return sdk::boundary([&] {
        if (!source || !emit || std::string_view(source) != "tushare.ft_mins")
            throw AstStatus(AST_INVALID);
        const AstHistoryConnectionSchema
            schema{"Tushare Token", "Tushare Token", 1, 256, 1, 60, 500};
        sdk::check(emit(context, &schema));
    });
}

AstStatus verify_connection(
    void*,
    const char* source,
    const char* credential,
    AstCancellation cancel,
    void* context,
    AstStatus (*emit)(void*, const AstConnectionCheck*)) noexcept
{
    return sdk::boundary([&] {
        if (!source || !credential || !emit || std::string_view(source) != "tushare.ft_mins")
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
        if (token.empty() || token.size() > 256
            || token.find_first_of(" \r\n\t") != std::string::npos)
            catalog_state = AST_CHECK_INVALID_CREDENTIAL;
        else {
            std::vector<tushare::FuturesListing> contracts;
            catalog_state = check([&] {
                contracts =
                    tushare::contracts(token, "SHFE", "CU", cancellation.source.get_token());
            });
            if (catalog_state == AST_CHECK_VERIFIED) {
                const auto cutoff = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch() - std::chrono::hours(24))
                                        .count();
                auto day = tushare::format_time(cutoff).substr(0, 10);
                auto compact = day;
                std::erase(compact, '-');
                std::erase_if(contracts, [&](const auto& contract) {
                    return contract.list_date > compact;
                });
                std::ranges::sort(contracts, [](const auto& left, const auto& right) {
                    return left.delist_date > right.delist_date;
                });
                if (!contracts.empty()) {
                    const auto& contract = contracts.front();
                    if (contract.delist_date < compact)
                        day = contract.delist_date.substr(0, 4) + "-"
                            + contract.delist_date.substr(4, 2) + "-"
                            + contract.delist_date.substr(6, 2);
                    history_state = check([&] {
                        tushare::Minutes provider(token);
                        provider.start();
                        const auto begin = tushare::parse_time(day + " 09:00:00");
                        (void)provider.read(
                            {contract.identity,
                                1,
                                begin,
                                begin + 60000000000LL,
                                source,
                                contract.ts_code},
                            cancellation.source.get_token());
                    });
                }
            }
        }
        if (cancellation.source.stop_requested())
            throw AstStatus(AST_CANCELLED);
        const AstConnectionCheck catalog{"catalog", catalog_state},
            history{"history", history_state};
        sdk::check(emit(context, &catalog));
        sdk::check(emit(context, &history));
    });
}

const AstHistoryConnection
    connection{sizeof(AstHistoryConnection), 1, describe_connection, verify_connection};
const AstHistory history{sizeof(AstHistory), 3, sources, minutes};
const AstHistoryTables history_tables{sizeof(AstHistoryTables), 1, tables, read_table};

AstStatus
create(const char* capability, const AstSetting* settings, uint32_t count, void** out) noexcept
{
    if (!out)
        return AST_INVALID;
    *out = nullptr;
    return sdk::boundary([&] {
        if (!capability
            || (std::string_view(capability) != AST_HISTORY
                && std::string_view(capability) != AST_HISTORY_CONNECTION
                && std::string_view(capability) != AST_HISTORY_TABLES))
            throw AstStatus(AST_UNSUPPORTED);
        if (count > 64 || (count && !settings))
            throw AstStatus(AST_INVALID);
        for (uint32_t i = 0; i < count; ++i)
            if (!settings[i].key || !settings[i].value
                || (std::string_view(settings[i].key) != "source"
                    && std::string_view(settings[i].key) != "credential"))
                throw AstStatus(AST_INVALID);
        auto result = std::make_unique<Instance>();
        result->source = sdk::setting(settings, count, "source");
        result->credential = sdk::setting(settings, count, "credential");
        if (!result->source.empty() && result->source != "tushare.ft_mins")
            throw AstStatus(AST_UNSUPPORTED);
        auto post = [instance = result.get(), transport = tushare::https_transport()](
                        const std::string& body,
                        std::stop_token stop) {
            sdk::check(instance->budget.acquire(instance->budget.context));
            if (stop.stop_requested())
                throw AstStatus(AST_CANCELLED);
            return transport(body, stop);
        };
        if (result->source == "tushare.ft_mins")
            result->minutes = std::make_unique<tushare::Minutes>(result->credential, post);
        if (std::string_view(capability) == AST_HISTORY_TABLES && count
            && result->credential.empty())
            throw AstStatus(AST_INVALID);
        // A metadata instance has no credential and only lists the tables.
        if (std::string_view(capability) == AST_HISTORY_TABLES && !result->credential.empty())
            result->tables = std::make_unique<tushare::Tables>(result->credential, post);
        *out = result.release();
    });
}

AstStatus start(void* object) noexcept
{
    return sdk::boundary([&] {
        auto* self = static_cast<Instance*>(object);
        if (!self)
            throw AstStatus(AST_INVALID);
        if (self->minutes)
            self->minutes->start();
        self->running = true;
    });
}

void stop(void* object) noexcept
{
    auto* self = static_cast<Instance*>(object);
    if (!self)
        return;
    if (self->minutes)
        self->minutes->stop();
    self->running = false;
}

void destroy(void* object) noexcept
{
    delete static_cast<Instance*>(object);
}

AstStatus response_get(
    void* object,
    void* context,
    AstStatus (*emit)(void*, const char*, const uint8_t*, uint32_t, int64_t)) noexcept
{
    return sdk::boundary([&] {
        const auto* self = static_cast<Instance*>(object);
        if (!self || !self->tables || !emit)
            throw AstStatus(AST_INVALID);
        const auto& response = self->tables->response();
        if (response)
            sdk::check(emit(
                context,
                response->format.c_str(),
                reinterpret_cast<const uint8_t*>(response->body.data()),
                static_cast<uint32_t>(response->body.size()),
                response->received_at_ns));
    });
}

const AstHistoryResponse response_evidence{sizeof(AstHistoryResponse), 1, response_get};

AstStatus query(void*, const char* id, uint32_t version, uint32_t size, const void** out) noexcept
{
    if (!out)
        return AST_INVALID;
    *out = nullptr;
    if (id && std::string_view(id) == AST_HISTORY_RESPONSE && version == 1
        && size == sizeof(AstHistoryResponse)) {
        *out = &response_evidence;
        return AST_OK;
    }
    if (id && std::string_view(id) == AST_HISTORY_CONNECTION && version == 1
        && size == sizeof(AstHistoryConnection)) {
        *out = &connection;
        return AST_OK;
    }
    if (id && std::string_view(id) == AST_HISTORY_TABLES && version == 1
        && size == sizeof(AstHistoryTables)) {
        *out = &history_tables;
        return AST_OK;
    }
    if (!id || std::string_view(id) != AST_HISTORY || version != 3 || size != sizeof(AstHistory))
        return AST_UNSUPPORTED;
    *out = &history;
    return AST_OK;
}

const AstCapability capabilities[] = {
    {AST_HISTORY, 3, "data"},
    {AST_HISTORY_CONNECTION, 1, "data"},
    {AST_HISTORY_TABLES, 1, "data"},
    {AST_HISTORY_RESPONSE, 1, "data"}
};
const AstPlugin plugin{
    sizeof(AstPlugin),
    ASTERION_PLUGIN_ABI_VERSION,
    "asterion.data.tushare",
    "1.0.0",
    ASTERION_PLUGIN_PLATFORM,
    capabilities,
    4,
    create,
    start,
    stop,
    destroy,
    query
};

} // namespace

extern "C" ASTERION_PLUGIN_EXPORT const AstPlugin*
asterion_plugin_entry(uint32_t abi, uint32_t size) noexcept
{
    return abi == ASTERION_PLUGIN_ABI_VERSION && size == sizeof(AstPlugin) ? &plugin : nullptr;
}
