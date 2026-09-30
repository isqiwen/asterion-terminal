#ifndef ASTERION_PLUGIN_HISTORY_CONNECTION_H
#define ASTERION_PLUGIN_HISTORY_CONNECTION_H
#include "abi.h"
#ifdef __cplusplus
extern "C" {
#endif
#define AST_HISTORY_CONNECTION_V1 "asterion.history.connection.v1"
/* Real history connection inputs: one provider credential and a request budget.
 * Password-like credentials MUST set remember_allowed=0. Metadata contains no secrets. */
typedef struct {
  const char *credential_label_en, *credential_label_zh;
  uint32_t credential_required, credential_max_length, remember_allowed;
  uint32_t requests_per_minute_default, requests_per_minute_max;
} AstHistoryConnectionSchema;
enum {
  AST_CHECK_VERIFIED = 0,
  AST_CHECK_NOT_CHECKED = 1,
  AST_CHECK_INVALID_CREDENTIAL = 2,
  AST_CHECK_PERMISSION = 3,
  AST_CHECK_RATE_LIMIT = 4,
  AST_CHECK_NETWORK = 5,
  AST_CHECK_FAILED = 6
};
typedef struct {
  const char* scope;
  uint32_t state;
} AstConnectionCheck;
typedef struct {
  uint32_t size, version;
  AstStatus (*describe)(void*, const char* source, void* context,
                        AstStatus (*emit)(void*, const AstHistoryConnectionSchema*));
  /* Performs read-only provider requests. No credentials or vendor diagnostics in results. */
  AstStatus (*verify)(void*, const char* source, const char* credential, AstCancellation,
                      void* context, AstStatus (*emit)(void*, const AstConnectionCheck*));
} AstHistoryConnectionV1;
#ifdef __cplusplus
}
#endif
#endif
