/*
 * soma.h: SomaFM's channel list, in plain C (host-tested in tests/test_radio.c).
 * The firmware carries a snapshot (src/soma_snapshot.c, tools/soma_snapshot.py)
 * and radio.c replaces it with the live list from api.somafm.com/channels.json
 * once the panel is online. Every channel streams 128 kbps MP3 at
 * http://ice1.somafm.com/<id>-128-mp3 (all 51 checked, 3 Nov 2026).
 */
#pragma once
#include <stddef.h>

typedef struct { char id[16]; char title[32]; char desc[80]; } soma_channel_t;

extern const soma_channel_t SOMA_SNAPSHOT[];
extern const int            SOMA_SNAPSHOT_N;

/* channels.json -> out (at most max). Returns how many, 0 if the JSON is not what we expect. */
int  soma_parse(const char *json, soma_channel_t *out, int max);
/* The 128 kbps MP3 stream for a channel id. */
void soma_url(const char *id, char *url, size_t cap);
