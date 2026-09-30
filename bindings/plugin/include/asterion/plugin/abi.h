#ifndef ASTERION_PLUGIN_ABI_H
#define ASTERION_PLUGIN_ABI_H
#include <stdint.h>
#include <stddef.h>
#if defined(__GNUC__)
#define ASTERION_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define ASTERION_PLUGIN_EXPORT
#endif
#if defined(__APPLE__) && defined(__aarch64__)
#define ASTERION_PLUGIN_PLATFORM "macos-arm64"
#elif defined(__APPLE__) && defined(__x86_64__)
#define ASTERION_PLUGIN_PLATFORM "macos-x86_64"
#elif defined(__linux__) && defined(__x86_64__)
#define ASTERION_PLUGIN_PLATFORM "linux-x86_64"
#else
#error Unsupported native plugin platform
#endif
#ifdef __cplusplus
extern "C" {
#endif
#define ASTERION_PLUGIN_ABI_VERSION 1u
#define ASTERION_PLUGIN_ENTRY "asterion_plugin_entry_v1"
/* All strings are bounded, NUL-terminated UTF-8. Never transfer owning C++ objects.
 * Borrowed inputs live through the call. Descriptors live until library unload.
 * Output row pointers live only during the callback. Copy them before returning.
 * Calls on an instance are serialized. No exceptions may cross ANY callback. */
typedef int32_t AstStatus;
enum {
  AST_OK = 0,
  AST_INVALID = 1,
  AST_UNSUPPORTED = 2,
  AST_PERMISSION = 3,
  AST_UNAVAILABLE = 4,
  AST_CANCELLED = 5,
  AST_FAILED = 6,
  AST_LIMIT = 7
};
typedef struct {
  const char* key;
  const char* value;
} AstSetting;
typedef struct {
  void* context;
  int32_t (*requested)(void*);
} AstCancellation;
typedef struct {
  const char* id;
  uint32_t version;
  const char* kind;
} AstCapability;
typedef struct {
  uint32_t size, abi_version;
  const char* id;
  const char* version;
  const char* platform;
  const AstCapability* capabilities;
  uint32_t capability_count;
  /* create allocates inside the plugin; destroy releases inside the same plugin.
   * On failure *instance must be NULL. Query must set *table to NULL on failure. */
  AstStatus (*create)(const char* capability, const AstSetting*, uint32_t, void** instance);
  AstStatus (*start)(void* instance);
  void (*stop)(void* instance);
  void (*destroy)(void* instance);
  AstStatus (*query)(void* instance, const char* capability, uint32_t version, uint32_t table_size,
                     const void** table);
} AstPluginV1;
typedef const AstPluginV1* (*AstPluginEntryV1)(uint32_t host_abi, uint32_t host_struct_size);
#ifdef __cplusplus
}
#endif
#endif
