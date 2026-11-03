/*
 * spk_dsp.c: see spk_dsp.h. Biquads from the RBJ audio EQ cookbook.
 */
#include "spk_dsp.h"

#include <math.h>
#include <string.h>

const int SPK_SOFT_HZ[6]   = { 0, 8000, 6000, 5000, 4000, 3000 };
const int SPK_LOWCUT_HZ[5] = { 0, 60, 90, 120, 160 };
/* Five bands about 1.6 octaves apart, the top one under the 11 kHz the
 * 22.05 kHz engine can make; each is a peaking filter about 1.5 octaves wide
 * (Q 0.9), so neighbours overlap a little and a smooth curve needs no steps. */
const int SPK_EQ_HZ[SPK_EQ_BANDS] = { 80, 250, 800, 2500, 7000 };
const char *const SPK_EQ_NAMES[SPK_EQ_BANDS] = { "80 Hz", "250 Hz", "800 Hz", "2.5 kHz", "7 kHz" };
#define EQ_Q 0.9f
/* Presets, in dB per band. Warm and Night take the edge off a small driver;
 * Voice lifts speech and station announcers; Bass is for rain and surf. */
const spk_preset_t SPK_PRESETS[SPK_N_PRESETS] = {
    { "Flat",   {  0,  0,  0,  0,  0 } },
    { "Warm",   {  3,  2,  0, -2, -4 } },
    { "Bright", { -2,  0,  0,  3,  4 } },
    { "Voice",  { -4, -2,  2,  4,  1 } },
    { "Night",  { -2, -1,  0, -4, -8 } },
    { "Bass",   {  6,  3,  0,  0,  0 } },
};

int spk_preset_match(const int *eq_db)
{
    for (int p = 0; p < SPK_N_PRESETS; p++) {
        int i = 0;
        while (i < SPK_EQ_BANDS && SPK_PRESETS[p].db[i] == eq_db[i]) i++;
        if (i == SPK_EQ_BANDS) return p;
    }
    return -1;
}

bool spk_tune_flat(const spk_tune_t *t)
{
    for (int i = 0; i < SPK_EQ_BANDS; i++) if (t->eq_db[i]) return false;
    return !t->gain_db && !t->soften && !t->low_cut && !t->limit;
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

/* Peaking EQ: +/- db at hz, Q wide. */
static void peak(biq_coef_t *c, float hz, float db, float q, int sr)
{
    float A = powf(10.0f, db / 40.0f), w0 = 6.2831853f * hz / (float)sr, cw = cosf(w0), alpha = sinf(w0) / (2.0f * q);
    norm(c, 1 + alpha * A, -2 * cw, 1 - alpha * A, 1 + alpha / A, -2 * cw, 1 - alpha / A);
}

void spk_coef(const spk_tune_t *t, int sr, spk_coef_t *c)
{
    memset(c, 0, sizeof *c);
    int soft = t->soften < 0 ? 0 : t->soften > 5 ? 5 : t->soften;
    int cut  = t->low_cut < 0 ? 0 : t->low_cut > 4 ? 4 : t->low_cut;
    if ((c->use_hp = cut > 0))        pass(&c->hp, (float)SPK_LOWCUT_HZ[cut], sr, true);
    for (int i = 0; i < SPK_EQ_BANDS; i++) {
        int db = t->eq_db[i] < -SPK_EQ_MAX_DB ? -SPK_EQ_MAX_DB : t->eq_db[i] > SPK_EQ_MAX_DB ? SPK_EQ_MAX_DB : t->eq_db[i];
        if ((c->use_eq[i] = db != 0)) peak(&c->eq[i], (float)SPK_EQ_HZ[i], (float)db, EQ_Q, sr);
    }
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
