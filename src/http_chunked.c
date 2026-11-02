/*
 * http_chunked.c: see http_chunked.h.
 *
 *   chunk      = chunk-size [ chunk-ext ] CRLF chunk-data CRLF
 *   last-chunk = "0" [ chunk-ext ] CRLF trailer-section CRLF
 */
#include "http_chunked.h"

enum { CH_SIZE, CH_EXT, CH_SIZE_LF, CH_DATA, CH_DATA_CR, CH_DATA_LF, CH_TRAILER };

static int hexval(uint8_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void chunked_init(chunked_t *c) { c->state = CH_SIZE; c->left = 0; c->done = false; c->bad = false; }

size_t chunked_feed(chunked_t *c, const uint8_t *in, size_t n, uint8_t *out)
{
    size_t i = 0, w = 0;
    while (i < n) {
        uint8_t b = in[i];
        switch (c->state) {
        case CH_SIZE: {
            int h = hexval(b);
            if (h >= 0) { c->left = c->left * 16 + (size_t)h; i++; }
            else if (b == ';') { c->state = CH_EXT; i++; }
            else if (b == '\r') { c->state = CH_SIZE_LF; i++; }
            else if (b == '\n') { c->state = c->left ? CH_DATA : CH_TRAILER; i++; }   /* bare LF: be lenient */
            else { c->bad = true; return w; }
            break; }
        case CH_EXT:                                    /* ;name=value ... up to the CRLF */
            if (b == '\r') c->state = CH_SIZE_LF;
            else if (b == '\n') c->state = c->left ? CH_DATA : CH_TRAILER;
            i++; break;
        case CH_SIZE_LF:
            if (b != '\n') { c->bad = true; return w; }
            c->state = c->left ? CH_DATA : CH_TRAILER; i++;
            if (c->state == CH_TRAILER) c->done = true;
            break;
        case CH_DATA: {
            size_t take = n - i < c->left ? n - i : c->left;
            if (out + w != in + i) { for (size_t k = 0; k < take; k++) out[w + k] = in[i + k]; }   /* memmove-safe: w <= i */
            w += take; i += take; c->left -= take;
            if (!c->left) c->state = CH_DATA_CR;
            break; }
        case CH_DATA_CR:
            if (b == '\r') { c->state = CH_DATA_LF; i++; }
            else if (b == '\n') { c->state = CH_SIZE; i++; }
            else { c->bad = true; return w; }
            break;
        case CH_DATA_LF:
            if (b != '\n') { c->bad = true; return w; }
            c->state = CH_SIZE; i++; break;
        default:                                        /* CH_TRAILER: nothing more for the caller */
            c->done = true; return w;
        }
    }
    return w;
}
