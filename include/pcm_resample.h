/*
 * pcm_resample.h: decoded radio (stereo, 32-48 kHz) to the panel's mono
 * 22.05 kHz, in plain C (host-tested). Channels are averaged; the rate is
 * dropped by averaging each output sample's span of input (a box filter: a
 * cheap anti-alias that is plenty for a small bedside speaker), with the
 * fractional position carried across calls so chunks join seamlessly.
 */
#pragma once
#include <stdint.h>

typedef struct {
    float   pos;             /* fractional position into the carried frames + the next chunk */
    int16_t carry[8];        /* the last few mono frames of the previous chunk: a span may straddle chunks */
    int     carry_n;
} pcmrs_t;

void pcmrs_init(pcmrs_t *s);
/* in: frames * channels interleaved samples at in_rate. Writes up to max_out
 * mono samples at out_rate; returns how many. Leftover position is kept. */
int  pcmrs_run(pcmrs_t *s, const int16_t *in, int frames, int channels, int in_rate, int out_rate, int16_t *out, int max_out);
