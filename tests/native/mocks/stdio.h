#pragma once
#include <stddef.h>
/* The native library exercises coordination without opening a UART/console. */
#define stdout ((void *)0)
static inline void flockfile(void *stream) { }
static inline void funlockfile(void *stream) { }
static inline int fflush(void *stream) { return 0; }
static inline int printf(const char *format, ...) { return 0; }
static inline size_t fwrite(const void *data, size_t size, size_t n, void *stream) { return n; }
