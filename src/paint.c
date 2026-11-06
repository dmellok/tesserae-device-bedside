/*
 * paint.c: when and how the panel is refreshed: the painter's snapshot and pump, the clean-refresh policy, clears and full repaints
 */
#include "app_internal.h"

/* What core 1 paints from: everything composite_row() reads, frozen at submit
 * (painter.h). Core 0 is free to change the live state meanwhile. */
paint_snap_t P;

static void snapshot(void)
{
    P.content = cur_content;
    P.frame = tesserae_frame();
    P.banner = tune_banner;
    memcpy(P.text, banner_text, sizeof P.text);
    if (P.content == CONTENT_BOOT) P.boot = s_boot;
    local_snapshot();
}

void paint_row(int y, uint8_t out[IT8951_ROW_BYTES], void *ctx)
{
    (void)ctx;
    if (P.content == CONTENT_BOOT) { boot_row(y, out, &P.boot); return; }
    if (P.content == CONTENT_SETUP || P.content == CONTENT_FLASHING) { setup_row(y, out); return; }
    if (P.content == CONTENT_DASHBOARD && P.frame) dash_row(y, out, P.frame);
    else if (P.content == CONTENT_WHITE || P.content == CONTENT_LIVE) memset(out, 0xFF, IT8951_ROW_BYTES);
    else                                           IT8951_PATTERNS[3].fn(y, out, NULL);   /* the test card: grey blocks */
    local_snapshot_row(y, out);
    if (P.banner && P.text[0]) banner_overlay_row(P.text, y, out);
}

/* Painting runs on core 1 (painter.h); paint_pump() hands it the next job
 * whenever it is idle. Input is never queued for replay: knob turns made during
 * a small update change the state straight away and the next job paints the
 * latest of it, and input made while the screen changes to another page is
 * dropped (see the epoch check), so the panel never plays back a chain of stale
 * changes. A full GC16 clean-up runs after many non-flashing updates, once the
 * device is idle. */
bool     s_flush_pending;
static uint8_t  s_clear_mode = 0xFF;    /* a clear waiting for the painter */
static uint32_t s_painted_epoch;
static uint32_t s_partials;
uint32_t s_last_input_ms;
/* Partial updates leave the glass a little lighter each time. After the user
 * has been doing things (knob, keys, web app, HA), one full GC16 refresh once
 * input has settled puts the blacks back. Background updates (the live page's
 * clock, HA values) don't count: they get the slower clean-up below, so the
 * panel doesn't flash every minute at night. */
static bool     s_touched;
bool     s_job_screen;           /* the job in flight changes page */
static bool     s_job_counts, s_job_resets;
static uint32_t s_last_full_ms;                 /* when a full INIT + GC16 was last submitted */
/* A screen change or full refresh comes out faded on this glass until a
 * second pass runs the waveform over it again; the image is still in the
 * controller, so the pass costs only the waveform. */
static painter_job_t s_echo;
static bool          s_job_echo, s_echo_due;
uint8_t       s_echo_mode = 0xFF;   /* off: a GL16 pass streaked dithered greys (/api/cmd?echo=gl16|gc16 to try) */
void local_flush(void) { s_flush_pending = true; }

void paint_pump(void)
{
    if (!painter_busy() && it8951_bitmap() != local_state()->load_1bpp) it8951_set_bitmap(local_state()->load_1bpp);   /* only between jobs */
    if (painter_take_done()) {
        if (s_job_resets) s_partials = 0; else if (s_job_counts) s_partials++;
        if (s_job_screen) inputs_discard();     /* taps and turns made while the page changed */
        if (s_job_echo) s_echo_due = true;
        s_job_screen = s_job_counts = s_job_resets = s_job_echo = false;
    }
    if (painter_busy()) return;
    if (s_echo_due && !s_flush_pending && s_clear_mode == 0xFF) {
        s_echo_due = false;
        painter_submit(&s_echo);
        return;
    }
    s_echo_due = false;                         /* newer work replaces it */

    painter_job_t j = { 0 };
    if (s_clear_mode != 0xFF) {
        j.kind = PJ_CLEAR; j.mode = s_clear_mode; s_clear_mode = 0xFF;
        s_job_resets = true;
        painter_submit(&j);
        return;
    }
    if (!s_flush_pending) return;
    s_flush_pending = false;

    local_dirty_t d;
    bool full = false, all_du = true;
    j.mode = IT8951_MODE_GL16;
    while (local_take_dirty(&d)) {
        if (d.full) { full = true; j.mode = d.mode; continue; }
        if (j.n < PAINTER_MAX_RECTS) { j.rects[j.n][0] = d.x; j.rects[j.n][1] = d.y; j.rects[j.n][2] = d.w; j.rects[j.n][3] = d.h; j.n++; }
        if (d.mode == IT8951_MODE_GC16) j.mode = d.mode;
        if (d.mode != IT8951_MODE_DU) all_du = false;
    }
    if (!full && !j.n) return;
    /* Two-level jobs: DU. A2 (quick_a2, API only) is faster but on this panel it left old
     * ink under new, digits stacking on the Live clock; menus only, never the clocks. */
    if (!full && all_du) j.mode = (local_state()->quick_a2 && local_screen() != SCR_HOME) ? IT8951_MODE_A2 : IT8951_MODE_DU;
    /* Screen updates: Clean draws every change with GC16 (just its area), Full
     * repaints the whole screen with GC16 every time. Both keep blacks black. */
    if (cur_content == CONTENT_BOOT) { if (!full) j.mode = IT8951_MODE_DU; }   /* the boot log: fast, black and white */
    else if (local_refresh_mode() == 2) { full = true; j.mode = IT8951_MODE_GC16; }
    else if (j.mode == IT8951_MODE_GL16) j.mode = IT8951_MODE_GC16;   /* every mode: GL16 lightens greys pass by pass and is no faster here (723 vs 741 ms) */
    j.kind = full ? PJ_FULL : PJ_AREAS;
    s_job_resets = full && j.mode == IT8951_MODE_GC16;
    if (s_job_resets) s_last_full_ms = to_ms_since_boot(get_absolute_time());
    {
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (s_job_resets) s_touched = false;
        else if (now - s_last_input_ms < 3000u) s_touched = true;   /* a partial the user caused: a clean refresh follows once they stop */
    }
    s_job_counts = !full;                               /* every partial wears its area slightly lighter on this panel: count them all */
    s_job_screen = local_epoch() != s_painted_epoch;
    s_painted_epoch = local_epoch();
    if (s_echo_mode != 0xFF && s_job_screen && !full) {      /* menus; a full refresh has its INIT */
        s_echo = (painter_job_t){ 0 };
        s_echo.kind = PJ_REDISPLAY; s_echo.mode = s_echo_mode;
        if (full) { s_echo.n = 1; s_echo.rects[0][0] = 0; s_echo.rects[0][1] = 0; s_echo.rects[0][2] = IT8951_W; s_echo.rects[0][3] = IT8951_H; }
        else { s_echo.n = j.n; memcpy(s_echo.rects, j.rects, sizeof j.rects); }
        s_job_echo = true;
    }
    printf("paint: job %s mode %u, %d rect(s)%s:", full ? "full" : "areas", j.mode, j.n, all_du && !full ? " (quick)" : "");
    for (int i = 0; i < j.n; i++) printf(" [%d,%d %dx%d]", j.rects[i][0], j.rects[i][1], j.rects[i][2], j.rects[i][3]);
    printf("\n");
    snapshot();
    painter_submit(&j);
}

/* Kept for callers that want the paint started now rather than next pass. */
static void local_flush_now(void) { s_flush_pending = true; paint_pump(); }

void cleanup_if_idle(void)
{
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (!local_state()->stop_clean) s_touched = false;   /* Settings: Clean-up when you stop, off */
    if (s_touched && !painter_busy() && !s_flush_pending && now - s_last_input_ms >= SETTLE_MS) {
        printf("paint: input settled, full refresh\n");
        s_touched = false;
        s_partials = 0;
        local_set_dirty_full();
        local_flush_now();
        return;
    }
    /* Residual drift from partials: a full INIT + GC16 resets it. Every
     * clean_min minutes with any, or sooner after 3 per minute of that (30 at
     * the default 10; so a once-a-minute clock never runs ahead of a longer
     * setting), once the panel has been idle 20 s (the flash is just the
     * panel, no light, so nights are fine). */
    uint32_t mins = (uint32_t)(local_state()->clean_min > 0 ? local_state()->clean_min : 10);
    uint32_t interval = mins * 60u * 1000u;
    bool due = s_partials >= 3u * mins || (s_partials > 0 && now - s_last_full_ms > interval);
    if (due && !painter_busy() && !s_flush_pending && now - s_last_input_ms > 20000 && local_screen() == SCR_HOME) {
        painter_lock(); int board = it8951_sense_temp(); painter_unlock();     /* the night's temperature, for the log and the waveform choice */
        printf("paint: full clean refresh after %lu partial updates, board %d C, waveform %d C, VCOM %d mV\n",
               (unsigned long)s_partials, board, it8951_temp_forced(), (int)it8951_cached_vcom());
        s_partials = 0; s_last_full_ms = now;
        local_set_dirty_full();
        local_flush_now();
    }
}

/* The banner says where the web UI is and, until the device is paired and has
 * a frame, what it is waiting for. Once the dashboard is up it goes quiet. */
/* ---- painting --------------------------------------------------------------- */

void repaint(void)
{
    banner_compose();
    local_set_dirty_full();
    local_flush_now();
}

void show_content(content_t c)
{
    cur_content = c;
    printf("paint: %s\n", content_name());
    repaint();
}

void clear_screen(uint8_t mode)
{
    printf("paint: %s clear\n", mode == IT8951_MODE_INIT ? "INIT" : "GC16");
    cur_content = CONTENT_WHITE;
    { local_dirty_t d; while (local_take_dirty(&d)) {} }   /* the clear covers them */
    s_flush_pending = false;
    s_clear_mode = mode;
    paint_pump();
}

/* New frame downloaded: show it. A full GC16 paint, as the server's frame is
 * a complete page; partial patches are a later protocol step. */
void on_frame(void)
{
    if (cur_content == CONTENT_BOOT) { printf("paint: dashboard frame %s ready\n", tesserae_frame_etag()); return; }
    if (local_live_active()) {                 /* the live page is up: keep the frame for when it is chosen */
        printf("paint: dashboard frame %s kept (live page showing)\n", tesserae_frame_etag());
        return;
    }
    local_frame_arrived();
    cur_content = CONTENT_DASHBOARD;
    printf("paint: dashboard frame %s\n", tesserae_frame_etag());
    repaint();
}
