/*
 * audio.h: the sleep-sound generator on core 1, for now streamed to the web
 * app (GET /audio) since the board has no speaker yet.
 *
 * A timer interrupt on core 1 renders 10 ms blocks into a ring of samples,
 * tracking real time so it never drifts; it pre-empts the painter (painter.h).
 * Rendering runs only while someone is listening: the web stream, or the
 * speaker (a MAX98357A on I2S: GP18 BCLK, GP19 LRCLK, GP21 data, GP16 the
 * amp's shutdown), which reads the same ring through DMA.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "rainsynth.h"

#define AUDIO_SR    RAIN_SR
#define AUDIO_RING  16384u               /* samples: ~0.74 s (the SRAM ring without PSRAM) */
/* Use a bigger ring (PSRAM), before core 1 starts: a long stall on core 0 (a
 * frame download) then no longer gaps the web stream. */
void     audio_use_buffer(int16_t *buf, uint32_t samples);
uint32_t audio_ring_samples(void);

/* Core 1: start the render timer (call from core 1). */
void audio_core1_start(void);

/* Core 0: the sound to make, from the UI state. */
void audio_set(const rs_params_t *p);

/* Streaming readers (core 0, lwIP context). Positions are absolute sample
 * counts; the ring holds the last AUDIO_RING of them. */
void     audio_listener(int delta);      /* +1 on connect, -1 on close */
uint32_t audio_write_pos(void);
/* Copy up to max samples from *pos (advanced; skipped ahead if overrun).
 * Returns samples copied. */
size_t   audio_read(uint32_t *pos, int16_t *dst, size_t max);

int      audio_listeners(void);

/* The speaker. Pins first (before core 1; the amp starts shut down). Core 0
 * says whether there's anything to hear and whether the speaker is wanted;
 * the tick switches the amp, keeping it on ~3 s after the sound stops so
 * fades play out. */
void     audio_init_pins(void);
void     audio_speaker_enable(bool on);
void     audio_speaker_audible(bool audible);
void     audio_speaker_tick(void);
bool     audio_speaker_playing(void);
uint32_t audio_speaker_fills(void);   /* I2S buffers refilled so far (diagnostics) */
int      audio_cpu_permille(void);       /* render time as a share of core 1 */
