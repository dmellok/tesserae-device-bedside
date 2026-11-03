/*
 * icy.h: the Icecast / SHOUTcast side of an internet radio stream, in plain C
 * (host-tested in tests/test_radio.c).
 *
 * A station answers a GET with ICY headers (icy-name, icy-br, icy-metaint)
 * and then a continuous MP3 body. With "Icy-MetaData: 1" asked for, every
 * icy-metaint bytes of audio are followed by one length byte (x16) and that
 * many bytes of "StreamTitle='...';StreamUrl='';" padded with NULs. icy_feed()
 * splits the two: audio bytes go to the sink, titles land in the struct.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t metaint;        /* 0: the stream has no metadata blocks */
    uint32_t until_meta;     /* audio bytes before the next metadata block */
    int      meta_left;      /* -1: the next byte is a block's length; else bytes of the block still to come */
    char     meta[520];      /* the block being gathered (at most 16 * 255 bytes; we keep what fits) */
    int      meta_n;
    char     title[96];      /* the latest StreamTitle, '' until one arrives */
    bool     title_new;      /* set when title changed; the reader clears it */
} icy_t;

typedef void (*icy_sink_fn)(void *ctx, const uint8_t *audio, size_t n);

void icy_init(icy_t *c, uint32_t metaint);
void icy_feed(icy_t *c, const uint8_t *d, size_t n, icy_sink_fn sink, void *ctx);

/* One response header line (without CRLF). Fills what it recognises; returns
 * true when the line was one of ours (icy-name, icy-br, icy-metaint,
 * content-type). name/ctype may be NULL. */
bool icy_header(const char *line, char *name, size_t name_cap, int *kbps, uint32_t *metaint, char *ctype, size_t ctype_cap);
