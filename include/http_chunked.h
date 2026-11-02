/*
 * http_chunked.h: decode an HTTP/1.1 chunked transfer body (RFC 9112 7.1) a
 * few bytes at a time. Plain C, no dependencies; tested on the host
 * (tests/test_misc.c). GitHub's API answers chunked now and then.
 *
 *   chunked_t c; chunked_init(&c);
 *   n = chunked_feed(&c, in, len, out);   // payload bytes written to out (<= len)
 *   if (c.done) ...                       // the empty terminating chunk was seen
 *
 * `out` may be the same buffer as `in` (in-place): payload never runs ahead
 * of where it is read from. Chunk extensions and trailers are skipped.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t state;      /* CH_* in http_chunked.c */
    size_t  left;       /* payload bytes still to copy in the current chunk */
    bool    done;       /* terminating chunk seen; later bytes are trailers */
    bool    bad;        /* framing error: a size line that is not hex */
} chunked_t;

void   chunked_init(chunked_t *c);
size_t chunked_feed(chunked_t *c, const uint8_t *in, size_t n, uint8_t *out);
