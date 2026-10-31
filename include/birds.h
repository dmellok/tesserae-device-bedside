/*
 * birds.h: an Australian dawn chorus from real recordings, plus a few
 * synthesised species for variety.
 *
 * Short clips of six species (assets/birds, sources and licences in
 * NOTICES.md) are kept in flash as 8-bit mu-law and arranged live: which clip,
 * when, a slightly different pitch, and a random distance that sets loudness,
 * dullness and how much of the small outdoor reverb it gets. So no two mornings
 * repeat. A conductor schedules calls from an intensity 0..1: at low intensity
 * a single magpie sings with long gaps; as it rises more species join, gaps
 * shorten and the chorus gets louder; the kookaburras come in near the top.
 *
 * Plain C (tools/render_rain.c renders it on a host with the same clips).
 * Mono at RAIN_SR. Mixed into the sleep-sound engine by rainsynth.c.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef enum {               /* the order of assets/birds/clips.json */
    BIRD_MAGPIE = 0,         /* Australian magpie: warbling carol, the dawn leader */
    BIRD_BUTCHERBIRD,        /* grey butcherbird: rich piping phrases */
    BIRD_FAIRYWREN,          /* superb fairy-wren: high reel */
    BIRD_WHIPBIRD,           /* eastern whipbird: long whistle, whip crack */
    BIRD_WATTLEBIRD,         /* red wattlebird: harsh "chock" coughs */
    BIRD_KOOKABURRA,         /* laughing kookaburra: a family laughing */
    /* synthesised (whistles and pings synthesise well; birds.c builds each call) */
    BIRD_BELLMINER,          /* bell miner: pure "tink" pings, a colony */
    BIRD_PARDALOTE,          /* spotted pardalote: soft "sleep-may-be" */
    BIRD_WHISTLER,           /* golden whistler: rising whistles, then a whip crack */
    BIRD_ROSELLA,            /* crimson rosella: bell-like "too-ee-too" */
    BIRD_COUNT
} bird_t;

#define BIRDS_ALL ((uint16_t)((1u << BIRD_COUNT) - 1))
#define BIRD_FIRST_SYNTH BIRD_BELLMINER
#define BIRDS_SYNTH ((uint16_t)(BIRDS_ALL & ~((1u << BIRD_FIRST_SYNTH) - 1)))
static inline bool bird_is_synth(int b) { return b >= BIRD_FIRST_SYNTH; }

const char *bird_name(int b);

void  bs_init(uint32_t seed);
/* Once per block: schedule calls for the chorus at `intensity` (0 = none)
 * from the species in `mask`. */
void  bs_block(float intensity, uint16_t mask, float block_s);
/* Start one call from bird b now, close by (audition). */
void  bs_call(int b);
/* Next sample of the chorus (dry plus reverb), roughly -1..1. */
float bs_tick(void);
/* True while any call or reverb tail is still sounding. */
bool  bs_active(void);
