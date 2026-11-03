/*
 * rainsynth.h: sleep sounds (rain, ocean, noise, wind, or a stored recording)
 * with thunder.
 *
 * Plain C with no Pico dependencies, so tools/render_rain.c can render it on a
 * host to audition and level-check. Mono, float internally, int16 out.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "birds.h"

#define RAIN_SR 22050

typedef enum { RS_RAIN = 0, RS_OCEAN = 1, RS_NOISE = 2, RS_WIND = 3, RS_RECORDING = 4, RS_RADIO = 5 } rs_kind_t;
#define RS_KINDS 6
typedef enum { RS_OPEN = 0, RS_LEAVES = 1, RS_ROOF = 2, RS_WINDOW = 3 } rs_surface_t;

typedef struct {
    bool     on;
    int      kind;         /* rs_kind_t */
    int      level;        /* 1..5 intensity: how hard it rains / how big the waves */
    int      volume;       /* 0..10 master */
    float    fade;         /* 0..1 extra gain (sleep timer's last minute) */
    /* rain shaping, 0..100 */
    int      patter;       /* loudness of individual drops */
    int      wash;         /* the steady hiss underneath */
    int      tone;         /* dark .. bright (also colours ocean and noise) */
    int      wind;         /* gusts and a wind layer */
    int      surface;      /* rs_surface_t: what the rain lands on */
    int      thunder;      /* how often thunder rolls: 0 never .. 100 every ~25 s */
    int      thunder_near; /* 0 far-off rumble .. 100 overhead with a crack */
    uint32_t strike;       /* change it to roll thunder now */
    /* the dawn chorus (birds.h) */
    float    chorus;       /* 0 off .. 1 full; the sleep sound fades out as it rises */
    uint16_t birds;        /* species mask (1 << bird_t) */
    int      bird_vol;     /* 0..10: the chorus has its own volume (it has to wake you) */
    uint32_t audition;     /* change it to play one call from audition_bird now */
    int      audition_bird;
    int      bird_soft;    /* 0 natural .. 100 soft: takes the edge off whistles and chirps (a low-pass on the birds alone) */
    /* speaker test: 0 none, 1 pink noise, 2 a 1 kHz tone, 3 a 100 Hz tone, 4 a
     * 100 Hz .. 8 kHz sweep; plays at the current volume whether or not the
     * sleep sound is on, mixed in over anything else */
    int      test;
    /* RS_RECORDING: an "ADP1" IMA ADPCM file (store.h), looped. NULL or not a
     * valid 22.05 kHz file: rain instead. Read in place (flash); the caller
     * swaps it out before that flash can change. */
    const uint8_t *rec;
    uint32_t       rec_len;
    /* RS_RADIO: pulls n mono samples at RAIN_SR of the decoded stream (radio.h);
     * zeros when there is nothing to play. NULL: the kind is silent. */
    int (*radio_pull)(int16_t *dst, int n);
} rs_params_t;

/* Sensible defaults for the shaping fields. */
#define RS_DEFAULTS .patter = 60, .wash = 55, .tone = 50, .wind = 20, .surface = RS_OPEN, \
                    .thunder = 0, .thunder_near = 35, .strike = 0, \
                    .chorus = 0, .birds = BIRDS_ALL, .bird_vol = 6, .audition = 0, .audition_bird = 0, .bird_soft = 35, .test = 0

void rs_init(uint32_t seed);
/* Render n samples with the given parameters. Changes glide, never click. */
void rs_render(const rs_params_t *p, int16_t *out, int n);
/* Seconds since the last thunder started (for UIs); large if none yet. */
float rs_thunder_age(void);
