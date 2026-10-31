/* A small first-fit heap in PSRAM, for mbedTLS and cJSON (HTTPS and the
 * GitHub release list): interrupt-safe, as lwIP callbacks allocate too. */
#pragma once
#include <stddef.h>
void  psram_heap_init(void *base, size_t size);
void *psram_heap_calloc(size_t n, size_t size);
void *psram_heap_malloc(size_t size);
void  psram_heap_free(void *p);
size_t psram_heap_free_bytes(void);
