/*
 * psram_heap.c: see psram_heap.h. Blocks carry a size header; free blocks are
 * kept in address order and merged with their neighbours.
 */
#include "psram_heap.h"
#include <string.h>
#include <stdint.h>
#include "hardware/sync.h"

typedef struct blk { size_t size; struct blk *next; } blk_t;   /* size includes the header */
static blk_t *s_free;
static size_t s_avail;

void psram_heap_init(void *base, size_t size)
{
    s_free = (blk_t *)base; s_free->size = size & ~(size_t)7; s_free->next = NULL; s_avail = s_free->size;
}

void *psram_heap_malloc(size_t size)
{
    if (!s_free || !size) return NULL;
    size_t need = (size + sizeof(blk_t) + 7) & ~(size_t)7;
    uint32_t ints = save_and_disable_interrupts();
    blk_t **pp = &s_free, *b;
    for (b = s_free; b; pp = &b->next, b = b->next) {
        if (b->size < need) continue;
        if (b->size - need >= sizeof(blk_t) + 16) {         /* split */
            blk_t *rest = (blk_t *)((uint8_t *)b + need);
            rest->size = b->size - need; rest->next = b->next;
            *pp = rest; b->size = need;
        } else *pp = b->next;
        s_avail -= b->size;
        break;
    }
    restore_interrupts(ints);
    return b ? (void *)(b + 1) : NULL;
}

void *psram_heap_calloc(size_t n, size_t size)
{
    if (size && n > (size_t)-1 / size) return NULL;
    void *p = psram_heap_malloc(n * size);
    if (p) memset(p, 0, n * size);
    return p;
}

void psram_heap_free(void *p)
{
    if (!p) return;
    blk_t *b = (blk_t *)p - 1;
    uint32_t ints = save_and_disable_interrupts();
    s_avail += b->size;
    blk_t *prev = NULL, *cur = s_free;
    while (cur && cur < b) { prev = cur; cur = cur->next; }
    b->next = cur;
    if (prev) prev->next = b; else s_free = b;
    if (cur && (uint8_t *)b + b->size == (uint8_t *)cur) { b->size += cur->size; b->next = cur->next; }
    if (prev && (uint8_t *)prev + prev->size == (uint8_t *)b) { prev->size += b->size; prev->next = b->next; }
    restore_interrupts(ints);
}

size_t psram_heap_free_bytes(void) { return s_avail; }
