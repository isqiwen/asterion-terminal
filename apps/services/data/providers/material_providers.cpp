#include "material_providers.hpp"
#include "provider_programs.hpp"
#include <asterion/system/native_plugin.hpp>
#include <asterion/plugin/material.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <set>

namespace asterion::material_providers {
namespace {

std::string text(const char* value, std::size_t maximum = 4096)
{
    if (!value || !*value || strnlen(value, maximum + 1) > maximum)
        throw std::invalid_argument("invalid native material metadata");
    return value;
}

const AstMaterial& table(const NativeInstance& instance)
{
    const auto* api =
        static_cast<const AstMaterial*>(instance.query(AST_MATERIAL, 1, sizeof(AstMaterial)));
    if (api->size != sizeof(AstMaterial) || api->version != 1 || !api->sources || !api->read)
        throw std::invalid_argument("invalid native material capability");
    return *api;
}

struct Installed
{
    NativeLibrary library;
    std::vector<Source> sources;
};

const std::vector<Installed>& registry()
{
    static const auto value = [] {
        std::vector<Installed> result;
        std::set<std::string> ids;
        for (auto& library : discover_native_plugins(native_plugin_directory())) {
            if (std::ranges::none_of(library.descriptor().capabilities, [](const auto& c) {
                    return c.id == AST_MATERIAL && c.version == 1 && c.kind == "data";
                }))
                continue;
            auto instance = library.create(AST_MATERIAL, {});
            struct Output
            {
                std::vector<Source> rows;
                std::exception_ptr error;
            } output;
            const auto status = table(*instance).sources(
                instance->handle(),
                &output,
                [](void* context, const AstMaterialSource* row) noexcept -> AstStatus {
                    auto& out = *static_cast<Output*>(context);
                    if (out.error)
                        return AST_FAILED;
                    try {
                        if (!row || !row->max_bytes || row->max_bytes > protocol::max_material_bytes
                            || row->credential_required > 1 || row->no_additional_fee > 1
                            || out.rows.size() >= 128)
                            throw std::invalid_argument("invalid native material source");
                        Source source{
                            text(row->source),
                            text(row->name),
                            text(row->usage),
                            {},
                            {},
                            row->max_bytes,
                            row->credential_required != 0,
                            row->no_additional_fee != 0
                        };
                        validate_id(source.id);
                        out.rows.push_back(std::move(source));
                        return AST_OK;
                    } catch (...) {
                        out.error = std::current_exception();
                        return AST_FAILED;
                    }
                });
            if (output.error)
                std::rethrow_exception(output.error);
            check_plugin_status(status);
            for (auto& source : output.rows) {
                if (!ids.insert(source.id).second)
                    throw std::invalid_argument("duplicate material source");
                source.plugin_id = library.descriptor().id;
                source.artifact = library.sha256();
            }
            result.push_back({std::move(library), std::move(output.rows)});
        }
        return result;
    }();
    return value;
}

struct Selection
{
    const Installed& plugin;
    const Source& source;
};

Selection select(const std::string& id, const std::string& expected)
{
    validate_id(id);
    for (const auto& plugin : registry())
        for (const auto& source : plugin.sources)
            if (source.id == id) {
                if (source.artifact != expected)
                    throw Error(
                        ErrorCode::conflict,
                        "material provider differs from the admitted module");
                return {plugin, source};
            }
    throw Error(ErrorCode::unavailable, "material provider is not installed");
}

} // namespace

std::vector<Source> sources()
{
    std::vector<Source> result;
    for (const auto& plugin : registry())
        result.insert(result.end(), plugin.sources.begin(), plugin.sources.end());
    return result;
}

std::filesystem::path artifact_path(const std::string& id, const std::string& expected)
{
    return select(id, expected).plugin.library.path();
}

void preserve(
    const data::proto::MaterialRequest& request,
    const std::filesystem::path& directory,
    std::stop_token stop)
{
    protocol::validate_material_request(request);
    provider_programs::preserve(
        request.provider_artifact(),
        directory,
        [&] {
            return artifact_path(request.source_id(), request.provider_artifact());
        },
        stop);
}

Response fetch(
    const data::proto::MaterialRequest& request,
    const std::function<void(std::stop_token)>& acquire,
    std::stop_token stop)
{
    protocol::validate_material_request(request);
    const auto selected = select(request.source_id(), request.provider_artifact());
    if (selected.source.credential_required || !selected.source.no_additional_fee)
        throw Error(
            ErrorCode::permission_denied,
            "material source access or cost is not supported by this path");
    if (!acquire || request.max_bytes() > selected.source.max_bytes)
        throw std::invalid_argument("material source budget is missing or exceeds capability");
    if (stop.stop_requested())
        throw Error(ErrorCode::cancelled, "material acquisition cancelled");
    auto instance = selected.plugin.library.create(AST_MATERIAL, {});
    instance->start();

    struct Work
    {
        const std::function<void(std::stop_token)>& acquire;
        std::stop_token stop;
        unsigned maximum, used = 0;
        std::size_t max_bytes;
        std::optional<Response> result;
        std::exception_ptr error;
    } work{acquire, stop, request.max_requests(), 0, request.max_bytes(), {}, {}};

    const AstCancellation cancellation{&work, [](void* ctx) -> int32_t {
                                           return static_cast<Work*>(ctx)->stop.stop_requested();
                                       }};
    const AstRequestBudget budget{&work, [](void* ctx) noexcept -> AstStatus {
                                      auto& w = *static_cast<Work*>(ctx);
                                      if (w.error)
                                          return AST_FAILED;
                                      try {
                                          if (w.stop.stop_requested())
                                              return AST_CANCELLED;
                                          if (w.used >= w.maximum)
                                              throw Error(
                                                  ErrorCode::resource_exhausted,
                                                  "material request budget exhausted");
                                          w.acquire(w.stop);
                                          ++w.used;
                                          return AST_OK;
                                      } catch (...) {
                                          w.error = std::current_exception();
                                          return AST_FAILED;
                                      }
                                  }};
    const auto status = table(*instance).read(
        instance->handle(),
        request.source_id().c_str(),
        request.resource_id().c_str(),
        request.max_bytes(),
        cancellation,
        budget,
        &work,
        [](void* ctx,
            const uint8_t* data,
            uint32_t count,
            const char* media,
            int64_t published) noexcept -> AstStatus {
            auto& w = *static_cast<Work*>(ctx);
            if (w.error)
                return AST_FAILED;
            try {
                if (w.result || !w.used || !data || !count || count > w.max_bytes || published < 0)
                    throw std::invalid_argument("invalid native material response");
                Response r;
                r.bytes.assign(reinterpret_cast<const char*>(data), count);
                r.media_type = text(media, 128);
                r.acquired_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                                       .count();
                if (published)
                    r.source_published_at_ms = published;
                w.result = std::move(r);
                return AST_OK;
            } catch (...) {
                w.error = std::current_exception();
                return AST_FAILED;
            }
        });
    if (work.error)
        std::rethrow_exception(work.error);
    if (stop.stop_requested())
        throw Error(ErrorCode::cancelled, "material acquisition cancelled");
    check_plugin_status(status);
    if (!work.result)
        throw std::invalid_argument("material provider did not return an original response");
    return std::move(*work.result);
}

} // namespace asterion::material_providers
