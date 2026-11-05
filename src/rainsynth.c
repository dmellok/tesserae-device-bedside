/*
 * rainsynth.c: procedural sleep sounds. See rainsynth.h.
 *
 * Rain    a bed of filtered noise (the wash), a low body, and individual drops:
 *         each a short noise tick ringing a damped resonator, so every drop has
 *         its own pitch and decay. The surface sets the drops' pitch and ring
 *         (soft ground, leaves, a tin roof, a window); tone shifts everything
 *         darker or brighter; wind adds gusts that push the rain around and a
 *         moving whistle of its own.
 * Thunder rolls of low rumble: several overlapping swells with their own
 *         timing, through a filter that opens on each peak. Near strikes start
 *         with a crack and crackle, then the rain briefly gets heavier.
 * Ocean   brown noise through a filter that opens and closes with each wave;
 *         each wave has its own length and size, with foam hiss on the break.
 * Noise   pink-to-brown noise; low levels are deep, high levels brighter.
 * Wind    on its own: a breathy band of pink noise whose pitch and strength
 *         follow slow gusts, a low roar under it, a thin whistle in strong gusts.
 * Recording  a stored IMA ADPCM loop (store.h), level as loudness, tone as
 *         brightness; thunder can roll over it as over rain.
 *
 * Every gain glides, so switching sound, level or volume, or turning it on and
 * off, fades instead of clicking.
 */
#include "rainsynth.h"

#include <math.h>
#include <string.h>

#define SR ((float)RAIN_SR)

/* ---- noise ------------------------------------------------------------------ */
static uint32_t s_rng = 0x9E3779B9u;
static inline uint32_t rnd(void) { uint32_t x = s_rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return s_rng = x; }
static inline float white(void) { return (float)(int32_t)rnd() * (1.0f / 2147483648.0f); }   /* -1..1 */
static inline float uni(void)   { return (float)(rnd() >> 8) * (1.0f / 16777216.0f); }        /* 0..1 */
static inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
static inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

/* Paul Kellet's economy pink filter. */
typedef struct { float b0, b1, b2; } pink_t;
static inline float pink(pink_t *p, float w)
{
    p->b0 = 0.99765f * p->b0 + w * 0.0990460f;
    p->b1 = 0.96300f * p->b1 + w * 0.2965164f;
    p->b2 = 0.57000f * p->b2 + w * 1.0526913f;
    return (p->b0 + p->b1 + p->b2 + w * 0.1848f) * 0.25f;
}

/* Leaky integrator: brown noise. */
static inline float brown(float *s, float w) { *s = *s * 0.996f + w * 0.06f; return *s; }

/* One-pole low-pass; coefficient from cutoff. */
static inline float lp_coef(float hz) { return 1.0f - expf(-6.2831853f * hz / SR); }
static inline float lp(float *s, float x, float a) { *s += a * (x - *s); return *s; }

/* Chamberlin state-variable filter (low + band), cutoff as f = 2 sin(pi fc/sr). */
typedef struct { float low, band; } svf_t;
static inline float svf_f(float hz) { return 2.0f * sinf(3.14159265f * hz / SR); }
static inline void svf(svf_t *s, float x, float f, float q)
{
    s->low += f * s->band;
    float high = x - s->low - q * s->band;
    s->band += f * high;
}

/* ---- drops: damped resonators -------------------------------------------------- */
#define VOICES 28
typedef struct {
    float y1, y2, a, b;        /* y = a*y1 - b*y2 + exc */
    float exc, exc_decay;      /* excitation: a short decaying noise tick */
    float gain;
    int   life;                /* samples left */
} drop_t;
static drop_t s_v[VOICES];

static void drop_start(float hz, float tau_s, float amp, float tick_s)
{
    if (hz > SR * 0.42f) hz = SR * 0.42f;
    drop_t *v = NULL;
    for (int i = 0; i < VOICES; i++) if (s_v[i].life <= 0) { v = &s_v[i]; break; }
    if (!v) {                                   /* steal the quietest */
        v = &s_v[0];
        for (int i = 1; i < VOICES; i++) if (s_v[i].gain < v->gain) v = &s_v[i];
    }
    float r = expf(-1.0f / (tau_s * SR));
    v->a = 2.0f * r * cosf(6.2831853f * hz / SR);
    v->b = r * r;
    v->y1 = v->y2 = 0;
    v->exc = 1.0f;
    v->exc_decay = expf(-1.0f / (tick_s * SR));
    v->gain = amp * (1.0f - r) * 2.2f;          /* resonator peak ~ 1/(1-r): keep amp ~ output peak */
    v->life = (int)(tau_s * SR * 7.0f) + 64;
}

static inline float drops_tick(void)
{
    float sum = 0;
    for (int i = 0; i < VOICES; i++) {
        drop_t *v = &s_v[i];
        if (v->life <= 0) continue;
        float e = v->exc * white();
        v->exc *= v->exc_decay;
        float y = v->a * v->y1 - v->b * v->y2 + e;
        v->y2 = v->y1; v->y1 = y;
        sum += y * v->gain + e * v->gain * 0.35f;   /* a little raw tick on top */
        v->life--;
    }
    return sum;
}

/* What the rain lands on: drop pitch range, ring time, tick sharpness. */
typedef struct { float lo, hi, tau_lo, tau_hi, tick, plop_p, wash_k, drop_k; } surface_t;
static const surface_t SURF[4] = {
    /* open ground: soft, short, lots of wash */ { 900, 3200, 0.0012f, 0.004f, 0.0009f, 0.05f, 1.15f, 0.8f },
    /* leaves: crisp, dense, papery            */ { 1600, 5200, 0.0008f, 0.0025f, 0.0004f, 0.03f, 0.9f, 1.0f },
    /* tin roof: ringing, metallic             */ { 2400, 6800, 0.006f, 0.022f, 0.0003f, 0.02f, 0.7f, 0.75f },
    /* window: glassy plinks, fewer, bigger    */ { 2000, 6000, 0.003f, 0.009f, 0.0005f, 0.10f, 0.6f, 1.1f },
};

/* ---- thunder -------------------------------------------------------------------- */
#define PEAKS 7
typedef struct { float t0, amp, att, dec; } peak_t;
static struct {
    bool   on;
    float  t, dur, near;
    int    np;
    peak_t pk[PEAKS];
    float  env;                 /* per-block envelope */
    float  br, crack_lp, crackle_lp;
    svf_t  f1, f2;
} T;
static float    s_next_thunder = 30.0f;  /* seconds until the next one */
static float    s_thunder_age = 1e9f;
static uint32_t s_last_strike;
static float    s_storm = 0;             /* rain lifts for a while after a near strike */

float rs_thunder_age(void) { return s_thunder_age; }

static void thunder_start(float near)
{
    T.on = true; T.t = 0; T.near = clamp01(near);
    T.dur = lerp(10.0f, 6.0f, T.near) + 4.0f * uni();
    T.np = 3 + (int)(uni() * 4.99f);
    float first = lerp(0.6f, 0.0f, T.near) + 0.3f * uni();
    for (int k = 0; k < T.np; k++) {
        peak_t *p = &T.pk[k];
        p->t0  = k == 0 ? first : first + (T.dur * 0.7f) * uni() * (0.3f + 0.7f * (float)k / (float)T.np);
        p->amp = (k == 0 ? 1.0f : 0.35f + 0.55f * uni()) * (1.0f - 0.45f * (float)k / (float)T.np);
        p->att = lerp(0.45f, 0.03f, T.near) * (0.6f + 0.8f * uni());
        p->dec = 0.7f + 1.6f * uni();
    }
    s_thunder_age = 0;
    if (T.near > 0.55f) s_storm = 0.35f * T.near;
}

static float thunder_env(float t)
{
    float e = 0;
    for (int k = 0; k < T.np; k++) {
        float dt = t - T.pk[k].t0;
        if (dt <= 0) continue;
        e += T.pk[k].amp * (1.0f - expf(-dt / T.pk[k].att)) * expf(-dt / T.pk[k].dec);
    }
    return e;
}

/* ---- state ------------------------------------------------------------------- */
static pink_t s_pk;
static float  s_br, s_br2;
static float  s_hp_lo, s_wash_lp, s_body_lp, s_rumble_lp, s_out_lp, s_tone_lp;
static svf_t  s_sea, s_foam, s_wind;
static float  s_gust = 1.0f, s_gust_target = 1.0f;
static int    s_gust_left;
static float  s_wind_f = 500.0f, s_wind_target = 500.0f;
static float  s_wave_t, s_wave_len = 9.0f, s_wave_size = 1.0f;
static float  s_noise_lp;
/* wind on its own */
static pink_t s_wpk;
static svf_t  s_wbp, s_wwh;
static float  s_wbr, s_wroar, s_wg = 0.6f, s_wg_target = 0.6f, s_wf = 500.0f, s_wf_target = 500.0f;
static int    s_wg_left;
/* the recording: an ADPCM decoder over the file in flash */
static struct {
    const uint8_t *p;
    uint32_t len, n, bb, per, blk, k, pos;
    int      pred, idx;
    bool     ok;
} R;
static float  s_rec_lp;
static const int16_t ADP_STEP[89] = {7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,
    157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,
    2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,
    18500,20350,22385,24623,27086,29794,32767};
static const int8_t ADP_IDX[8] = {-1,-1,-1,-1,2,4,6,8};
static inline uint32_t rd32(const uint8_t *b) { return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }

/* Point the decoder at a file (or NULL); checks the header and the size. */
static void rec_open(const uint8_t *p, uint32_t len)
{
    memset(&R, 0, sizeof R);
    R.p = p; R.len = len;
    if (!p || len < 16 || rd32(p) != 0x31504441u || rd32(p + 4) != RAIN_SR) return;
    R.n = rd32(p + 8); R.bb = rd32(p + 12);
    if (R.n < 2 || R.bb < 8 || R.bb > 65536) return;
    R.per = 1 + 2 * (R.bb - 4);
    uint32_t blocks = (R.n + R.per - 1) / R.per;
    if (16 + (uint64_t)blocks * R.bb > len) return;
    R.ok = true;
}

static inline float rec_next(void)
{
    const uint8_t *b = R.p + 16 + R.blk * R.bb;
    if (R.k == 0) {                              /* a block starts with its first sample */
        R.pred = (int16_t)(b[0] | b[1] << 8);
        R.idx = b[2] > 88 ? 88 : b[2];
    } else {
        uint32_t c = R.k - 1;
        int code = (b[4 + (c >> 1)] >> ((c & 1) * 4)) & 15;
        int st = ADP_STEP[R.idx], dq = st >> 3;
        if (code & 4) dq += st;
        if (code & 2) dq += st >> 1;
        if (code & 1) dq += st >> 2;
        R.pred += (code & 8) ? -dq : dq;
        R.pred = R.pred > 32767 ? 32767 : R.pred < -32768 ? -32768 : R.pred;
        R.idx += ADP_IDX[code & 7];
        R.idx = R.idx < 0 ? 0 : R.idx > 88 ? 88 : R.idx;
    }
    float v = (float)R.pred * (1.0f / 32768.0f);
    if (++R.pos >= R.n) { R.pos = 0; R.blk = 0; R.k = 0; }      /* the end: loop (the web app blended it) */
    else if (++R.k >= R.per) { R.k = 0; R.blk++; }
    return v;
}

/* Gliding gains and controls. */
static float  g_rain, g_ocean, g_noise, g_master, g_birds, g_wind, g_rec;
static uint32_t s_last_audition;
static float    s_bird_lp1, s_bird_lp2;    /* bird softness: two poles on the bird bus alone */
/* the speaker test signal (rs_params_t.test): its own pink noise and phase */
static float    g_test;
static float    g_radio;                   /* the radio bed's gain */
static int16_t  s_radio_buf[1024];         /* this block of the decoded stream */
static pink_t   s_tpk;
static float    s_test_ph, s_sweep_t;
static float  s_level_s = 3.0f, s_patter = 0.6f, s_wash = 0.55f, s_tone = 0.5f, s_windk = 0.2f;

/* ---- the alarm sound: a short pattern that repeats while the alarm rings ----
 * Synthesised, so there is nothing to store: a voice (how a note sounds) and a
 * score (when each note starts, how long it lasts, its pitch). Each voice
 * keeps one envelope per note: beeps are gated, bells and marimba decay. */
const char *const RS_TONE_NAMES[RS_TONES] = { "Chorus only", "Beep", "Digital", "Chime", "Marimba", "Rising" };
typedef struct { float t, dur, hz; } tn_note_t;
typedef struct { int voice; float period; int n; const tn_note_t *notes; } tn_pattern_t;
enum { TV_BEEP, TV_BELL, TV_MARIMBA, TV_SOFT };
#define TN_MAX 8
static const tn_note_t N_BEEP[]    = { {0, 0.12f, 1000}, {0.25f, 0.12f, 1000}, {0.5f, 0.12f, 1000}, {0.75f, 0.12f, 1000} };
static const tn_note_t N_DIGITAL[] = { {0, 0.06f, 2200}, {0.1f, 0.06f, 2200}, {0.2f, 0.06f, 2200}, {0.5f, 0.06f, 2200}, {0.6f, 0.06f, 2200}, {0.7f, 0.06f, 2200} };
static const tn_note_t N_CHIME[]   = { {0, 1.8f, 523.25f}, {0.45f, 1.8f, 659.25f}, {0.9f, 2.6f, 783.99f} };                       /* C5 E5 G5 */
static const tn_note_t N_MARIMBA[] = { {0, 0.5f, 392.0f}, {0.22f, 0.5f, 523.25f}, {0.44f, 0.5f, 659.25f}, {0.66f, 0.9f, 783.99f},
                                       {1.4f, 0.5f, 659.25f}, {1.62f, 0.9f, 523.25f} };                                           /* G4 C5 E5 G5, E5 C5 */
static const tn_note_t N_RISING[]  = { {0, 0.5f, 329.63f}, {0.4f, 0.5f, 392.0f}, {0.8f, 0.5f, 493.88f}, {1.2f, 0.5f, 587.33f}, {1.6f, 1.2f, 659.25f} };   /* E4 G4 B4 D5 E5 */
static const tn_pattern_t TONES[RS_TONES] = {
    { TV_SOFT, 1.0f, 0, NULL },
    { TV_BEEP, 2.0f, 4, N_BEEP }, { TV_BEEP, 1.6f, 6, N_DIGITAL }, { TV_BELL, 4.0f, 3, N_CHIME },
    { TV_MARIMBA, 3.2f, 6, N_MARIMBA }, { TV_SOFT, 3.6f, 5, N_RISING },
};
static float g_tone, s_tn_t, s_tn_ph[TN_MAX], s_tn_amp[TN_MAX];
static bool  s_tn_on[TN_MAX];

/* The next sample of the pattern; dec_bell / dec_mar: the per-sample decay of those voices. */
static float tone_tick(const tn_pattern_t *tp, float dec_bell, float dec_mar)
{
    float sig = 0;
    for (int k = 0; k < tp->n && k < TN_MAX; k++) {
        const tn_note_t *nt = &tp->notes[k];
        float age = s_tn_t - nt->t;
        if (age < 0 || age >= nt->dur) { s_tn_on[k] = false; continue; }
        if (!s_tn_on[k]) { s_tn_on[k] = true; s_tn_ph[k] = 0; s_tn_amp[k] = 1.0f; }
        s_tn_ph[k] += nt->hz / SR; if (s_tn_ph[k] >= 1.0f) s_tn_ph[k] -= 1.0f;
        float ph = 6.2831853f * s_tn_ph[k], v;
        switch (tp->voice) {
        case TV_BEEP: {                                             /* a clock radio: a square-ish pip, gated with 4 ms ramps */
            float r = 0.004f, env = age < r ? age / r : nt->dur - age < r ? (nt->dur - age) / r : 1.0f;
            v = (sinf(ph) + 0.35f * sinf(3.0f * ph)) * 0.5f * env; break; }
        case TV_BELL:                                               /* a chime: the strike, an inharmonic partial, a long ring */
            s_tn_amp[k] *= dec_bell;
            v = (sinf(ph) + 0.45f * sinf(2.76f * ph) * s_tn_amp[k] + 0.2f * sinf(5.4f * ph) * s_tn_amp[k]) * 0.5f * s_tn_amp[k] * (age < 0.003f ? age / 0.003f : 1.0f);
            break;
        case TV_MARIMBA:                                            /* a wooden bar: quick decay, a bright tap at the start */
            s_tn_amp[k] *= dec_mar;
            v = (sinf(ph) + 0.4f * sinf(4.0f * ph) * s_tn_amp[k] * s_tn_amp[k]) * 0.7f * s_tn_amp[k] * (age < 0.002f ? age / 0.002f : 1.0f);
            break;
        default: {                                                  /* a soft tone: 40 ms in, 120 ms out */
            float rel = nt->dur - 0.12f, env = age < 0.04f ? age / 0.04f : age > rel ? (nt->dur - age) / 0.12f : 1.0f;
            v = (sinf(ph) + 0.2f * sinf(2.0f * ph)) * 0.45f * env; break; }
        }
        sig += v;
    }
    s_tn_t += 1.0f / SR;
    if (s_tn_t >= tp->period) s_tn_t -= tp->period;
    return sig;
}



void rs_init(uint32_t seed)
{
    s_rng = seed ? seed : 0x9E3779B9u;
    memset(s_v, 0, sizeof s_v);
    memset(&T, 0, sizeof T);
    s_next_thunder = 30.0f; s_thunder_age = 1e9f; s_last_strike = 0; s_storm = 0;
    memset(&s_pk, 0, sizeof s_pk); s_br = s_br2 = 0;
    s_hp_lo = s_wash_lp = s_body_lp = s_rumble_lp = s_out_lp = s_tone_lp = s_noise_lp = 0;
    memset(&s_sea, 0, sizeof s_sea); memset(&s_foam, 0, sizeof s_foam); memset(&s_wind, 0, sizeof s_wind);
    s_gust = s_gust_target = 1.0f; s_gust_left = 0; s_wind_f = s_wind_target = 500.0f;
    s_wave_t = 0; s_wave_len = 9.0f; s_wave_size = 1.0f;
    g_rain = g_ocean = g_noise = g_master = g_wind = g_rec = 0;
    memset(&s_wpk, 0, sizeof s_wpk); memset(&s_wbp, 0, sizeof s_wbp); memset(&s_wwh, 0, sizeof s_wwh);
    s_wbr = s_wroar = 0; s_wg = s_wg_target = 0.6f; s_wf = s_wf_target = 500.0f; s_wg_left = 0;
    rec_open(NULL, 0); s_rec_lp = 0;
    s_level_s = 3.0f; s_patter = 0.6f; s_wash = 0.55f; s_tone = 0.5f; s_windk = 0.2f;
    g_birds = 0; s_last_audition = 0;
    s_bird_lp1 = s_bird_lp2 = 0; g_test = 0; g_radio = 0; s_test_ph = s_sweep_t = 0; memset(&s_tpk, 0, sizeof s_tpk);
    g_tone = 0; s_tn_t = 0; memset(s_tn_ph, 0, sizeof s_tn_ph); memset(s_tn_amp, 0, sizeof s_tn_amp); memset(s_tn_on, 0, sizeof s_tn_on);
    bs_init(seed * 2654435761u + 1u);
}

/* Wave envelope: slow swell to a break, then a long wash back. t in 0..1. */
static float wave_env(float t)
{
    if (t < 0.45f) { float u = t / 0.45f; return u * u * (3.0f - 2.0f * u) * 0.75f; }      /* swell */
    if (t < 0.55f) { float u = (t - 0.45f) / 0.10f; return 0.75f + 0.25f * sinf(u * 1.5707963f); } /* break */
    float u = (t - 0.55f) / 0.45f; return (1.0f - u) * (1.0f - u) * 1.0f + 0.05f;         /* wash */
}

void rs_render(const rs_params_t *p, int16_t *out, int n)
{
    const float glide = 1.0f / (SR * 1.2f);     /* ~1.2 s fades */
    const float blk = (float)n / SR;            /* this block, in seconds */
    int   kind  = p->kind;
    /* the dawn chorus: the sleep sound fades out under it as it builds */
    float chorus = clamp01(p->chorus);
    float under = 1.0f - clamp01(chorus * 1.6f);
    float tr = (p->on && kind == RS_RAIN)  ? under : 0.0f;
    /* rain also stands in for a recording that isn't there (below) */
    float to = (p->on && kind == RS_OCEAN) ? under : 0.0f;
    float tn = (p->on && kind == RS_NOISE) ? under : 0.0f;
    float tw = (p->on && kind == RS_WIND)  ? under : 0.0f;
    float tq = (p->on && kind == RS_RADIO && p->radio_pull) ? under : 0.0f;
    if (p->rec != R.p || p->rec_len != R.len) rec_open(p->rec, p->rec_len);
    float tc = (p->on && kind == RS_RECORDING && R.ok) ? under : 0.0f;
    if (p->on && kind == RS_RECORDING && !R.ok) tr = under;          /* no usable recording: rain */
    if (p->audition != s_last_audition) { s_last_audition = p->audition; bs_call(p->audition_bird); }
    bs_block(chorus, p->birds, blk);
    float bv = (p->bird_vol < 0 ? 0 : p->bird_vol > 10 ? 10 : (float)p->bird_vol) / 10.0f;
    /* The chorus rises evenly in loudness: -30 dB as it starts, full at the ring
     * (-6 dB in the last minute). It was 0.35..1 in amplitude, only 9 dB of
     * rise, so the first birds already sounded almost at full. */
    float tb = powf(bv, 1.5f) * 1.8f * (chorus > 0 ? powf(10.0f, -1.5f * (1.0f - chorus)) : 1.0f);
    bool birds_on = chorus > 0 || bs_active();
    /* softness: 0 leaves the birds alone (9 kHz, nothing to hear at 22 kHz), 100 rolls them off above ~1.6 kHz */
    float a_bird = lp_coef(9000.0f * powf(0.18f, clamp01(p->bird_soft / 100.0f)));
    float tt = p->test > 0 && p->test <= 4 ? 1.0f : 0.0f;
    /* the alarm sound: the pattern chosen, at the chorus's volume, as loud as tone_level says */
    const tn_pattern_t *tp = &TONES[p->ring > 0 && p->ring < RS_TONES ? p->ring : 0];
    float ta = tp->n ? clamp01(p->ring_level) : 0.0f;
    float tg = powf(bv, 1.5f) * 0.9f;
    float dec_bell = expf(-2.4f / SR), dec_mar = expf(-8.0f / SR);
    float vol = p->volume < 0 ? 0 : p->volume > 10 ? 10 : (float)p->volume;
    float tm = (vol / 10.0f) * (vol / 10.0f) * clamp01(p->fade);
    float lvl_t = (float)(p->level < 1 ? 1 : p->level > 5 ? 5 : p->level);

    /* Per-block control glides (~10 ms blocks, ~0.5 s to settle). */
    const float cg = 0.02f;
    s_level_s += (lvl_t - s_level_s) * cg;
    s_patter  += (clamp01(p->patter / 100.0f) - s_patter) * cg;
    s_wash    += (clamp01(p->wash / 100.0f) - s_wash) * cg;
    s_tone    += (clamp01(p->tone / 100.0f) - s_tone) * cg;
    s_windk   += (clamp01(p->wind / 100.0f) - s_windk) * cg;
    float L = (s_level_s - 1.0f) / 4.0f;        /* 0..1 */
    const surface_t *sf = &SURF[p->surface < 0 || p->surface > 3 ? 0 : p->surface];
    float tone_k = lerp(0.6f, 1.5f, s_tone);    /* pitch / brightness multiplier */

    /* storm lift after a near strike decays over ~20 s */
    s_storm *= expf(-blk / 20.0f);
    float Ls = clamp01(L + s_storm);

    /* rain */
    float rate    = (18.0f + 260.0f * Ls * Ls + 40.0f * Ls) * sf->drop_k / SR;
    float a_hp    = lp_coef(500.0f);
    float a_wash  = lp_coef((2600.0f + 3400.0f * Ls) * tone_k);
    float a_body  = lp_coef(900.0f * tone_k);
    float a_rum   = lp_coef(110.0f);
    float g_wash  = (0.26f + 0.22f * Ls) * sf->wash_k * lerp(0.0f, 1.8f, s_wash);
    float g_body  = (0.12f + 0.12f * Ls) * lerp(0.4f, 1.4f, s_wash);
    float g_rum   = 0.35f * Ls * Ls;
    float g_drops = (0.50f - 0.18f * Ls) * lerp(0.0f, 1.7f, s_patter);
    float a_tone  = lp_coef(lerp(3500.0f, 10000.0f, s_tone));
    /* wind: gust depth and speed, and the whistle's level */
    float gust_depth = 0.15f + 0.75f * s_windk;
    float g_whistle  = 0.20f * s_windk * s_windk;
    /* noise colour: brown at level 1, pink at 5, tone shifts it */
    float a_noise = lp_coef((250.0f + 2200.0f * L) * tone_k);

    /* wind on its own: gust walk per block */
    float a_roar = lp_coef(160.0f * tone_k);
    float a_rec  = lp_coef(lerp(2500.0f, 11000.0f, s_tone));
    float g_recl = lerp(0.55f, 1.0f, L);

    /* ocean: wave clock advances per block */
    float size_gain = 0.55f + 0.45f * L;
    if (g_ocean > 1e-4f || to > 0) {
        s_wave_t += blk / s_wave_len;
        if (s_wave_t >= 1.0f) { s_wave_t -= 1.0f; s_wave_len = 7.0f + 6.0f * uni(); s_wave_size = 0.6f + 0.4f * uni(); }
    }

    /* thunder: schedule, trigger, and the block's envelope + filter */
    s_thunder_age += blk;
    float thunder_k = clamp01(p->thunder / 100.0f);
    float near_k = clamp01(p->thunder_near / 100.0f);
    bool stormy = tr > 0 || tc > 0;              /* thunder rolls over rain and recordings */
    if (p->strike != s_last_strike) { s_last_strike = p->strike; if (stormy) thunder_start(near_k + (uni() - 0.5f) * 0.3f); }
    if (stormy && thunder_k > 0 && !T.on) {
        s_next_thunder -= blk;
        if (s_next_thunder <= 0) {
            thunder_start(near_k + (uni() - 0.5f) * 0.4f);
            float mean = lerp(240.0f, 25.0f, thunder_k);
            s_next_thunder = mean * (0.4f + 1.2f * uni());
        }
    } else if (thunder_k <= 0) s_next_thunder = 20.0f + 20.0f * uni();
    float th_env = 0, th_f = 0, th_g = 0, crack = 0;
    if (T.on) {
        th_env = thunder_env(T.t);
        th_f = svf_f(55.0f + (70.0f + 260.0f * T.near) * clamp01(th_env));
        th_g = (0.16f + 0.22f * T.near) * th_env;
        crack = T.near > 0.55f ? (T.near - 0.55f) * 2.2f : 0.0f;
    }

    /* the radio: this block of the decoded stream, pulled once (nothing while it is faded out) */
    bool radio_on = p->radio_pull && (tq > 0 || g_radio > 1e-4f);
    int radio_n = n > (int)(sizeof s_radio_buf / sizeof s_radio_buf[0]) ? (int)(sizeof s_radio_buf / sizeof s_radio_buf[0]) : n;
    if (radio_on) p->radio_pull(s_radio_buf, radio_n);

    for (int i = 0; i < n; i++) {
        g_rain   += (tr - g_rain)  * glide * 4.0f;
        g_ocean  += (to - g_ocean) * glide * 4.0f;
        g_noise  += (tn - g_noise) * glide * 4.0f;
        g_master += (tm - g_master) * glide * 2.0f;
        g_birds  += (tb - g_birds) * glide * 2.0f;
        g_wind   += (tw - g_wind)  * glide * 4.0f;
        g_rec    += (tc - g_rec)   * glide * 4.0f;
        g_test   += (tt - g_test)  * glide * 6.0f;
        g_radio  += (tq - g_radio) * glide * 4.0f;
        g_tone   += (ta - g_tone)  * glide * 6.0f;
        if (g_master < 1e-5f && tm == 0.0f && g_rain + g_ocean + g_noise + g_wind + g_rec < 1e-4f && !T.on && !birds_on && g_tone < 1e-4f && ta == 0.0f) { out[i] = 0; continue; }

        float w = white();
        float x = 0;

        if (g_rain > 1e-4f) {
            /* gusts: a slow random walk; windier = deeper and quicker */
            if (--s_gust_left <= 0) {
                s_gust_target = 1.0f - gust_depth * 0.5f + gust_depth * uni();
                s_gust_left = (int)(SR * lerp(5.0f, 1.5f, s_windk) * (0.5f + uni()));
                s_wind_target = 280.0f + 700.0f * uni();
            }
            s_gust += (s_gust_target - s_gust) * (1.0f / (SR * lerp(1.8f, 0.6f, s_windk)));
            s_wind_f += (s_wind_target - s_wind_f) * (1.0f / (SR * 2.0f));

            float pk = pink(&s_pk, w);
            float hp = pk - lp(&s_hp_lo, pk, a_hp);
            float wash = lp(&s_wash_lp, hp, a_wash) * g_wash * s_gust;
            float body = lp(&s_body_lp, pk, a_body) * g_body;
            float rum  = lp(&s_rumble_lp, brown(&s_br, w), a_rum) * g_rum;

            if (uni() < rate * s_gust) {
                float u = uni();
                float amp = 0.10f + 0.9f * u * u * u;               /* mostly light, a few heavy */
                if (uni() < sf->plop_p) drop_start((380.0f + 520.0f * uni()) * tone_k, 0.018f + 0.02f * uni(), amp * 1.4f, 0.0015f);
                else drop_start(lerp(sf->lo, sf->hi, uni()) * tone_k, lerp(sf->tau_lo, sf->tau_hi, uni()), amp, sf->tick);
            }
            float drops = drops_tick() * g_drops;

            float whistle = 0;
            if (g_whistle > 1e-4f) {
                svf(&s_wind, w, svf_f(s_wind_f * (0.7f + 0.6f * s_gust)), 0.18f);
                whistle = s_wind.band * g_whistle * (s_gust - 0.4f);
            }
            x += (lp(&s_tone_lp, wash + body + drops, a_tone) + rum + whistle) * g_rain;
        } else {
            drops_tick();                                           /* let any tails die */
        }

        if (T.on) {
            /* rumble: brown noise through two low-pass stages that open with the roll */
            float b = brown(&T.br, w) * 4.0f;
            svf(&T.f1, b, th_f, 1.1f);
            svf(&T.f2, T.f1.low, th_f, 1.1f);
            float th = T.f2.low * th_g;
            if (crack > 0 && T.t < 0.6f) {
                float hpw = w - lp(&T.crack_lp, w, 0.25f);          /* bright burst at the strike */
                th += hpw * crack * expf(-T.t / 0.07f) * 0.35f;
                if (uni() < 0.012f) T.crackle_lp += (uni() - 0.5f) * crack * 0.6f;  /* crackle */
                T.crackle_lp *= 0.985f;
                th += T.crackle_lp * expf(-T.t / 0.25f);
            }
            float bed = g_rain > g_rec ? g_rain : g_rec;
            x += th * (bed > 0.05f ? bed : 0.05f);
            T.t += 1.0f / SR;
            if (T.t > T.dur + 3.0f) T.on = false;
        }

        if (g_ocean > 1e-4f) {
            float t = s_wave_t + (float)i / SR / s_wave_len;
            float e = wave_env(t > 1.0f ? 1.0f : t) * s_wave_size;
            float b = brown(&s_br2, w) * 3.0f;
            svf(&s_sea, b, svf_f((180.0f + 1500.0f * e * size_gain) * tone_k), 1.2f);
            float foam_in = w * e * e;
            svf(&s_foam, foam_in, svf_f(3500.0f * tone_k), 1.4f);
            x += (s_sea.low * (0.25f + 0.9f * e) * size_gain + s_foam.band * 0.35f * size_gain) * 0.26f * g_ocean;
        }

        if (g_wind > 1e-4f) {
            if (--s_wg_left <= 0) {                                /* the next gust: how strong, how long, what pitch */
                s_wg_target = (0.25f + 0.75f * uni()) * (0.55f + 0.45f * L);
                s_wg_left = (int)(SR * (1.5f + 5.0f * uni()) * lerp(1.4f, 0.7f, L));
                s_wf_target = (260.0f + 700.0f * uni()) * (0.8f + 0.5f * L);
            }
            s_wg += (s_wg_target - s_wg) * (1.0f / (SR * 2.2f));
            s_wf += (s_wf_target - s_wf) * (1.0f / (SR * 3.0f));
            float pk = pink(&s_wpk, w);
            svf(&s_wbp, pk, svf_f(s_wf * tone_k * (0.6f + 0.8f * s_wg)), 0.45f);
            float roar = lp(&s_wroar, brown(&s_wbr, w), a_roar);
            float wx = s_wbp.band * 1.1f * s_wg + roar * 0.7f * (0.35f + 0.65f * s_wg);
            if (s_wg > 0.55f) {                                    /* a thin whistle at the top of strong gusts */
                svf(&s_wwh, w, svf_f(s_wf * 2.4f * tone_k), 0.05f);
                wx += s_wwh.band * 0.06f * (s_wg - 0.55f) * 2.2f;
            }
            x += wx * lerp(0.22f, 0.6f, L) * g_wind;
        }

        if (g_rec > 1e-4f && R.ok) {
            x += lp(&s_rec_lp, rec_next(), a_rec) * g_recl * 0.8f * g_rec;
        }

        if (radio_on && i < radio_n) {                             /* intensity trims it: 1 is -4 dB, 5 full */
            x += (float)s_radio_buf[i] * (1.0f / 32768.0f) * lerp(0.6f, 1.0f, L) * g_radio;
        }

        if (g_noise > 1e-4f) {
            float pk = pink(&s_pk, w);
            float br = brown(&s_br, w) * 2.0f;
            float mix = pk * (0.3f + 0.7f * L) + br * (1.0f - 0.6f * L);
            x += lp(&s_noise_lp, mix, a_noise) * 0.2f * g_noise;
        }

        x *= g_master * 0.9f;
        if (birds_on) x += lp(&s_bird_lp2, lp(&s_bird_lp1, bs_tick(), a_bird), a_bird) * g_birds * 0.9f;
        if (g_tone > 1e-4f || ta > 0.0f) {                        /* the alarm sound over the chorus, the rest held back under it */
            float sig = tone_tick(tp, dec_bell, dec_mar);
            x = x * (1.0f - 0.55f * g_tone) + sig * tg * g_tone;
        } else if (s_tn_t != 0.0f) { s_tn_t = 0; memset(s_tn_on, 0, sizeof s_tn_on); }   /* next time, from the top of the pattern */
        if (g_test > 1e-4f) {                                     /* the speaker test, faded in over the mix */
            float sig = 0, f = 0;
            switch (p->test) {
            case 1: sig = pink(&s_tpk, w) * 0.5f; break;
            case 2: f = 1000.0f; break;
            case 3: f = 100.0f; break;
            case 4: s_sweep_t += 1.0f / SR; if (s_sweep_t >= 5.0f) s_sweep_t = 0;      /* 100 Hz to 8 kHz in 5 s, over and over */
                    f = 100.0f * powf(80.0f, s_sweep_t / 5.0f); break;
            default: break;
            }
            if (f > 0) { s_test_ph += f / SR; if (s_test_ph >= 1.0f) s_test_ph -= 1.0f; sig = sinf(6.2831853f * s_test_ph); }
            x = x * (1.0f - g_test) + sig * 0.35f * g_master * g_test;
        }
        /* gentle soft clip */
        float ax = fabsf(x);
        if (ax > 0.6f) x = (x > 0 ? 1.0f : -1.0f) * (0.6f + (ax - 0.6f) / (1.0f + (ax - 0.6f) * 2.5f));
        x = lp(&s_out_lp, x, 0.85f);                              /* tame aliasing fizz */
        int v = (int)(x * 32767.0f);
        out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}
