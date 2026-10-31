/*
 * birds.c: the dawn chorus. See birds.h.
 */
#include "birds.h"
#include "birds_index.h"
#include "rainsynth.h"            /* RAIN_SR */

#include <math.h>
#include <string.h>

extern const uint8_t bird_data[];  /* src/birds_data.S */

#define SR ((float)RAIN_SR)

static const char *const NAMES[BIRD_COUNT] = {
    "Magpie", "Butcherbird", "Fairy-wren", "Whipbird", "Wattlebird", "Kookaburra",
    "Bell miner", "Pardalote", "Golden whistler", "Rosella",
};
const char *bird_name(int b) { return (b >= 0 && b < BIRD_COUNT) ? NAMES[b] : ""; }

/* When each species joins as the chorus builds, and its mean gap between calls
 * at full intensity (four times longer just after it joins). */
typedef struct { float join, gap; } sp_t;
static const sp_t SP[BIRD_COUNT] = {
    { 0.00f,  7.0f },   /* magpie: first, alone, long gaps */
    { 0.12f, 10.0f },   /* butcherbird */
    { 0.25f,  9.0f },   /* fairy-wren */
    { 0.40f, 14.0f },   /* whipbird */
    { 0.55f, 11.0f },   /* wattlebird */
    { 0.75f, 40.0f },   /* kookaburras, near the top */
    { 0.18f,  5.0f },   /* bell miner (synth) */
    { 0.06f,  6.0f },   /* pardalote (synth): quiet, early */
    { 0.48f, 13.0f },   /* golden whistler (synth) */
    { 0.32f, 12.0f },   /* rosella (synth) */
};

static uint32_t s_rng = 0x2545F491u;
static inline uint32_t rnd(void) { uint32_t x = s_rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return s_rng = x; }
static inline float uni(void) { return (float)(rnd() >> 8) * (1.0f / 16777216.0f); }
static inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

static int16_t s_mu[256];          /* mu-law decode */

/* ---- voices ---- */
#define VOICES 6
typedef struct {
    const uint8_t *p;
    uint32_t len;
    float    pos, rate, gain, lp_a, lp, send;
    bool     on;
} voice_t;
static voice_t V[VOICES];

/* ---- synthesised calls ----
 * A call is a list of notes. Each note is a sine (plus a little 2nd and 3rd
 * harmonic) whose pitch follows start -> peak -> end, with an attack, a release
 * and optional vibrato. The species generators below build one call at a time
 * with fresh random pitch and timing, so these birds never repeat either. */
#define SVOICES 4
#define NOTES 16
#define SINE_N 1024
typedef struct { float t0, dur, fa, fb, fc, pk, att, rel, amp, h2, h3, vr, vd; } note_t;
typedef struct {
    note_t n[NOTES];
    int    nn, cur;
    float  t, ph, gain, lp_a, lp, send;
    bool   on;
} svoice_t;
static svoice_t SV[SVOICES];
static float s_sin[SINE_N + 1];

static inline float sine(float ph)      /* ph in cycles */
{
    float x = (ph - (float)(int)ph) * SINE_N;
    int i = (int)x;
    return s_sin[i] + (s_sin[i + 1] - s_sin[i]) * (x - (float)i);
}

static int add(svoice_t *v, float t0, float dur, float fa, float fb, float fc, float amp)
{
    if (v->nn >= NOTES) return NOTES - 1;
    v->n[v->nn] = (note_t){ t0, dur, fa, fb, fc, 0.5f, 0.012f, dur * 0.35f, amp, 0.08f, 0.03f, 0, 0 };
    return v->nn++;
}

/* Build one call for synthesised bird b into v. */
static void make_call(svoice_t *v, int b)
{
    v->nn = 0;
    float t = 0;
    int j;
    switch (b) {
    case BIRD_BELLMINER: {                       /* 1-3 "tinks", each bird its own pitch */
        float f = 2750.0f + 450.0f * uni();
        int k = 1 + (int)(uni() * 2.99f);
        for (int i = 0; i < k; i++) {
            j = add(v, t, 0.16f, f, f * 1.004f, f * 0.998f, 0.75f);
            v->n[j].att = 0.0015f; v->n[j].rel = 0.15f; v->n[j].h2 = 0.05f; v->n[j].h3 = 0.0f;
            t += 0.35f + 0.6f * uni(); f *= 0.99f + 0.02f * uni();
        }
        break; }
    case BIRD_PARDALOTE: {                       /* "sleep - may - be", repeated */
        float k = 0.94f + 0.12f * uni();
        int reps = 2 + (int)(uni() * 1.99f);
        for (int r = 0; r < reps; r++) {
            j = add(v, t, 0.15f, 3500 * k, 3450 * k, 3350 * k, 0.45f);         v->n[j].att = 0.02f;  v->n[j].rel = 0.06f;
            j = add(v, t + 0.27f, 0.11f, 2950 * k, 2920 * k, 2900 * k, 0.40f); v->n[j].att = 0.015f; v->n[j].rel = 0.05f;
            j = add(v, t + 0.48f, 0.19f, 3050 * k, 3150 * k, 3180 * k, 0.45f); v->n[j].att = 0.02f;  v->n[j].rel = 0.08f;
            t += 2.2f + 1.2f * uni();
        }
        break; }
    case BIRD_WHISTLER: {                        /* wi-wi-wi-wi ... WHIT (or ee-CHONG) */
        float f = 1450.0f + 250.0f * uni();
        int k = 4 + (int)(uni() * 2.99f);
        for (int i = 0; i < k; i++) {
            j = add(v, t, 0.085f, f, f * 1.06f, f * 1.03f, 0.45f + 0.06f * (float)i);
            v->n[j].h2 = 0.12f;
            t += 0.15f; f *= 1.05f;
        }
        if (uni() < 0.6f) {                      /* the whip crack */
            j = add(v, t, 0.13f, f, f * 1.25f, f * 1.45f, 0.8f); v->n[j].h2 = 0.15f;
            j = add(v, t + 0.13f, 0.035f, f * 1.45f, f * 2.4f, f * 3.4f, 0.9f);
            v->n[j].att = 0.002f; v->n[j].rel = 0.012f; v->n[j].h2 = 0.35f; v->n[j].h3 = 0.2f;
        } else {                                 /* "ee-CHONG" */
            j = add(v, t, 0.24f, 2200, 2250, 2300, 0.7f); v->n[j].vr = 18; v->n[j].vd = 0.01f;
            j = add(v, t + 0.27f, 0.13f, 2600, 2000, 1250, 0.95f); v->n[j].pk = 0.3f; v->n[j].h2 = 0.25f; v->n[j].h3 = 0.12f;
        }
        break; }
    case BIRD_ROSELLA: default: {                /* bell-like "too-ee-too" whistles */
        float f = 2300.0f + 300.0f * uni();
        if (uni() < 0.7f) {
            j = add(v, t, 0.28f, f, f * 1.03f, f * 1.05f, 0.6f);                 v->n[j].vr = 5; v->n[j].vd = 0.008f; v->n[j].h2 = 0.14f;
            j = add(v, t + 0.34f, 0.22f, f * 1.28f, f * 1.32f, f * 1.30f, 0.65f); v->n[j].vr = 5; v->n[j].vd = 0.008f; v->n[j].h2 = 0.14f;
            if (uni() < 0.7f) {
                j = add(v, t + 0.62f, 0.32f, f * 1.08f, f * 1.05f, f * 0.98f, 0.6f); v->n[j].vr = 5; v->n[j].vd = 0.008f; v->n[j].h2 = 0.14f;
            }
        } else {                                 /* "cussik cussik" */
            for (int r = 0; r < 2; r++) {
                j = add(v, t, 0.09f, f * 1.5f, f * 1.2f, f * 0.95f, 0.55f); v->n[j].pk = 0.3f; v->n[j].h2 = 0.2f;
                j = add(v, t + 0.11f, 0.07f, f * 1.6f, f * 1.4f, f * 1.1f, 0.5f); v->n[j].h2 = 0.2f;
                t += 0.45f;
            }
        }
        break; }
    }
}

static void start_synth(int b, float dist)
{
    svoice_t *v = NULL;
    for (int i = 0; i < SVOICES; i++) if (!SV[i].on) { v = &SV[i]; break; }
    if (!v) return;                              /* all busy: skip this call */
    make_call(v, b);
    v->cur = 0; v->t = 0; v->ph = 0;
    v->gain = lerp(1.0f, 0.3f, dist);
    float fc = lerp(10000.0f, 3000.0f, dist);
    v->lp_a = 1.0f - expf(-6.2831853f * fc / SR);
    v->lp = 0;
    v->send = lerp(0.10f, 0.50f, dist);
    v->on = v->nn > 0;
}

static inline float synth_tick(svoice_t *v)
{
    while (v->cur < v->nn && v->t >= v->n[v->cur].t0 + v->n[v->cur].dur) v->cur++;
    if (v->cur >= v->nn) { v->on = false; return 0; }
    const note_t *n = &v->n[v->cur];
    float y = 0;
    float u = v->t - n->t0;
    if (u >= 0) {
        float x = u / n->dur;
        float f = x < n->pk ? n->fa + (n->fb - n->fa) * (x / n->pk)
                            : n->fb + (n->fc - n->fb) * ((x - n->pk) / (1.0f - n->pk));
        if (n->vd > 0) f *= 1.0f + n->vd * sine(u * n->vr);
        v->ph += f / SR;
        if (v->ph > 1024.0f) v->ph -= 1024.0f;
        float e = 1.0f;
        if (u < n->att) e = u / n->att;
        float r = n->dur - u;
        if (r < n->rel) e *= r / n->rel;
        y = (sine(v->ph) + n->h2 * sine(v->ph * 2.0f) + n->h3 * sine(v->ph * 3.0f)) * e * n->amp;
    }
    v->t += 1.0f / SR;
    return y;
}

/* ---- a small Schroeder reverb: the space around the birds ---- */
#define C1 558
#define C2 594
#define C3 638
#define C4 678
#define A1 278
#define A2 220
static float cb1[C1], cb2[C2], cb3[C3], cb4[C4], ab1[A1], ab2[A2];
static int   ci[4], ai[2];
static float cdamp[4];
static float s_tail;                /* reverb output level, for bs_active() */

static inline float comb(float *b, int n, int *i, float *d, float x)
{
    float y = b[*i];
    *d = y * 0.65f + *d * 0.35f;    /* damping */
    b[*i] = x + *d * 0.80f;
    if (++*i >= n) *i = 0;
    return y;
}
static inline float allpass(float *b, int n, int *i, float x)
{
    float y = b[*i];
    b[*i] = x + y * 0.5f;
    if (++*i >= n) *i = 0;
    return y - x;
}

/* ---- conductor ---- */
static float s_next[BIRD_COUNT];
static int   s_last_clip[BIRD_COUNT];

void bs_init(uint32_t seed)
{
    s_rng = seed ? seed : 0x2545F491u;
    for (int i = 0; i < 256; i++) {           /* G.711 mu-law decode */
        int u = ~i & 0xFF, t = ((u & 0x0F) << 3) + 0x84;
        t <<= (u & 0x70) >> 4;
        s_mu[i] = (int16_t)((u & 0x80) ? (0x84 - t) : (t - 0x84));
    }
    memset(V, 0, sizeof V);
    memset(SV, 0, sizeof SV);
    for (int i = 0; i <= SINE_N; i++) s_sin[i] = sinf(6.2831853f * (float)i / SINE_N);
    memset(cb1, 0, sizeof cb1); memset(cb2, 0, sizeof cb2); memset(cb3, 0, sizeof cb3); memset(cb4, 0, sizeof cb4);
    memset(ab1, 0, sizeof ab1); memset(ab2, 0, sizeof ab2);
    memset(ci, 0, sizeof ci); memset(ai, 0, sizeof ai); memset(cdamp, 0, sizeof cdamp);
    s_tail = 0;
    for (int b = 0; b < BIRD_COUNT; b++) { s_next[b] = SP[b].gap * uni(); s_last_clip[b] = -1; }
}

static int pick_clip(int b)
{
    int idx[16], n = 0;
    for (int i = 0; i < BIRD_CLIPS && n < 16; i++) if (BIRD_CLIP[i].species == b) idx[n++] = i;
    if (!n) return -1;
    int k = idx[rnd() % (uint32_t)n];
    for (int t = 0; t < 4 && n > 1 && k == s_last_clip[b]; t++) k = idx[rnd() % (uint32_t)n];   /* not the same clip twice running */
    s_last_clip[b] = k;
    return k;
}

/* Start a call. dist 0 = close and clear, 1 = far, dull and reverberant. */
static void start(int b, float dist)
{
    int c = pick_clip(b);
    if (c < 0) return;
    voice_t *v = NULL;
    for (int i = 0; i < VOICES; i++) if (!V[i].on) { v = &V[i]; break; }
    if (!v) {                                   /* steal the one nearest its end */
        v = &V[0];
        for (int i = 1; i < VOICES; i++)
            if ((float)V[i].len - V[i].pos < (float)v->len - v->pos) v = &V[i];
    }
    v->p = bird_data + BIRD_CLIP[c].off;
    v->len = BIRD_CLIP[c].len;
    v->pos = 0;
    v->rate = 1.0f + (uni() - 0.5f) * 0.08f;    /* +-4 % pitch and speed */
    v->gain = lerp(1.0f, 0.3f, dist);
    float fc = lerp(10000.0f, 3000.0f, dist);
    v->lp_a = 1.0f - expf(-6.2831853f * fc / SR);
    v->lp = 0;
    v->send = lerp(0.10f, 0.50f, dist);
    v->on = true;
}

void bs_call(int b)
{
    if (b < 0 || b >= BIRD_COUNT) return;
    if (bird_is_synth(b)) start_synth(b, 0.1f + 0.15f * uni()); else start(b, 0.1f + 0.15f * uni());
}

void bs_block(float I, uint16_t mask, float blk)
{
    if (I <= 0) {                               /* idle: keep the first calls spread out */
        for (int b = 0; b < BIRD_COUNT; b++) if (s_next[b] < 1.0f) s_next[b] = SP[b].gap * (0.3f + uni());
        return;
    }
    int busy = 0;
    for (int i = 0; i < VOICES; i++) busy += V[i].on;
    for (int i = 0; i < SVOICES; i++) busy += SV[i].on;
    int room = 1 + (int)(I * 5.0f);             /* more birds at once as it builds */
    for (int b = 0; b < BIRD_COUNT; b++) {
        if (!(mask & (1u << b))) continue;
        if (I <= SP[b].join) { if (s_next[b] < 2.0f) s_next[b] = 2.0f + SP[b].gap * uni(); continue; }
        s_next[b] -= blk;
        if (s_next[b] > 0) continue;
        float r = (I - SP[b].join) / (1.0f - SP[b].join);
        float gap = SP[b].gap * lerp(4.0f, 1.0f, r);
        s_next[b] = gap * (0.5f + uni());
        if (busy >= room) continue;             /* skip this one: too crowded */
        /* early on birds are further off; as it builds some come close */
        float dist = lerp(0.85f, 0.15f, I) * (0.6f + 0.8f * uni());
        if (bird_is_synth(b)) start_synth(b, dist > 1.0f ? 1.0f : dist);
        else start(b, dist > 1.0f ? 1.0f : dist);
        busy++;
    }
}

float bs_tick(void)
{
    float dry = 0, wet = 0;
    for (int i = 0; i < VOICES; i++) {
        voice_t *v = &V[i];
        if (!v->on) continue;
        uint32_t k = (uint32_t)v->pos;
        if (k + 1 >= v->len) { v->on = false; continue; }
        float fr = v->pos - (float)k;
        float s = ((float)s_mu[v->p[k]] * (1.0f - fr) + (float)s_mu[v->p[k + 1]] * fr) * (1.0f / 32768.0f);
        v->lp += v->lp_a * (s - v->lp);
        float y = v->lp * v->gain;
        dry += y;
        wet += y * v->send;
        v->pos += v->rate;
    }
    for (int i = 0; i < SVOICES; i++) {
        svoice_t *v = &SV[i];
        if (!v->on) continue;
        float s = synth_tick(v) * 0.32f;         /* about the level of the recorded clips */
        v->lp += v->lp_a * (s - v->lp);
        float y = v->lp * v->gain;
        dry += y;
        wet += y * v->send;
    }
    float r = comb(cb1, C1, &ci[0], &cdamp[0], wet) + comb(cb2, C2, &ci[1], &cdamp[1], wet)
            + comb(cb3, C3, &ci[2], &cdamp[2], wet) + comb(cb4, C4, &ci[3], &cdamp[3], wet);
    r = allpass(ab1, A1, &ai[0], r * 0.25f);
    r = allpass(ab2, A2, &ai[1], r);
    s_tail += (fabsf(r) - s_tail) * 0.001f;
    return dry + r * 0.8f;
}

bool bs_active(void)
{
    for (int i = 0; i < VOICES; i++) if (V[i].on) return true;
    for (int i = 0; i < SVOICES; i++) if (SV[i].on) return true;
    return s_tail > 1e-5f;
}
