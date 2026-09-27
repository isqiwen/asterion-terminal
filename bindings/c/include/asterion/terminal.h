#ifndef ASTERION_TERMINAL_H
#define ASTERION_TERMINAL_H
#ifdef __cplusplus
extern "C" {
#define ASTERION_NOEXCEPT noexcept
#else
#define ASTERION_NOEXCEPT
#endif

/* Opaque single-thread-owned runtime. Caller serializes all calls and destruction.
 * UTF-8 JSON protocol version 1. Every response is allocated by this library;
 * free it only with asterion_terminal_free. NULL indicates allocation failure.
 * No C++ object or exception crosses this boundary. */
void* asterion_terminal_create(void) ASTERION_NOEXCEPT;
char* asterion_terminal_call(void* runtime, const char* request) ASTERION_NOEXCEPT;
void asterion_terminal_free(char* response) ASTERION_NOEXCEPT;
void asterion_terminal_destroy(void* runtime) ASTERION_NOEXCEPT;

#undef ASTERION_NOEXCEPT
#ifdef __cplusplus
}
#endif
#endif
