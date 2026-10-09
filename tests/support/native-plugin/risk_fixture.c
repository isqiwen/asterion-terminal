/* Test-only failures: a successful status must never imply an allow decision. */
#include <asterion/plugin/risk.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
/* Hold a real plugin lifecycle call while its account remains observable.
   1 = creation, 2 = destruction, 3 = evaluation, 0 = released. */
static pthread_mutex_t lifecycle_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t lifecycle_changed = PTHREAD_COND_INITIALIZER;
static int held_stage, waiting_stage;
ASTERION_PLUGIN_EXPORT void asterion_fixture_risk_hold(int stage) {
  pthread_mutex_lock(&lifecycle_mutex);
  held_stage = stage;
  pthread_cond_broadcast(&lifecycle_changed);
  pthread_mutex_unlock(&lifecycle_mutex);
}
ASTERION_PLUGIN_EXPORT int asterion_fixture_risk_waiting(void) {
  pthread_mutex_lock(&lifecycle_mutex);
  int result = waiting_stage;
  pthread_mutex_unlock(&lifecycle_mutex);
  return result;
}
static void wait_lifecycle(int stage) {
  pthread_mutex_lock(&lifecycle_mutex);
  while (held_stage == stage) {
    waiting_stage = stage;
    pthread_cond_wait(&lifecycle_changed, &lifecycle_mutex);
  }
  waiting_stage = 0;
  pthread_mutex_unlock(&lifecycle_mutex);
}
typedef struct {
  int mode;
} Instance;
static AstStatus create(const char* capability, const AstSetting* settings, uint32_t count,
                        void** out) {
  if (!out)
    return AST_INVALID;
  *out = NULL;
  if (!capability || strcmp(capability, AST_RISK_V1) || !settings)
    return AST_INVALID;
  if (count != 3 &&
      (count != 1 || !settings[0].key || strcmp(settings[0].key, "mode") || !settings[0].value))
    return AST_INVALID;
  wait_lifecycle(1);
  Instance* self = calloc(1, sizeof(Instance));
  if (!self)
    return AST_FAILED;
  self->mode = count == 3 ? 3 : atoi(settings[0].value);
  *out = self;
  return AST_OK;
}
static AstStatus start(void* self) {
  return self ? AST_OK : AST_INVALID;
}
static void stop(void* self) {
  (void)self;
}
static void destroy(void* self) {
  wait_lifecycle(2);
  free(self);
}
static AstStatus evaluate(void* self, const AstRiskContext* input, uint32_t* decision) {
  if (!self || !input || !decision)
    return AST_INVALID;
  wait_lifecycle(3);
  switch (((Instance*)self)->mode) {
  case 0:
    return AST_OK; /* untouched output */
  case 1:
    *decision = 999;
    return AST_OK; /* unknown decision */
  case 2:
    *decision = AST_RISK_ALLOW;
    return AST_FAILED; /* error overrides allow */
  case 3:
    *decision = AST_RISK_ORDER_QUANTITY;
    return AST_OK;
  default:
    return AST_UNAVAILABLE;
  }
}
static const AstRiskV1 table = {sizeof(AstRiskV1), 1, evaluate};
static AstStatus query(void* self, const char* capability, uint32_t version, uint32_t size,
                       const void** out) {
  if (!out)
    return AST_INVALID;
  *out = NULL;
  if (!self || !capability)
    return AST_INVALID;
  if (strcmp(capability, AST_RISK_V1) || version != 1 || size != sizeof(table))
    return AST_UNSUPPORTED;
  *out = &table;
  return AST_OK;
}
static const AstCapability capabilities[] = {{AST_RISK_V1, 1, "risk"}};
static const AstPluginV1 plugin = {sizeof(AstPluginV1),
                                   ASTERION_PLUGIN_ABI_VERSION,
                                   "test.risk-failures",
                                   "1.0.0",
                                   ASTERION_PLUGIN_PLATFORM,
                                   capabilities,
                                   1,
                                   create,
                                   start,
                                   stop,
                                   destroy,
                                   query};
ASTERION_PLUGIN_EXPORT const AstPluginV1* asterion_plugin_entry_v1(uint32_t abi, uint32_t size) {
  return abi == ASTERION_PLUGIN_ABI_VERSION && size == sizeof(AstPluginV1) ? &plugin : NULL;
}
