/*
 * audio.c: sleep sounds on core 1. See audio.h.
 */
#include "audio.h"

#include <string.h>
#include "pico/stdlib.h"
#include "hardware/sync.h"

#define BLOCK      220                    /* 10 ms */
#define MAX_CATCH  (AUDIO_SR / 5)         /* after a stall, catch up at most 200 ms */

static int16_t            s_ring_sram[AUDIO_RING];
static int16_t           *s_ring = s_ring_sram;
static uint32_t           s_ring_n = AUDIO_RING;
static volatile uint32_t  s_wpos;         /* samples written, ever */
static volatile int       s_listeners;
static rs_params_t        s_params = { false, 0, 3, 5, 1.0f };
static volatile uint32_t  s_params_seq;   /* bumped by core 0 on change */
static repeating_timer_t  s_timer;
static uint64_t           s_t0_us, s_due;  /* sample clock */
static uint32_t           s_busy_us, s_win_us, s_cpu_pm;

static bool on_timer(repeating_timer_t *t)
{
    (void)t;
    uint64_t now = time_us_64();
    uint64_t due = (now - s_t0_us) * AUDIO_SR / 1000000u;     /* samples real time has asked for */
    if (s_listeners <= 0) { s_due = due; return true; }      /* nobody listening: keep the clock only */
    if (due - s_due > MAX_CATCH) s_due = due - MAX_CATCH;    /* long stall (flash write): skip ahead */

    rs_params_t p;
    uint32_t seq;
    do { seq = s_params_seq; __dmb(); p = s_params; __dmb(); } while ((seq & 1u) || seq != s_params_seq);

    while (s_due < due) {
        int n = (int)(due - s_due); if (n > BLOCK) n = BLOCK;
        uint32_t w = s_wpos, off = w % s_ring_n;
        if (off + (uint32_t)n > s_ring_n) n = (int)(s_ring_n - off);
        rs_render(&p, &s_ring[off], n);
        __dmb();
        s_wpos = w + (uint32_t)n;
        s_due += (uint64_t)n;
    }
    uint64_t end = time_us_64();
    s_busy_us += (uint32_t)(end - now);
    s_win_us  += 10000;
    if (s_win_us >= 1000000) { s_cpu_pm = s_busy_us * 1000u / s_win_us; s_busy_us = s_win_us = 0; }
    return true;
}

void audio_use_buffer(int16_t *buf, uint32_t samples)
{
    if (!buf || samples < AUDIO_RING) return;
    memset(buf, 0, samples * sizeof(int16_t));
    s_ring = buf; s_ring_n = samples;
}
uint32_t audio_ring_samples(void) { return s_ring_n; }

void audio_core1_start(void)
{
    rs_init(time_us_32() | 1u);
    s_t0_us = time_us_64();
    s_due = 0;
    /* A pool on its own hardware alarm, created here so its IRQ lands on core 1. */
    alarm_pool_t *pool = alarm_pool_create_with_unused_hardware_alarm(2);
    alarm_pool_add_repeating_timer_us(pool, -10000, on_timer, NULL, &s_timer);
}

void audio_set(const rs_params_t *p)
{
    if (!memcmp(p, &s_params, sizeof *p)) return;
    s_params_seq++; __dmb();
    s_params = *p;
    __dmb(); s_params_seq++;
}

void audio_listener(int delta) { s_listeners += delta; if (s_listeners < 0) s_listeners = 0; }
int  audio_listeners(void)     { return s_listeners; }
int  audio_cpu_permille(void)  { return s_listeners ? (int)s_cpu_pm : 0; }
uint32_t audio_write_pos(void) { return s_wpos; }

size_t audio_read(uint32_t *pos, int16_t *dst, size_t max)
{
    uint32_t w = s_wpos; __dmb();
    if (w - *pos > s_ring_n - BLOCK * 2) *pos = w - s_ring_n / 2;       /* fell behind: skip */
    size_t n = w - *pos;
    if (n > max) n = max;
    for (size_t i = 0; i < n; ) {
        uint32_t off = (*pos + (uint32_t)i) % s_ring_n;
        size_t run = s_ring_n - off; if (run > n - i) run = n - i;
        memcpy(dst + i, &s_ring[off], run * sizeof(int16_t));
        i += run;
    }
    *pos += (uint32_t)n;
    return n;
}
