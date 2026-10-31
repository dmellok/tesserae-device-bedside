/*
 * painter.c: the panel driver on core 1. See painter.h.
 *
 * Hand-off is shared memory plus SEV/WFE. The inter-core FIFO stays free: the
 * SDK's multicore lockout uses it, which flash_safe_execute() needs so that
 * config saves can pause core 1 while flash is being written.
 */
#include "painter.h"

#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/mutex.h"
#include "hardware/sync.h"

static mutex_t        s_panel;               /* whoever drives the IT8951 holds it */
static it8951_row_fn  s_fn;
static painter_job_t  s_job;
static volatile bool  s_pending;             /* core 0 -> core 1: s_job is ready */
static volatile bool  s_running;             /* submitted and not yet finished */
static volatile bool  s_done;                /* core 1 -> core 0: finished */
static bool           s_started;
static void         (*s_core1_init)(void);

#define CORE1_STACK_WORDS 2048               /* 8 KB: row generator + driver */
static uint32_t s_core1_stack[CORE1_STACK_WORDS];

static void run(const painter_job_t *j)
{
    switch (j->kind) {
    case PJ_FULL:  it8951_show_pattern(s_fn, NULL, j->mode); break;
    case PJ_AREAS: if (j->n > 0) it8951_update_areas((const int (*)[4])j->rects, j->n, s_fn, NULL, j->mode); break;
    case PJ_CLEAR: it8951_clear(j->mode); break;
    case PJ_REDISPLAY: it8951_redisplay_areas((const int (*)[4])j->rects, j->n, j->mode); break;
    }
}

static void core1_main(void)
{
    multicore_lockout_victim_init();         /* let flash writes on core 0 pause us */
    if (s_core1_init) s_core1_init();        /* audio: timer IRQs on this core */
    for (;;) {
        while (!s_pending) __wfe();
        __dmb();
        painter_job_t j = s_job;
        s_pending = false;
        mutex_enter_blocking(&s_panel);
        run(&j);
        mutex_exit(&s_panel);
        __dmb();
        s_done = true;
        s_running = false;
        __sev();
    }
}

void painter_init(void) { mutex_init(&s_panel); }

void painter_start(it8951_row_fn fn, void (*core1_init)(void))
{
    if (s_started) return;
    s_fn = fn;
    s_core1_init = core1_init;
    s_started = true;
    multicore_launch_core1_with_stack(core1_main, s_core1_stack, sizeof s_core1_stack);
}

bool painter_busy(void) { return s_running; }

bool painter_submit(const painter_job_t *job)
{
    if (s_running) return false;
    if (!s_started) {                        /* before core 1 is up: paint inline */
        mutex_enter_blocking(&s_panel);
        run(job);
        mutex_exit(&s_panel);
        s_done = true;
        return true;
    }
    s_job = *job;
    s_running = true;
    __dmb();
    s_pending = true;
    __sev();
    return true;
}

bool painter_take_done(void)
{
    if (!s_done) return false;
    s_done = false;
    return true;
}

void painter_lock(void)   { mutex_enter_blocking(&s_panel); }
void painter_unlock(void) { mutex_exit(&s_panel); }
