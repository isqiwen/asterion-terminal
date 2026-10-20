#ifndef ASTERION_PLUGIN_HISTORY_RESPONSE_H
#define ASTERION_PLUGIN_HISTORY_RESPONSE_H
#include "abi.h"
#ifdef __cplusplus
extern "C" {
#endif
#define AST_HISTORY_RESPONSE "asterion.history.response"

/* Optional observation of the immediately preceding successful table read.
 * Emits at most one bounded, allowlisted original response; no request/header.
 * No emission means not recorded, never reconstructed or assumed complete. */
typedef struct
{
    uint32_t size, version;
    AstStatus (*get)(
        void* instance,
        void* context,
        AstStatus (*emit)(
            void*,
            const char* format,
            const uint8_t* bytes,
            uint32_t count,
            int64_t received_at_ns));
} AstHistoryResponse;
#ifdef __cplusplus
}
#endif
#endif
