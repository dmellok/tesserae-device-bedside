/*
 * test_radio.c: the radio's plain-C parts: splitting ICY metadata out of a
 * stream (src/icy.c) however the bytes arrive, reading the station's headers,
 * and bringing decoded stereo at 44.1 / 48 kHz down to the panel's mono
 * 22.05 kHz (src/pcm_resample.c).
 */
#include "test.h"
#include "icy.h"
#include "pcm_resample.h"
#include <stdlib.h>

/* A stream with metaint 16: 16 audio bytes, a block, 16 more, an empty block, 16 more. */
static uint8_t STREAM[200]; static int STREAM_N;
static void make_stream(void)
{
    int n = 0;
    for (int i = 0; i < 16; i++) STREAM[n++] = (uint8_t)('a' + i);
    const char *meta = "StreamTitle='Boards of Canada - Dayvan Cowboy';StreamUrl='';";   /* 60 chars -> 4 x 16 */
    STREAM[n++] = 4;
    for (int i = 0; i < 64; i++) STREAM[n++] = i < (int)strlen(meta) ? (uint8_t)meta[i] : 0;
    for (int i = 0; i < 16; i++) STREAM[n++] = (uint8_t)('A' + i);
    STREAM[n++] = 0;                                                /* an empty block: nothing new */
    for (int i = 0; i < 16; i++) STREAM[n++] = (uint8_t)('0' + i);
    STREAM_N = n;
}

static uint8_t got[256]; static int got_n;
static void sink(void *ctx, const uint8_t *d, size_t n) { (void)ctx; memcpy(got + got_n, d, n); got_n += (int)n; }

static void metadata_is_split_out_whole(void)
{
    icy_t c; icy_init(&c, 16); got_n = 0;
    icy_feed(&c, STREAM, (size_t)STREAM_N, sink, NULL);
    CHECK_EQ(got_n, 48);
    CHECK(memcmp(got, "abcdefghijklmnop", 16) == 0 && memcmp(got + 16, "ABCDEFGHIJKLMNOP", 16) == 0 && memcmp(got + 32, "0123456789:;<=>?", 16) == 0);
    CHECK_STR(c.title, "Boards of Canada - Dayvan Cowboy");
    CHECK(c.title_new);
}

static void metadata_is_split_out_a_byte_at_a_time(void)
{
    /* TCP segments split anywhere: every chunk size must give the same audio and title. */
    for (size_t step = 1; step <= 7; step++) {
        icy_t c; icy_init(&c, 16); got_n = 0;
        for (int i = 0; i < STREAM_N; i += (int)step) { size_t k = (size_t)(STREAM_N - i) < step ? (size_t)(STREAM_N - i) : step; icy_feed(&c, STREAM + i, k, sink, NULL); }
        CHECK_EQ(got_n, 48);
        CHECK(memcmp(got + 32, "0123456789:;<=>?", 16) == 0);
        CHECK_STR(c.title, "Boards of Canada - Dayvan Cowboy");
    }
}

static void no_metaint_passes_everything(void)
{
    icy_t c; icy_init(&c, 0); got_n = 0;
    icy_feed(&c, STREAM, (size_t)STREAM_N, sink, NULL);
    CHECK_EQ(got_n, STREAM_N);
    CHECK_STR(c.title, "");
}

static void titles_with_quotes_and_headers_parse(void)
{
    icy_t c; icy_init(&c, 4); got_n = 0;
    const char *meta = "StreamTitle='Sinead O'Connor - Nothing Compares 2 U';StreamUrl='';";
    uint8_t s[200]; int n = 0;
    memcpy(s, "abcd", 4); n = 4; s[n++] = 5;                       /* 80 bytes of block */
    for (int i = 0; i < 80; i++) s[n++] = i < (int)strlen(meta) ? (uint8_t)meta[i] : 0;
    icy_feed(&c, s, (size_t)n, sink, NULL);
    CHECK_STR(c.title, "Sinead O'Connor - Nothing Compares 2 U");

    char name[64] = "", ctype[32] = ""; int kbps = 0; uint32_t mi = 0;
    CHECK(icy_header("icy-name: Drone Zone: Atmospheric ambient space music.", name, sizeof name, &kbps, &mi, ctype, sizeof ctype));
    CHECK_STR(name, "Drone Zone: Atmospheric ambient space music.");
    CHECK(icy_header("ICY-BR:128", NULL, 0, &kbps, &mi, NULL, 0)); CHECK_EQ(kbps, 128);
    CHECK(icy_header("icy-metaint: 16000", NULL, 0, &kbps, &mi, NULL, 0)); CHECK_EQ(mi, 16000);
    CHECK(icy_header("Content-Type: audio/mpeg", NULL, 0, &kbps, &mi, ctype, sizeof ctype)); CHECK_STR(ctype, "audio/mpeg");
    CHECK(!icy_header("Server: Icecast 2.4.4", name, sizeof name, &kbps, &mi, ctype, sizeof ctype));
}

static int16_t in[4800 * 2], out[4800];
static void stereo_44k_becomes_mono_22k(void)
{
    /* 1 kHz, left and right in phase: the same tone comes out at half the sample count, nearly full amplitude. */
    for (int i = 0; i < 4410; i++) { in[2 * i] = in[2 * i + 1] = (int16_t)(10000 * sin(6.283185307 * 1000 * i / 44100.0)); }
    pcmrs_t s; pcmrs_init(&s);
    int n = pcmrs_run(&s, in, 4410, 2, 44100, 22050, out, 4800);
    CHECK_EQ(n, 2205);
    int pk = 0; for (int i = 100; i < n; i++) if (abs(out[i]) > pk) pk = abs(out[i]);
    CHECK(pk > 9700 && pk <= 10000);
    /* chunk boundaries join: two halves give the same sample count as one go */
    pcmrs_t t; pcmrs_init(&t);
    int m = pcmrs_run(&t, in, 2205, 2, 44100, 22050, out, 4800) + pcmrs_run(&t, in + 2205 * 2, 2205, 2, 44100, 22050, out, 4800);
    CHECK_EQ(m, 2205);
}

static void other_rates_keep_time(void)
{
    /* 48 kHz: 4800 frames are 100 ms, so 2205 samples out (give or take the carried fraction). */
    for (int i = 0; i < 4800; i++) in[i] = (int16_t)(3000 * ((i / 24) & 1 ? 1 : -1));
    pcmrs_t s; pcmrs_init(&s);
    int n = pcmrs_run(&s, in, 4800, 1, 48000, 22050, out, 4800);
    CHECK(n >= 2203 && n <= 2205);
    pcmrs_init(&s);
    n = pcmrs_run(&s, in, 2205, 1, 22050, 22050, out, 4800);     /* already 22.05 kHz: untouched */
    CHECK_EQ(n, 2205); CHECK_EQ(out[10], in[10]);
}

/* ---- SomaFM's channel list (src/soma.c) ---- */
#include "soma.h"

static void somafm_channels_parse_and_stream_urls_follow(void)
{
    const char *json = "{\"channels\":[{\"id\":\"dronezone\",\"title\":\"Drone Zone\",\"description\":\"Served best chilled, safe with most medications. \",\"listeners\":\"418\"},"
                       "{\"id\":\"\",\"title\":\"nameless\"},{\"id\":\"groovesalad\",\"title\":\"Groove Salad\"},"
                       "{\"id\":\"longdesc\",\"title\":\"Long\",\"description\":\"0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789\"}]}";
    soma_channel_t ch[8];
    int n = soma_parse(json, ch, 8);
    CHECK_EQ(n, 3);                                              /* the one with no id is skipped */
    CHECK_STR(ch[0].id, "dronezone"); CHECK_STR(ch[0].title, "Drone Zone");
    CHECK_STR(ch[0].desc, "Served best chilled, safe with most medications.");   /* trimmed */
    CHECK_STR(ch[1].id, "groovesalad"); CHECK_STR(ch[1].desc, "");
    CHECK_EQ((int)strlen(ch[2].desc), 79); CHECK(strstr(ch[2].desc, "...") != NULL);   /* cut to fit, with an ellipsis */
    CHECK_EQ(soma_parse("not json", ch, 8), 0);
    CHECK_EQ(soma_parse("{\"channels\":5}", ch, 8), 0);
    char url[96]; soma_url("dronezone", url, sizeof url);
    CHECK_STR(url, "http://ice1.somafm.com/dronezone-128-mp3");
    /* the snapshot compiled in is well formed */
    CHECK(SOMA_SNAPSHOT_N >= 30);
    for (int i = 0; i < SOMA_SNAPSHOT_N; i++) CHECK(SOMA_SNAPSHOT[i].id[0] && SOMA_SNAPSHOT[i].title[0] && strlen(SOMA_SNAPSHOT[i].id) < 16);
}

int main(void)
{
    make_stream();
    RUN(metadata_is_split_out_whole);
    RUN(metadata_is_split_out_a_byte_at_a_time);
    RUN(no_metaint_passes_everything);
    RUN(titles_with_quotes_and_headers_parse);
    RUN(stereo_44k_becomes_mono_22k);
    RUN(other_rates_keep_time);
    RUN(somafm_channels_parse_and_stream_urls_follow);
    return test_summary("radio");
}
