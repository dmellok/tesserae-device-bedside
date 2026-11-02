/*
 * spk_dsp.h: the speaker's tone stage, applied to every sample on its way to
 * the amp (audio.c): a low cut, bass and treble shelves, a "soften" low-pass
 * for sharp highs, a level trim and a peak limiter. Plain C with no Pico
 * dependencies; tests/test_sound.c runs it on the host.
 *
 * Coefficients are worked out on core 0 (spk_coef) and applied per sample on
 * core 1 (spk_sample); the state carries the filters' memory and the gain,
 * which glides so a slider never clicks.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int gain_db;    /* -12..+6: level trim */
    int bass_db;    /* -8..+8: shelf below 200 Hz */
    int treble_db;  /* -12..+6: shelf above 3 kHz */
    int soften;     /* 0 off, 1..5: low-pass at SPK_SOFT_HZ[soften] */
    int low_cut;    /* 0 off, 1..4: high-pass at SPK_LOWCUT_HZ[low_cut] */
    int limit;      /* 0 off, 1 gentle (holds peaks to -6 dB), 2 firm (-12 dB) */
} spk_tune_t;

extern const int SPK_SOFT_HZ[6];      /* 0, 8000, 6000, 5000, 4000, 3000 */
extern const int SPK_LOWCUT_HZ[5];    /* 0, 60, 90, 120, 160 */

typedef struct { float b0, b1, b2, a1, a2; } biq_coef_t;    /* normalised: a0 = 1 */
typedef struct { float z1, z2; } biq_state_t;               /* transposed direct form II */

typedef struct {
    biq_coef_t hp, ls, hs, lp;
    bool  use_hp, use_ls, use_hs, use_lp;
    float gain;          /* linear */
    float limit_thr;     /* 0 = off */
    float limit_rel;     /* envelope release per sample */
    bool  active;        /* anything to do at all */
} spk_coef_t;

typedef struct {
    biq_state_t hp, ls, hs, lp;
    float gain;          /* glides towards coef.gain */
    float env, lim_g;    /* limiter envelope and its gain */
} spk_state_t;

bool spk_tune_flat(const spk_tune_t *t);
void spk_coef(const spk_tune_t *t, int sr, spk_coef_t *c);
void spk_state_init(spk_state_t *s, const spk_coef_t *c);

static inline float biq_run(const biq_coef_t *c, biq_state_t *s, float x)
{
    float y = c->b0 * x + s->z1;
    s->z1 = c->b1 * x - c->a1 * y + s->z2;
    s->z2 = c->b2 * x - c->a2 * y;
    return y;
}

/* One sample, full scale 1.0, in and out. */
static inline float spk_sample(const spk_coef_t *c, spk_state_t *s, float x)
{
    if (c->use_hp) x = biq_run(&c->hp, &s->hp, x);
    if (c->use_ls) x = biq_run(&c->ls, &s->ls, x);
    if (c->use_hs) x = biq_run(&c->hs, &s->hs, x);
    if (c->use_lp) x = biq_run(&c->lp, &s->lp, x);
    s->gain += (c->gain - s->gain) * 0.0005f;               /* ~90 ms glide */
    x *= s->gain;
    if (c->limit_thr > 0) {
        float a = x < 0 ? -x : x;
        s->env = a > s->env ? a : s->env * c->limit_rel;      /* peak hold, slow release */
        float gt = s->env > c->limit_thr ? c->limit_thr / s->env : 1.0f;
        s->lim_g += (gt - s->lim_g) * (gt < s->lim_g ? 0.5f : 0.01f);   /* fast down, gentle up */
        x *= s->lim_g;
    }
    return x;
}

/* n samples in place (host tests). */
void spk_process(const spk_coef_t *c, spk_state_t *s, float *x, int n);
