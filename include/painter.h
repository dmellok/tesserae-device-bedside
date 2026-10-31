/*
 * painter.h: the panel driver runs on core 1.
 *
 * Core 0 keeps the knob, keys, network and UI state; core 1 streams pixels to
 * the IT8951 and waits out its refresh. One job runs at a time. Core 0 submits
 * a job only when core 1 is idle, so jobs never queue up behind each other: the
 * UI keeps changing state while a paint runs, and the next job paints whatever
 * is current by then (latest wins).
 *
 * The row generator a job uses must read only a snapshot taken before submit,
 * never state core 0 is still changing.
 *
 * Audio, when it lands, also lives on core 1, in DMA interrupt context at a
 * higher priority than this loop. Painting is thread-mode background work that
 * those interrupts simply pre-empt; the IT8951 tolerates pauses in SPI.
 *
 * Direct panel calls from core 0 (VCOM, init, sleep, diagnostics) go through
 * painter_lock()/painter_unlock(), which waits for the job in progress.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "it8951.h"

#define PAINTER_MAX_RECTS 9

typedef enum { PJ_AREAS, PJ_FULL, PJ_CLEAR, PJ_REDISPLAY } painter_kind_t;   /* REDISPLAY: run the waveform again over rects, no load */

typedef struct {
    painter_kind_t kind;
    uint8_t        mode;                        /* IT8951_MODE_* */
    int            n;                           /* PJ_AREAS: rect count */
    int            rects[PAINTER_MAX_RECTS][4]; /* x, y, w, h */
} painter_job_t;

/* Call once, before any panel access (sets up the lock). */
void painter_init(void);
/* Start core 1 with the row generator jobs will use; core1_init (may be NULL)
 * runs on core 1 first, e.g. to put the audio timer's interrupt there. */
void painter_start(it8951_row_fn fn, void (*core1_init)(void));

bool painter_busy(void);
/* Hand a job to core 1. Returns false (and does nothing) while one is running. */
bool painter_submit(const painter_job_t *job);
/* True once after each job completes; core 0 runs its follow-ups then. */
bool painter_take_done(void);

void painter_lock(void);
void painter_unlock(void);
