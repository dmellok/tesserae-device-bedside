/*
 * pcm_resample.c: see pcm_resample.h.
 */
#include "pcm_resample.h"

void pcmrs_init(pcmrs_t *s) { s->pos = 0; s->carry_n = 0; }

/* Mono frame i of the virtual input: the carried frames, then this chunk. */
static inline int mono_at(const pcmrs_t *s, const int16_t *in, int channels, int i)
{
    if (i < s->carry_n) return s->carry[i];
    i -= s->carry_n;
    if (channels == 1) return in[i];
    int a = 0;
    for (int c = 0; c < channels; c++) a += in[i * channels + c];
    return a / channels;
}

int pcmrs_run(pcmrs_t *s, const int16_t *in, int frames, int channels, int in_rate, int out_rate, int16_t *out, int max_out)
{
    if (frames <= 0 || channels <= 0 || in_rate <= 0 || out_rate <= 0) return 0;
    float step = (float)in_rate / (float)out_rate;
    int total = s->carry_n + frames, n = 0;
    float p = s->pos;
    while (n < max_out && p + step <= (float)total) {
        int a = (int)p, b = (int)(p + step);             /* average the span [a, b) */
        if (b <= a) b = a + 1;
        if (b > total) b = total;
        long sum = 0;
        for (int i = a; i < b; i++) sum += mono_at(s, in, channels, i);
        out[n++] = (int16_t)(sum / (b - a));
        p += step;
    }
    /* keep the frames from floor(p) on for the next call, and the fraction */
    int from = (int)p, keep = total - from;
    if (keep > (int)(sizeof s->carry / sizeof s->carry[0])) { from = total - (int)(sizeof s->carry / sizeof s->carry[0]); keep = total - from; p = (float)from; }
    int16_t tmp[sizeof s->carry / sizeof s->carry[0]];
    for (int i = 0; i < keep; i++) tmp[i] = (int16_t)mono_at(s, in, channels, from + i);
    for (int i = 0; i < keep; i++) s->carry[i] = tmp[i];
    s->carry_n = keep;
    s->pos = p - (float)from;
    return n;
}
