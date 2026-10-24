#ifndef ASTERION_PLUGIN_MATERIAL_H
#define ASTERION_PLUGIN_MATERIAL_H
#include "history.h"
#ifdef __cplusplus
extern "C" {
#endif
#define AST_MATERIAL "asterion.material"

/* Provider-owned resource names, never arbitrary local file paths or uploads.
 * This initial host supports credential-free, declared no-additional-fee
 * sources only. Those declarations are capabilities, not proof of a user's
 * legal entitlement. The caller still confirms its intended use before admission. */
typedef struct
{
    const char *source, *name, *usage;
    uint32_t max_bytes;
    uint32_t credential_required;
    uint32_t no_additional_fee;
} AstMaterialSource;

typedef struct
{
    uint32_t size, version;
    AstStatus (*sources)(void*, void*, AstStatus (*emit)(void*, const AstMaterialSource*));
    /* Call budget before EVERY external request. No callback retention or
     * automatic retries. Emit exactly one original response (not a summary),
     * <=max_bytes, after successful acquisition. Media type names the actual
     * bytes. source_published_at_ms=0 means unknown, never fetch time.
     * Borrowed output expires when emit returns. */
    AstStatus (*read)(
        void*,
        const char* source,
        const char* resource,
        uint32_t max_bytes,
        AstCancellation,
        AstRequestBudget,
        void*,
        AstStatus (*emit)(
            void*,
            const uint8_t*,
            uint32_t,
            const char* media_type,
            int64_t source_published_at_ms));
} AstMaterial;
#ifdef __cplusplus
}
#endif
#endif
