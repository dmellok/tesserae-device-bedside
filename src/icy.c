/*
 * icy.c: see icy.h.
 */
#include "icy.h"

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>

void icy_init(icy_t *c, uint32_t metaint)
{
    memset(c, 0, sizeof *c);
    c->metaint = metaint;
    c->until_meta = metaint;
    c->meta_left = -1;
}

/* A finished metadata block: pull StreamTitle='...'; out of it. */
static void block_done(icy_t *c)
{
    c->meta[c->meta_n] = 0;
    const char *p = strstr(c->meta, "StreamTitle='");
    if (!p) return;
    p += 13;
    const char *e = strstr(p, "';");          /* the title may hold a quote; the field ends at ';  */
    if (!e) e = p + strlen(p);
    size_t n = (size_t)(e - p);
    if (n >= sizeof c->title) n = sizeof c->title - 1;
    char t[sizeof c->title];
    memcpy(t, p, n); t[n] = 0;
    while (n && (t[n - 1] == ' ' || t[n - 1] == '\'')) t[--n] = 0;   /* "...' " with a stray quote */
    if (strcmp(t, c->title)) { memcpy(c->title, t, n + 1); c->title_new = true; }
}

void icy_feed(icy_t *c, const uint8_t *d, size_t n, icy_sink_fn sink, void *ctx)
{
    if (!c->metaint) { if (n) sink(ctx, d, n); return; }
    while (n) {
        if (c->meta_left == -1 && c->until_meta == 0) {       /* a length byte */
            int len = (int)d[0] * 16;
            d++; n--;
            if (len == 0) { c->until_meta = c->metaint; continue; }   /* an empty block: nothing new */
            c->meta_left = len; c->meta_n = 0;
            continue;
        }
        if (c->meta_left > 0) {                                /* inside a block */
            size_t take = (size_t)c->meta_left < n ? (size_t)c->meta_left : n;
            size_t keep = take;
            if ((size_t)c->meta_n + keep > sizeof c->meta - 1) keep = sizeof c->meta - 1 - (size_t)c->meta_n;
            if (keep) { memcpy(c->meta + c->meta_n, d, keep); c->meta_n += (int)keep; }
            d += take; n -= take; c->meta_left -= (int)take;
            if (c->meta_left == 0) { block_done(c); c->meta_left = -1; c->until_meta = c->metaint; }
            continue;
        }
        size_t take = c->until_meta < n ? c->until_meta : n;  /* audio */
        sink(ctx, d, take);
        d += take; n -= take; c->until_meta -= (uint32_t)take;
    }
}

static void trimmed(const char *v, char *out, size_t cap)
{
    while (*v == ' ' || *v == '\t') v++;
    size_t n = strlen(v);
    while (n && (v[n - 1] == ' ' || v[n - 1] == '\r' || v[n - 1] == '\n')) n--;
    if (n >= cap) n = cap - 1;
    memcpy(out, v, n); out[n] = 0;
}

bool icy_header(const char *line, char *name, size_t name_cap, int *kbps, uint32_t *metaint, char *ctype, size_t ctype_cap)
{
    const char *colon = strchr(line, ':');
    if (!colon) return false;
    size_t kl = (size_t)(colon - line);
    const char *v = colon + 1;
    if (kl == 8 && !strncasecmp(line, "icy-name", 8))        { if (name) trimmed(v, name, name_cap); return true; }
    if (kl == 6 && !strncasecmp(line, "icy-br", 6))          { if (kbps) *kbps = atoi(v); return true; }
    if (kl == 11 && !strncasecmp(line, "icy-metaint", 11))   { if (metaint) *metaint = (uint32_t)strtoul(v, NULL, 10); return true; }
    if (kl == 12 && !strncasecmp(line, "content-type", 12))  { if (ctype) trimmed(v, ctype, ctype_cap); return true; }
    return false;
}
