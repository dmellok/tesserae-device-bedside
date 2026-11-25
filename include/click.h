/*
 * click.h: the knob's click, a 4 ms tick mixed into the speaker's output
 *
 * The knob is silent to turn in the dark; with Settings > Speaker > Knob click
 * on, each detent ticks, always quietly (CLICK_LEVEL in audio.h), whatever the volume. It is added where the samples leave for the speaker
 * (src/audio.c's I2S buffers, the simulator's audio queue), after the sound
 * generator and its ring, so it is as close to the turn as the output buffer
 * allows: about a tenth of a second on the device.
 *
 * click_ask() on core 0; click_next() once a sample where the output is filled.
 */
#pragma once
#include <stdint.h>
#include <math.h>

#define CLICK_LEN  96           /* samples: 4.4 ms at 22.05 kHz */
#define CLICK_GAP  400          /* between two, so a fast spin is a run of ticks and not a buzz */

typedef struct {
    int16_t  wave[CLICK_LEN];   /* full scale; built on the first ask */
    volatile uint32_t asked;
    volatile int level;         /* 0..10, the volume it was asked at */
    uint32_t served;
    int pos;                    /* where the one playing is; CLICK_GAP: none */
    bool built;
} click_t;

static inline void click_ask(click_t *c, int level)
{
    if (!c->built) {
        for (int k = 0; k < CLICK_LEN; k++)
            c->wave[k] = (int16_t)(32000.0f * expf(-(float)k / 14.0f) * sinf(6.2831853f * 2400.0f * (float)k / 22050.0f + 0.6f));
        c->pos = CLICK_GAP; c->built = true;
    }
    c->level = level;
    c->asked++;
}

/* The next sample of the click playing, 0 when there is none. */
static inline int click_next(click_t *c)
{
    if (!c->built) return 0;
    if (c->pos >= CLICK_GAP) {
        uint32_t a = c->asked;
        if (a == c->served) return 0;
        if (a - c->served > 3) c->served = a - 3;      /* a long spin while it could not play: not all of it later */
        c->served++; c->pos = 0;
    }
    int k = c->pos++;
    return k < CLICK_LEN ? c->wave[k] * c->level * 6 / 100 : 0;      /* volume 10: a little over half scale */
}
