/*
 * spk_dsp.c: see spk_dsp.h. Biquads from the RBJ audio EQ cookbook.
 */
#include "spk_dsp.h"

#include <math.h>
#include <string.h>

const int SPK_SOFT_HZ[6]   = { 0, 8000, 6000, 5000, 4000, 3000 };
const int SPK_LOWCUT_HZ[5] = { 0, 60, 90, 120, 160 };

bool spk_tune_flat(const spk_tune_t *t)
{
    return !t->gain_db && !t->bass_db && !t->treble_db && !t->soften && !t->low_cut && !t->limit;
}

static void norm(biq_coef_t *c, float b0, float b1, float b2, float a0, float a1, float a2)
{
    c->b0 = b0 / a0; c->b1 = b1 / a0; c->b2 = b2 / a0; c->a1 = a1 / a0; c->a2 = a2 / a0;
}

/* Second-order Butterworth high- or low-pass. */
static void pass(biq_coef_t *c, float hz, int sr, bool high)
{
    float w0 = 6.2831853f * hz / (float)sr, cw = cosf(w0), alpha = sinf(w0) / (2.0f * 0.7071f);
    if (high) norm(c, (1 + cw) / 2, -(1 + cw), (1 + cw) / 2, 1 + alpha, -2 * cw, 1 - alpha);
    else      norm(c, (1 - cw) / 2,   1 - cw,  (1 - cw) / 2, 1 + alpha, -2 * cw, 1 - alpha);
}

/* Shelf (slope 1): below hz for the low shelf, above it for the high shelf. */
static void shelf(biq_coef_t *c, float hz, float db, int sr, bool high)
{
    float A = powf(10.0f, db / 40.0f), w0 = 6.2831853f * hz / (float)sr, cw = cosf(w0);
    float alpha = sinf(w0) / 2.0f * sqrtf(2.0f), sa = 2.0f * sqrtf(A) * alpha;
    if (!high) norm(c,  A * ((A + 1) - (A - 1) * cw + sa),  2 * A * ((A - 1) - (A + 1) * cw),  A * ((A + 1) - (A - 1) * cw - sa),
                        (A + 1) + (A - 1) * cw + sa,       -2 * ((A - 1) + (A + 1) * cw),       (A + 1) + (A - 1) * cw - sa);
    else       norm(c,  A * ((A + 1) + (A - 1) * cw + sa), -2 * A * ((A - 1) + (A + 1) * cw),  A * ((A + 1) + (A - 1) * cw - sa),
                        (A + 1) - (A - 1) * cw + sa,        2 * ((A - 1) - (A + 1) * cw),       (A + 1) - (A - 1) * cw - sa);
}

void spk_coef(const spk_tune_t *t, int sr, spk_coef_t *c)
{
    memset(c, 0, sizeof *c);
    int soft = t->soften < 0 ? 0 : t->soften > 5 ? 5 : t->soften;
    int cut  = t->low_cut < 0 ? 0 : t->low_cut > 4 ? 4 : t->low_cut;
    if ((c->use_hp = cut > 0))        pass(&c->hp, (float)SPK_LOWCUT_HZ[cut], sr, true);
    if ((c->use_ls = t->bass_db != 0))   shelf(&c->ls, 200.0f, (float)t->bass_db, sr, false);
    if ((c->use_hs = t->treble_db != 0)) shelf(&c->hs, 3000.0f, (float)t->treble_db, sr, true);
    if ((c->use_lp = soft > 0))       pass(&c->lp, (float)SPK_SOFT_HZ[soft], sr, false);
    c->gain = powf(10.0f, (float)t->gain_db / 20.0f);
    c->limit_thr = t->limit <= 0 ? 0.0f : t->limit == 1 ? 0.5f : 0.25f;
    c->limit_rel = expf(-1.0f / ((float)sr * 0.15f));            /* 150 ms release */
    c->active = !spk_tune_flat(t);
}

void spk_state_init(spk_state_t *s, const spk_coef_t *c)
{
    memset(s, 0, sizeof *s);
    s->gain = c->gain; s->lim_g = 1.0f;
}

void spk_process(const spk_coef_t *c, spk_state_t *s, float *x, int n)
{
    for (int i = 0; i < n; i++) x[i] = spk_sample(c, s, x[i]);
}
