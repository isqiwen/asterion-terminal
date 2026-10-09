#ifndef ASTERION_TEST_FIXTURE_H
#define ASTERION_TEST_FIXTURE_H
#include <asterion/plugin/abi.h>
#define AST_TEST_LIFETIME "test.lifetime.v1"
typedef struct {
  uint32_t size, version;
  int32_t (*running)(void*);
  uint32_t (*destroyed)(void);
  uint32_t (*stopped)(void);
} TestLifetime;
#endif
