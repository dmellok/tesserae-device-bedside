/*
 * test_sound.c: the sleep-sound engine (src/rainsynth.c): a stored recording
 * decodes to what was encoded and loops without a gap; a bad file falls back
 * to rain; every sound stays inside full scale; off means silence.
 */
#include "test.h"
#include "rainsynth.h"
#include <stdlib.h>

uint8_t bird_data[64];                                /* the chorus isn't exercised here */

/* IMA ADPCM in the web app's "ADP1" layout (include/store.h), as www/app.src.html encodes it. */
static const int STEP[89] = {7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,
    157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,
    2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,
    18500,20350,22385,24623,27086,29794,32767};
static const int IDX[8] = {-1,-1,-1,-1,2,4,6,8};
static void w32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static uint32_t adp_encode(const int16_t *pcm, uint32_t n, uint8_t *out, uint32_t rate)
{
    const uint32_t bb = 1024, per = 1 + 2 * (bb - 4), nb = (n + per - 1) / per;
    memset(out, 0, 16 + nb * bb);
    memcpy(out, "ADP1", 4); w32(out + 4, rate); w32(out + 8, n); w32(out + 12, bb);
    int idx = 0;
    for (uint32_t b = 0; b < nb; b++) {
        uint8_t *o = out + 16 + b * bb; uint32_t s0 = b * per;
        int pred = s0 < n ? pcm[s0] : 0;
        o[0] = (uint8_t)pred; o[1] = (uint8_t)(pred >> 8); o[2] = (uint8_t)idx;
        for (uint32_t k = 0; k < per - 1; k++) {
            uint32_t i = s0 + 1 + k; int s = i < n ? pcm[i] : pred;
            int st = STEP[idx], diff = s - pred, c = 0;
            if (diff < 0) { c = 8; diff = -diff; }
            int dq = st >> 3;
            if (diff >= st) { c |= 4; diff -= st; dq += st; } st >>= 1;
            if (diff >= st) { c |= 2; diff -= st; dq += st; } st >>= 1;
            if (diff >= st) { c |= 1; dq += st; }
            pred += (c & 8) ? -dq : dq; pred = pred > 32767 ? 32767 : pred < -32768 ? -32768 : pred;
            idx += IDX[c & 7]; idx = idx < 0 ? 0 : idx > 88 ? 88 : idx;
            o[4 + (k >> 1)] |= (uint8_t)((k & 1) ? c << 4 : c);
        }
    }
    return 16 + nb * bb;
}

static void render(rs_params_t *p, int16_t *out, int n)
{
    for (int i = 0; i < n; i += 220) rs_render(p, out + i, n - i < 220 ? n - i : 220);
}

static double rms(const int16_t *s, int n) { double a = 0; for (int i = 0; i < n; i++) a += (double)s[i] * s[i]; return sqrt(a / n); }

/* Best normalised correlation of a stretch of output against the source, over lags. */
static double best_corr(const int16_t *out, int n, const int16_t *src, int srcn, int start)
{
    double best = 0;
    for (int lag = 0; lag < 64; lag++) {
        double c = 0, e1 = 0, e2 = 0;
        for (int i = 0; i < n; i++) {
            double a = out[start + i], b = src[(start + i - lag + srcn) % srcn];
            c += a * b; e1 += a * a; e2 += b * b;
        }
        double r = c / sqrt(e1 * e2 + 1e-9);
        if (r > best) best = r;
    }
    return best;
}

#define SR RAIN_SR
static int16_t src[SR * 3];
static uint8_t file[SR * 3 / 2 + 4096];
static int16_t out[SR * 8];

static void make_source(void)
{
    /* A 3 s loop: two tones and some noise, whole periods so the loop point is clean. */
    unsigned r = 1;
    for (int i = 0; i < SR * 3; i++) {
        r = r * 1103515245u + 12345u;
        double t = (double)i / SR;
        src[i] = (int16_t)(9000 * sin(2 * M_PI * 220 * t) + 5000 * sin(2 * M_PI * 330 * t) + (int)((r >> 16) % 2000) - 1000);
    }
}

static void recording_plays_what_was_stored(void)
{
    uint32_t len = adp_encode(src, SR * 3, file, SR);
    rs_init(42);
    rs_params_t p = { .on = true, .kind = RS_RECORDING, .level = 5, .volume = 10, .fade = 1.0f, RS_DEFAULTS, .tone = 100,
                      .rec = file, .rec_len = len };
    render(&p, out, SR * 3);
    CHECK(rms(out + SR, SR) > 1000);                                 /* audible */
    CHECK(best_corr(out, SR / 2, src, SR * 3, SR + SR / 4) > 0.95);  /* and it is the recording */
}

static void recording_loops_without_a_gap(void)
{
    uint32_t len = adp_encode(src, SR * 3, file, SR);
    rs_init(42);
    rs_params_t p = { .on = true, .kind = RS_RECORDING, .level = 5, .volume = 10, .fade = 1.0f, RS_DEFAULTS, .tone = 100,
                      .rec = file, .rec_len = len };
    render(&p, out, SR * 8);                                         /* past the end twice */
    /* No silent stretch around the loop points (3 s and 6 s): 10 ms windows all carry sound. */
    int quiet = 0;
    for (int w = SR * 2; w + 220 < SR * 7; w += 220) if (rms(out + w, 220) < 300) quiet++;
    CHECK_EQ(quiet, 0);
    CHECK(best_corr(out, SR / 2, src, SR * 3, SR * 6 + SR / 4) > 0.95);   /* the second time round */
}

static void bad_recording_falls_back_to_rain(void)
{
    uint32_t len = adp_encode(src, SR * 3, file, SR);
    file[0] = 'X';                                                   /* not ADP1 */
    rs_init(42);
    rs_params_t p = { .on = true, .kind = RS_RECORDING, .level = 3, .volume = 10, .fade = 1.0f, RS_DEFAULTS, .rec = file, .rec_len = len };
    render(&p, out, SR * 3);
    CHECK(rms(out + SR, SR * 2) > 500);                              /* rain, not silence */
    CHECK(best_corr(out, SR / 2, src, SR * 3, SR + SR / 4) < 0.5);   /* and not the file */
    /* A truncated file and a wrong sample rate are refused the same way. */
    file[0] = 'A';
    rs_init(42); p.rec_len = len / 2; render(&p, out, SR * 2);
    CHECK(best_corr(out, SR / 2, src, SR * 3, SR) < 0.5);
    rs_init(42); p.rec_len = len; w32(file + 4, 44100); render(&p, out, SR * 2);
    CHECK(best_corr(out, SR / 2, src, SR * 3, SR) < 0.5);
}

static void every_sound_stays_in_range(void)
{
    for (int k = 0; k < RS_KINDS; k++) for (int l = 1; l <= 5; l += 2) {
        if (k == RS_RADIO) continue;                                 /* needs a stream: its own test below */
        uint32_t len = adp_encode(src, SR * 3, file, SR);
        rs_init(7);
        rs_params_t p = { .on = true, .kind = k, .level = l, .volume = 10, .fade = 1.0f, RS_DEFAULTS, .thunder = 100, .thunder_near = 100,
                          .rec = file, .rec_len = len };
        render(&p, out, SR * 6);
        int clip = 0; double r = rms(out + SR * 2, SR * 4);
        for (int i = 0; i < SR * 6; i++) if (out[i] >= 32700 || out[i] <= -32700) clip++;
        CHECK(clip < SR / 100);                                      /* under 0.5 % at the very edge */
        CHECK(r > 200);                                              /* and not silent */
    }
}

static void off_and_zero_volume_are_silent(void)
{
    rs_init(9);
    rs_params_t p = { .on = true, .kind = RS_RAIN, .level = 3, .volume = 10, .fade = 1.0f, RS_DEFAULTS };
    render(&p, out, SR * 2);
    p.on = false;
    render(&p, out, SR * 4);                                         /* the ~1.2 s fade, then nothing */
    CHECK(rms(out + SR * 3, SR) < 5);
    rs_init(9);
    rs_params_t q = { .on = true, .kind = RS_WIND, .level = 5, .volume = 0, .fade = 1.0f, RS_DEFAULTS };
    render(&q, out, SR * 3);
    CHECK(rms(out + SR, SR * 2) < 5);
}

/* ---- the speaker's tone stage (src/spk_dsp.c) ---- */
#include "spk_dsp.h"

static float fbuf[SR];
/* Peak of a sine at hz once the stage has settled (its last half second). */
static double stage_peak(const spk_tune_t *t, double hz, double amp)
{
    spk_coef_t c; spk_state_t s; spk_coef(t, SR, &c); spk_state_init(&s, &c);
    for (int i = 0; i < SR; i++) fbuf[i] = (float)(amp * sin(6.283185307 * hz * i / SR));
    spk_process(&c, &s, fbuf, SR);
    double pk = 0; for (int i = SR / 2; i < SR; i++) if (fabs(fbuf[i]) > pk) pk = fabs(fbuf[i]);
    return pk;
}

static void flat_speaker_stage_changes_nothing(void)
{
    spk_tune_t t = { 0 };
    CHECK(spk_tune_flat(&t));
    spk_coef_t c; spk_coef(&t, SR, &c); CHECK(!c.active);
    CHECK_NEAR(stage_peak(&t, 1000, 0.5), 0.5, 0.002);
    CHECK_NEAR(stage_peak(&t, 60, 0.5), 0.5, 0.002);
}

static void treble_cut_and_soften_take_the_edge_off(void)
{
    spk_tune_t t = { .treble_db = -12 };
    CHECK(stage_peak(&t, 6000, 0.5) < 0.5 * 0.4);           /* well down an octave above the 3 kHz shelf */
    CHECK(stage_peak(&t, 9000, 0.5) < 0.5 * 0.3);
    CHECK(stage_peak(&t, 200, 0.5) > 0.5 * 0.95);           /* the lows untouched */
    spk_tune_t u = { .soften = 5 };                         /* low-pass at 3 kHz, second order */
    CHECK(stage_peak(&u, 6000, 0.5) < 0.5 * 0.3);
    CHECK(stage_peak(&u, 500, 0.5) > 0.5 * 0.95);
}

static void low_cut_and_bass_shape_the_bottom(void)
{
    spk_tune_t t = { .low_cut = 4 };                        /* 160 Hz */
    CHECK(stage_peak(&t, 40, 0.5) < 0.5 * 0.1);             /* two octaves below: 24 dB down */
    CHECK(stage_peak(&t, 1000, 0.5) > 0.5 * 0.95);
    spk_tune_t b = { .bass_db = 6 };
    CHECK_NEAR(stage_peak(&b, 50, 0.3), 0.3 * 2.0, 0.3 * 0.15);   /* +6 dB on the shelf */
    CHECK_NEAR(stage_peak(&b, 2000, 0.3), 0.3, 0.3 * 0.05);
}

static void level_and_limiter_hold_the_peaks(void)
{
    spk_tune_t g = { .gain_db = -6 };
    CHECK_NEAR(stage_peak(&g, 1000, 0.5), 0.25, 0.01);
    spk_tune_t l = { .limit = 2 };                          /* firm: nothing past -12 dB (0.25) */
    double pk = stage_peak(&l, 1000, 0.9);
    CHECK(pk < 0.28); CHECK(pk > 0.2);
    spk_tune_t q = { .limit = 1 };                          /* gentle: quiet passages untouched */
    CHECK(stage_peak(&q, 1000, 0.3) > 0.29);
}

/* The high-frequency content of a signal: the energy of its first difference
 * (6 dB per octave towards the top, so whistles and chirps dominate it). */
static double edge(const int16_t *s, int n)
{
    double d = 0;
    for (int i = 1; i < n; i++) { double b = (double)s[i] - s[i - 1]; d += b * b; }
    return d;
}

static void softness_takes_the_edge_off_the_birds(void)
{
    /* The same whistler call, natural and fully softened: its top end goes,
     * most of its body stays (the sharpness, not the bird, is what gets cut). */
    double e[2], r[2];
    for (int k = 0; k < 2; k++) {
        rs_init(11);
        rs_params_t p = { .on = false, .volume = 10, .fade = 1.0f, RS_DEFAULTS, .bird_soft = k ? 100 : 0, .audition = 1, .audition_bird = BIRD_WHISTLER };
        render(&p, out, SR * 3);
        r[k] = rms(out, SR * 3); e[k] = edge(out, SR * 3);
    }
    CHECK(r[0] > 200);                                       /* the synthesised whistler plays on the host */
    CHECK(e[1] < e[0] * 0.35);                               /* the top end: well down */
    CHECK(r[1] > r[0] * 0.25);                               /* still clearly a bird */
}

static void the_test_sound_plays_with_the_sleep_sound_off(void)
{
    rs_init(5);
    rs_params_t p = { .on = false, .volume = 10, .fade = 1.0f, RS_DEFAULTS, .test = 2 };
    render(&p, out, SR * 2);
    CHECK(rms(out + SR, SR) > 32767 * 0.15);                 /* a 1 kHz tone at about -9 dBFS */
    p.test = 0;
    render(&p, out, SR * 2);
    CHECK(rms(out + SR, SR) < 5);                            /* gone within a second */
    rs_init(5);
    rs_params_t q = { .on = false, .volume = 10, .fade = 1.0f, RS_DEFAULTS, .test = 4 };
    render(&q, out, SR * 6);                                 /* the sweep wraps after 5 s without a bang */
    int clip = 0; for (int i = 0; i < SR * 6; i++) if (out[i] >= 32700 || out[i] <= -32700) clip++;
    CHECK(clip == 0);
}

/* ---- the radio kind: the engine pulls the decoded stream and treats it like any other bed ---- */
static int pull_calls;
static int fake_pull(int16_t *dst, int n)
{
    static double ph;
    for (int i = 0; i < n; i++) { dst[i] = (int16_t)(12000 * sin(ph)); ph += 6.283185307 * 440 / SR; }
    pull_calls++;
    return n;
}

static void the_radio_plays_what_it_is_handed(void)
{
    rs_init(3); pull_calls = 0;
    rs_params_t p = { .on = true, .kind = RS_RADIO, .level = 5, .volume = 10, .fade = 1.0f, RS_DEFAULTS, .radio_pull = fake_pull };
    render(&p, out, SR * 3);
    CHECK(pull_calls > 0);
    CHECK(rms(out + SR * 2, SR) > 12000 * 0.7 * 0.6);          /* the 440 Hz tone, at most a little quieter */
    CHECK(rms(out + SR * 2, SR) < 12000 * 0.71);
    p.on = false;
    render(&p, out, SR * 3);
    CHECK(rms(out + SR * 2, SR) < 5);                            /* faded out, then nothing more is pulled */
    int calls = pull_calls; render(&p, out, SR);
    CHECK_EQ(pull_calls, calls);
    rs_init(3);
    rs_params_t q = { .on = true, .kind = RS_RADIO, .level = 5, .volume = 10, .fade = 1.0f, RS_DEFAULTS, .radio_pull = NULL };
    render(&q, out, SR * 2);
    CHECK(rms(out + SR, SR) < 5);                                /* no stream to pull from: silence, not a crash */
}

int main(void)
{
    make_source();
    RUN(recording_plays_what_was_stored);
    RUN(recording_loops_without_a_gap);
    RUN(bad_recording_falls_back_to_rain);
    RUN(every_sound_stays_in_range);
    RUN(off_and_zero_volume_are_silent);
    RUN(flat_speaker_stage_changes_nothing);
    RUN(treble_cut_and_soften_take_the_edge_off);
    RUN(low_cut_and_bass_shape_the_bottom);
    RUN(level_and_limiter_hold_the_peaks);
    RUN(softness_takes_the_edge_off_the_birds);
    RUN(the_test_sound_plays_with_the_sleep_sound_off);
    RUN(the_radio_plays_what_it_is_handed);
    return test_summary("sound");
}
