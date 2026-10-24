#include <asterion/plugin/material.h>
#include <asterion/plugin/sdk.hpp>
#include <string_view>
#include <initializer_list>
#include <chrono>
#include <thread>
using namespace asterion;

namespace {

struct Instance
{
    bool running = false;
};

AstStatus sources(void*, void* ctx, AstStatus (*emit)(void*, const AstMaterialSource*)) noexcept
{
    return sdk::boundary([&] {
        for (const auto* id : {"fixture-material", "fixture-paid", "fixture-auth"}) {
            const AstMaterialSource row{
                id,
                "Isolated synthetic material source",
                "Synthetic test inputs only",
                4096,
                std::string_view(id) == "fixture-auth",
                std::string_view(id) != "fixture-paid"
            };
            sdk::check(emit(ctx, &row));
        }
    });
}

AstStatus read(
    void* object,
    const char* source,
    const char* resource,
    uint32_t maximum,
    AstCancellation cancel,
    AstRequestBudget budget,
    void* ctx,
    AstStatus (*emit)(void*, const uint8_t*, uint32_t, const char*, int64_t)) noexcept
{
    return sdk::boundary([&] {
        if (!object || !static_cast<Instance*>(object)->running || !source || !resource || !emit
            || !budget.acquire || !maximum)
            throw AstStatus(AST_INVALID);
        if (cancel.requested && cancel.requested(cancel.context))
            throw AstStatus(AST_CANCELLED);
        sdk::check(budget.acquire(budget.context));
        if (std::string_view(resource) == "slow") {
            for (unsigned i = 0; i < 50; ++i) {
                if (cancel.requested && cancel.requested(cancel.context))
                    throw AstStatus(AST_CANCELLED);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        if (std::string_view(resource) == "over-budget")
            (void)budget.acquire(
                budget.context); // Faulty provider ignores denial: host must still reject.
        if (std::string_view(resource) == "no-original")
            return;
        const auto selected = std::string_view(resource);
        const std::string_view body = selected == "margin-announcement"
            ? "SHFE/rb/2026-11 计划于2026-10-24将保证金率调整为12%。"
            : selected == "margin-correction"
            ? "SHFE/rb/2026-11 计划于2026-10-24将保证金率调整为13%。"
            : selected == "margin-withdrawal" ? "The corrected announcement is withdrawn."
                                              : "工厂甲计划检修\n";
        sdk::check(
            emit(ctx, reinterpret_cast<const uint8_t*>(body.data()), body.size(), "text/plain", 0));
    });
}

AstStatus create(const char* capability, const AstSetting*, uint32_t count, void** out) noexcept
{
    if (!out)
        return AST_INVALID;
    *out = nullptr;
    return sdk::boundary([&] {
        if (!capability || std::string_view(capability) != AST_MATERIAL || count)
            throw AstStatus(AST_INVALID);
        *out = new Instance;
    });
}

AstStatus start(void* p) noexcept
{
    if (!p)
        return AST_INVALID;
    static_cast<Instance*>(p)->running = true;
    return AST_OK;
}

void stop(void* p) noexcept
{
    if (p)
        static_cast<Instance*>(p)->running = false;
}

void destroy(void* p) noexcept
{
    delete static_cast<Instance*>(p);
}

const AstMaterial api{sizeof(AstMaterial), 1, sources, read};

AstStatus query(void*, const char* id, uint32_t version, uint32_t size, const void** out) noexcept
{
    if (!out)
        return AST_INVALID;
    *out = nullptr;
    if (id && std::string_view(id) == AST_MATERIAL && version == 1 && size == sizeof(api))
        *out = &api;
    return *out ? AST_OK : AST_UNSUPPORTED;
}

const AstCapability capabilities[]{{AST_MATERIAL, 1, "data"}};
const AstPlugin plugin{
    sizeof(AstPlugin),
    ASTERION_PLUGIN_ABI_VERSION,
    "fixture.material",
    "1.0.0",
    ASTERION_PLUGIN_PLATFORM,
    capabilities,
    1,
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
