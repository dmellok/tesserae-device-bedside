/*
 * rainsynth.h: procedural sleep sounds (rain, ocean, noise) with thunder.
 *
 * Plain C with no Pico dependencies, so tools/render_rain.c can render it on a
 * host to audition and level-check. Mono, float internally, int16 out.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "birds.h"

#define RAIN_SR 22050

typedef enum { RS_RAIN = 0, RS_OCEAN = 1, RS_NOISE = 2 } rs_kind_t;
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
} rs_params_t;

/* Sensible defaults for the shaping fields. */
#define RS_DEFAULTS .patter = 60, .wash = 55, .tone = 50, .wind = 20, .surface = RS_OPEN, \
                    .thunder = 0, .thunder_near = 35, .strike = 0, \
                    .chorus = 0, .birds = BIRDS_ALL, .bird_vol = 6, .audition = 0, .audition_bird = 0

void rs_init(uint32_t seed);
/* Render n samples with the given parameters. Changes glide, never click. */
void rs_render(const rs_params_t *p, int16_t *out, int n);
/* Seconds since the last thunder started (for UIs); large if none yet. */
float rs_thunder_age(void);
