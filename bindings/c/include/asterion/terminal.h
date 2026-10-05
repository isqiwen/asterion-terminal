#ifndef ASTERION_TERMINAL_H
#define ASTERION_TERMINAL_H
#ifdef __cplusplus
extern "C" {
#define ASTERION_NOEXCEPT noexcept
#else
#define ASTERION_NOEXCEPT
#endif

/* Opaque runtime. asterion_terminal_call may be invoked concurrently from any
 * thread. One native owner advances commands and service I/O; slow operations
 * suspend without blocking status reads or unrelated account commands.
 * This call waits for its result. Destruction must not overlap any call.
 * UTF-8 JSON protocol version 1. Every response is allocated by this library;
 * free it only with asterion_terminal_free. NULL indicates allocation failure.
 * No C++ object or exception crosses this boundary. */
/* Creation returns NULL on failure. Optional error receives an allocated JSON
 * error envelope (or NULL if allocation also fails); it is cleared on success.
 * Free that envelope with asterion_terminal_free, just like call responses. */
void* asterion_terminal_create(char** error) ASTERION_NOEXCEPT;
char* asterion_terminal_call(void* runtime, const char* request) ASTERION_NOEXCEPT;
void asterion_terminal_free(char* response) ASTERION_NOEXCEPT;
void asterion_terminal_destroy(void* runtime) ASTERION_NOEXCEPT;

#undef ASTERION_NOEXCEPT
#ifdef __cplusplus
}
#endif
#endif
