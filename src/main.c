/*
 * tesserae-device-bedside: firmware for the Bedside Dashboard.
 *
 * Pico Plus 2 W + Waveshare 10.3" IT8951 HAT (1872x1404, 16 greys). Pulls its
 * dashboard from a Tesserae server (src/tesserae.c, docs/tesserae.md) into
 * PSRAM and paints it; draws the menu and a status banner itself as overlays
 * with the panel's partial refresh; serves a bench web page over WiFi
 * (src/net.c, www/index.html) with a live copy of the panel and the tunables.
 *
 * Inputs: the I2C rotary encoder and the NeoKey 1x4 on the Qw/ST connector
 * (src/inputs.c), the Pico's BOOTSEL/BOOT button (flash-CS trick and GP45),
 * an optional push button on GP15, the USB serial console (see help()), and
 * the web UI's virtual controller, which drives exactly the same handlers.
 *
 * Every input is BINDABLE: each key's press and hold, the knob's two turn
 * directions, press and hold, and the BOOT button's short and long press each
 * run an action from the table in run_action() (menu navigation, refresh,
 * content, lights, or "button:<name>", a server button wake that the Tesserae
 * Controls map turns into a page / rotation step / HA action). Bindings and
 * the key labels drawn in the strip along the top edge live in the flash
 * config and are edited from the web UI (and, in Phase 3, retained MQTT).
 * Defaults give the proposal's grammar: knob turn = move, press = select,
 * hold = back; keys = prev / next / refresh / menu.
 *
 * Everything on the panel goes through one row generator, composite_row():
 * the content (dashboard frame or test card), then the key label strip, then
 * the banner, then the menu.
 * Overlays therefore need no frame buffer, partial updates slice any rectangle
 * out of it, and the web UI's preview is the same generator decimated.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/stdio.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "tusb.h"                 /* cdc_line_coding_t: the 1200-baud reset */

#include "it8951.h"
#include "epd_io.h"
#include "ota.h"
#include "psram_heap.h"
#include "cJSON.h"
#include "boot_logo.h"
#include "pico/unique_id.h"
#include "hardware/structs/watchdog.h"
#define SETUP_MAGIC 0x5E7A5E7Au          /* watchdog scratch 0: boot into WiFi setup */
#include "patterns.h"
#include "store.h"
#include "patterns.h"
#include "psram.h"
#include "menu.h"
#include "net.h"
#include "config.h"
#include "tesserae.h"
#include "inputs.h"
#include "local.h"
#include "net_rest.h"
#include "draw.h"
#include "painter.h"
#include "audio.h"
#include "power.h"
#include "ha.h"
#include "live.h"

#ifndef FW_VERSION
#define FW_VERSION "0.1.0"
#endif
#ifndef BTN_PIN
#define BTN_PIN 15                  /* optional external button to GND; idle high */
#endif
#ifndef BOOT_BTN_PIN
#define BOOT_BTN_PIN 45             /* Pico Plus 2 W wires its BOOT button to GP45 too */
#endif
#ifndef LONG_PRESS_MS
#define LONG_PRESS_MS 600
#endif
#ifndef MENU_MOVE_MODE
#define MENU_MOVE_MODE IT8951_MODE_A2   /* fast B/W update when the highlight moves */
#endif

#if defined(__has_include)
#  if __has_include("secrets.h")
#    include "secrets.h"
#  endif
#endif

static bool have_psram;

/* Run-time tunables (web UI / serial). */
static uint8_t  tune_move_mode     = MENU_MOVE_MODE;
static uint32_t tune_long_press_ms = LONG_PRESS_MS;
static bool     tune_banner        = true;


/* ---- content ------------------------------------------------------------------ */

static void handle_key(int ch);
static void console_input(const char *text);
static volatile const char *s_phase = "boot";   /* last main-loop phase passed (state JSON) */
static volatile uint32_t    s_loops;
typedef enum { CONTENT_DASHBOARD, CONTENT_CARD, CONTENT_WHITE, CONTENT_LIVE, CONTENT_BOOT, CONTENT_SETUP, CONTENT_FLASHING } content_t;   /* BOOT: logo + the boot log; SETUP: WiFi setup steps */   /* LIVE: white, the live page is drawn over it */
static content_t cur_content = CONTENT_CARD;

static void frame_row(int y, uint8_t out[IT8951_ROW_BYTES], void *ctx)
{
    memcpy(out, (const uint8_t *)ctx + (size_t)y * IT8951_ROW_BYTES, IT8951_ROW_BYTES);
}

/* A dashboard frame is the whole panel, or just the viewport the overlay
 * leaves (tesserae_frame_geom); white around a viewport frame. */
static void dash_row(int y, uint8_t out[IT8951_ROW_BYTES], const uint8_t *frame)
{
    int gx, gy, gw, gh;
    tesserae_frame_geom(&gx, &gy, &gw, &gh);
    if (gw >= IT8951_W && gh >= IT8951_H) { frame_row(y, out, (void *)frame); return; }
    memset(out, 0xFF, IT8951_ROW_BYTES);
    if (y < gy || y >= gy + gh) return;
    memcpy(out + gx / 2, frame + (size_t)(y - gy) * (size_t)(gw / 2), (size_t)(gw / 2));
}

/* Diagnostic patterns: solid black, and 16 grey steps left (black) to right (white). */
static void black_row(int y, uint8_t out[IT8951_ROW_BYTES], void *ctx) { (void)y; (void)ctx; memset(out, 0x00, IT8951_ROW_BYTES); }
static void ramp_row(int y, uint8_t out[IT8951_ROW_BYTES], void *ctx)
{
    (void)y; (void)ctx;
    for (int x = 0; x < IT8951_W; x += 2) {
        uint8_t a = (uint8_t)(x * 16 / IT8951_W), b = (uint8_t)((x + 1) * 16 / IT8951_W);
        out[x / 2] = (uint8_t)((a << 4) | b);
    }
}

/* ---- the boot screen: the Tesserae logo, the firmware, and the log as it
 * happens (Font24, the newest line in black), until the first dashboard. ---- */
#define BOOT_LOGO_X   104                 /* top left, the log aligned under it */
#define BOOT_LOGO_Y   84
#define BOOT_SUB_Y    (BOOT_LOGO_Y + 200 + 30)
#define BOOT_RULE_Y   (BOOT_SUB_Y + 66)
#define BOOT_LOG_X    112
#define BOOT_LOG_Y    (BOOT_RULE_Y + 34)
#define BOOT_LINE_H   30
#define BOOT_LINES    ((IT8951_H - 50 - BOOT_LOG_Y) / BOOT_LINE_H)
#define BOOT_COLS     ((IT8951_W - 2 * BOOT_LOG_X) / GLYPH_W)
typedef struct { char line[BOOT_LINES][BOOT_COLS + 1]; char sub[96]; } boot_text_t;
static boot_text_t s_boot;                 /* built on core 0 */
static uint32_t    s_boot_seen, s_boot_last_ms, s_boot_hold_until;
static uint8_t     s_boot_line[2][IT8951_W];

static size_t app_log_tail(char *dst, size_t cap);
static volatile uint32_t s_log_w;          /* the console log's write position (below) */
static void local_flush(void);
/* Lines left off the screen: the panel's own paint timings (each repaint of
 * the log would otherwise add a line, and repaint it again). */
static bool boot_skip(const char *l, size_t n)
{
    static const char *const SKIP[] = { "streamed in", "area(s) in parallel", "second pass over" };
    for (size_t k = 0; k < sizeof SKIP / sizeof SKIP[0]; k++) {
        size_t m = strlen(SKIP[k]);
        for (size_t i = 0; i + m <= n; i++) if (!memcmp(l + i, SKIP[k], m)) return true;
    }
    return false;
}

/* The last BOOT_LINES non-empty lines of the log. True when they changed. */
static bool boot_collect(void)
{
    static char tail[3072];
    static boot_text_t prev;
    prev = s_boot;
    size_t n = app_log_tail(tail, sizeof tail);
    int k = BOOT_LINES;
    memset(s_boot.line, 0, sizeof s_boot.line);
    size_t end = n;
    while (k > 0 && end > 0) {
        size_t e = end; while (e > 0 && (tail[e - 1] == '\n' || tail[e - 1] == '\r')) e--;
        if (e == 0) break;
        size_t b = e; while (b > 0 && tail[b - 1] != '\n') b--;
        if (boot_skip(tail + b, e - b)) { end = b; continue; }
        size_t len = e - b; if (len > BOOT_COLS) len = BOOT_COLS;
        k--;
        for (size_t i = 0; i < len; i++) { char c = tail[b + i]; s_boot.line[k][i] = (c >= 32 && c < 127) ? c : ' '; }
        end = b;
    }
    if (k > 0) {                            /* fewer lines than rows: top-align */
        memmove(s_boot.line[0], s_boot.line[k], sizeof s_boot.line[0] * (size_t)(BOOT_LINES - k));
        memset(s_boot.line[BOOT_LINES - k], 0, sizeof s_boot.line[0] * (size_t)k);
    }
    int slot = ota_boot_partition();
    snprintf(s_boot.sub, sizeof s_boot.sub, "Bedside  \x80  firmware %s%s%c", FW_VERSION, slot >= 0 ? "  \x80  slot " : "", slot >= 0 ? 'A' + slot : ' ');
    return memcmp(&prev, &s_boot, sizeof prev) != 0;
}

static void boot_row(int y, uint8_t out[IT8951_ROW_BYTES], const boot_text_t *t)
{
    memset(out, 0xFF, IT8951_ROW_BYTES);
    if (y >= BOOT_LOGO_Y && y < BOOT_LOGO_Y + boot_logo_h) {
        int x0 = BOOT_LOGO_X & ~1;
        memcpy(out + x0 / 2, boot_logo + (size_t)(y - BOOT_LOGO_Y) * (boot_logo_w / 2), boot_logo_w / 2);
        return;
    }
    uint8_t *line = s_boot_line[get_core_num()];
    if (y >= BOOT_SUB_Y && y < BOOT_SUB_Y + ui34.line_h) {
        memset(line, 0xF, IT8951_W);
        draw_font_row(&ui34, line, IT8951_W, BOOT_LOG_X, t->sub, y - BOOT_SUB_Y, G_DARK);
        draw_pack(out, 0, line, IT8951_W);
        return;
    }
    if (y >= BOOT_RULE_Y && y < BOOT_RULE_Y + 2) {
        memset(line, 0xF, IT8951_W); memset(line + BOOT_LOG_X, G_LIGHT, IT8951_W - 2 * BOOT_LOG_X);
        draw_pack(out, 0, line, IT8951_W);
        return;
    }
    if (y >= BOOT_LOG_Y && y < BOOT_LOG_Y + BOOT_LINES * BOOT_LINE_H) {
        int i = (y - BOOT_LOG_Y) / BOOT_LINE_H, g = (y - BOOT_LOG_Y) % BOOT_LINE_H;
        if (g >= GLYPH_H || !t->line[i][0]) return;
        bool newest = (i == BOOT_LINES - 1 || !t->line[i + 1][0]);
        memset(line, 0xF, IT8951_W);
        draw_text_row(line, IT8951_W, BOOT_LOG_X, t->line[i], g, 1, newest ? G_BLACK : G_DARK);
        draw_pack(out, 0, line, IT8951_W);
    }
}

/* ---- setup mode screen: the logo and how to connect ------------------------- */
static char     s_setup_why[96];
static uint32_t s_setup_until;          /* restart to try the saved WiFi again (0: stay) */
typedef struct { int y; const bfont_t *f; uint8_t g; const char *t; } setup_line_t;
static char s_fw_version_line[48];
static void setup_row(int y, uint8_t out[IT8951_ROW_BYTES])
{
    memset(out, 0xFF, IT8951_ROW_BYTES);
    if (y >= BOOT_LOGO_Y && y < BOOT_LOGO_Y + boot_logo_h) {
        memcpy(out + (BOOT_LOGO_X & ~1) / 2, boot_logo + (size_t)(y - BOOT_LOGO_Y) * (boot_logo_w / 2), boot_logo_w / 2);
        return;
    }
    static char url[40];
    snprintf(url, sizeof url, "or go to http://192.168.4.1");
    const setup_line_t F[] = {          /* CONTENT_FLASHING: the panel keeps this up through a USB flash */
        { 380,  &ui72, G_BLACK, "Updating firmware" },
        { 520,  &ui46, G_DARK,  "New firmware is loading over USB." },
        { 590,  &ui46, G_DARK,  "Keep the panel plugged in. It restarts by itself." },
        { 0,    &ui34, G_DARK,  "" }, { 0, &ui34, G_DARK, "" }, { 0, &ui34, G_DARK, "" },
        { 1240, &ui34, G_DARK,  s_fw_version_line },
    };
    const setup_line_t S[] = {
        { 380,  &ui72, G_BLACK, "Set up this panel" },
        { 520,  &ui46, G_DARK,  "1   On your phone, join the WiFi network" },
        { 590,  &ui96, G_BLACK, net_setup_ssid() },
        { 760,  &ui46, G_DARK,  "2   The setup page opens by itself" },
        { 822,  &ui34, G_DARK,  url },
        { 930,  &ui46, G_DARK,  "3   Choose your WiFi and your Tesserae server" },
        { 1240, &ui34, G_DARK,  s_setup_why },
    };
    const setup_line_t *L = cur_content == CONTENT_FLASHING ? F : S;
    uint8_t *line = s_boot_line[get_core_num()];
    for (size_t i = 0; i < sizeof S / sizeof S[0]; i++) {
        if (y < L[i].y || y >= L[i].y + L[i].f->line_h || !L[i].t[0]) continue;
        memset(line, 0xF, IT8951_W);
        int x = BOOT_LOG_X + (L[i].f == &ui34 && i == 4 ? 62 : 0);
        draw_font_row(L[i].f, line, IT8951_W, x, L[i].t, y - L[i].y, L[i].g);
        draw_pack(out, 0, line, IT8951_W);
        return;
    }
    if (y >= 1200 && y < 1202) { memset(line, 0xF, IT8951_W); memset(line + BOOT_LOG_X, G_LIGHT, IT8951_W - 2 * BOOT_LOG_X); draw_pack(out, 0, line, IT8951_W); }
}

const char *app_setup_ssid(void)   { return config_get()->wifi_ssid; }
const char *app_setup_server(void) { return config_get()->server_url; }
const char *app_setup_name(void)   { return config_get()->device_name; }

/* Repaint the log when new lines have arrived (fast 2-level update). */
static void boot_service(void)
{
    if (cur_content != CONTENT_BOOT || s_log_w == s_boot_seen) return;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - s_boot_last_ms < 400 || painter_busy()) return;
    s_boot_seen = s_log_w; s_boot_last_ms = now;
    if (!boot_collect()) return;              /* only paint timings arrived */
    local_mark_area(0, BOOT_LOG_Y, IT8951_W, BOOT_LINES * BOOT_LINE_H, IT8951_MODE_DU);
    local_flush();
}

static void content_row(int y, uint8_t out[IT8951_ROW_BYTES])
{
    if (cur_content == CONTENT_BOOT) { boot_row(y, out, &s_boot); return; }
    if (cur_content == CONTENT_SETUP || cur_content == CONTENT_FLASHING) { setup_row(y, out); return; }
    const uint8_t *frame = tesserae_frame();
    if (cur_content == CONTENT_DASHBOARD && frame) dash_row(y, out, frame);
    else if (cur_content == CONTENT_WHITE || cur_content == CONTENT_LIVE) memset(out, 0xFF, IT8951_ROW_BYTES);
    else                                           IT8951_PATTERNS[3].fn(y, out, NULL);   /* the test card: grey blocks */
}

static const char *content_name(void)
{
    if (cur_content == CONTENT_BOOT) return "boot screen";
    if (cur_content == CONTENT_SETUP) return "WiFi setup";
    if (cur_content == CONTENT_FLASHING) return "updating firmware";
    if (cur_content == CONTENT_LIVE) return "live";
    if (cur_content == CONTENT_DASHBOARD) return tesserae_frame() ? "dashboard" : "dashboard (none yet)";
    return cur_content == CONTENT_CARD ? "test card" : "white";
}

/* ---- overlays: local UI + banner ------------------------------------------------ */

static char banner_text[96];

static void composite_row(int y, uint8_t out[IT8951_ROW_BYTES], void *ctx)
{
    (void)ctx;
    content_row(y, out);
    if (cur_content == CONTENT_BOOT || cur_content == CONTENT_SETUP || cur_content == CONTENT_FLASHING) return;   /* these screens stand alone */
    local_overlay_row(y, out);
    if (tune_banner && banner_text[0]) banner_overlay_row(banner_text, y, out);
}

/* What core 1 paints from: everything composite_row() reads, frozen at submit
 * (painter.h). Core 0 is free to change the live state meanwhile. */
static struct {
    content_t      content;
    const uint8_t *frame;
    bool           banner;
    char           text[sizeof banner_text];
    boot_text_t    boot;
} P;

static void snapshot(void)
{
    P.content = cur_content;
    P.frame = tesserae_frame();
    P.banner = tune_banner;
    memcpy(P.text, banner_text, sizeof P.text);
    if (P.content == CONTENT_BOOT) P.boot = s_boot;
    local_snapshot();
}

static void paint_row(int y, uint8_t out[IT8951_ROW_BYTES], void *ctx)
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
static bool     s_flush_pending;
static uint8_t  s_clear_mode = 0xFF;    /* a clear waiting for the painter */
static uint32_t s_painted_epoch;
static uint32_t s_partials;
static uint32_t s_last_input_ms;
/* Partial updates leave the glass a little lighter each time. After the user
 * has been doing things (knob, keys, web app, HA), one full GC16 refresh once
 * input has settled puts the blacks back. Background updates (the live page's
 * clock, HA values) don't count: they get the slower clean-up below, so the
 * panel doesn't flash every minute at night. */
#define SETTLE_MS 4000u
static bool     s_touched;
static bool     s_job_screen;           /* the job in flight changes page */
static bool     s_job_counts, s_job_resets;
/* A screen change or full refresh comes out faded on this glass until a
 * second pass runs the waveform over it again; the image is still in the
 * controller, so the pass costs only the waveform. */
static painter_job_t s_echo;
static bool          s_job_echo, s_echo_due;
static uint8_t       s_echo_mode = 0xFF;   /* off: a GL16 pass streaked dithered greys (/api/cmd?echo=gl16|gc16 to try) */
static void local_flush(void) { s_flush_pending = true; }

static void paint_pump(void)
{
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
    if (!full && all_du) j.mode = IT8951_MODE_DU;       /* menu steps: black and white, ~0.25 s; the settle pass restores the greys */
    /* Screen updates: Clean draws every change with GC16 (just its area), Full
     * repaints the whole screen with GC16 every time. Both keep blacks black. */
    if (cur_content == CONTENT_BOOT) { if (!full) j.mode = IT8951_MODE_DU; }   /* the boot log: fast, black and white */
    else if (local_refresh_mode() == 2) { full = true; j.mode = IT8951_MODE_GC16; }
    else if (j.mode == IT8951_MODE_GL16) j.mode = IT8951_MODE_GC16;   /* every mode: GL16 lightens greys pass by pass and is no faster here (723 vs 741 ms) */
    j.kind = full ? PJ_FULL : PJ_AREAS;
    s_job_resets = full && j.mode == IT8951_MODE_GC16;
    {
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (s_job_resets) s_touched = false;
        else if (local_refresh_mode() == 0 && now - s_last_input_ms < 3000u && j.mode != IT8951_MODE_GC16) s_touched = true;   /* Fast mode: a partial update the user caused */
    }
    s_job_counts = j.mode != IT8951_MODE_GC16;          /* GC16 areas leave nothing to clean up */
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

static void cleanup_if_idle(void)
{
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (s_touched && !painter_busy() && !s_flush_pending && now - s_last_input_ms >= SETTLE_MS) {
        printf("paint: input settled, full refresh\n");
        s_touched = false;
        s_partials = 0;
        local_set_dirty_full();
        local_flush_now();
        return;
    }
    if (s_partials >= 40 && !painter_busy() && now - s_last_input_ms > 20000 && local_screen() == SCR_HOME) {
        printf("paint: GC16 clean-up after %lu partial updates\n", (unsigned long)s_partials);
        s_partials = 0;
        local_set_dirty_full();
        local_flush_now();
    }
}

/* The banner says where the web UI is and, until the device is paired and has
 * a frame, what it is waiting for. Once the dashboard is up it goes quiet. */
static void banner_compose(void)
{
    const char *ip = net_connected() ? net_ip() : NULL;
    tess_state_t st = tesserae_state();
    if (!ip)                              snprintf(banner_text, sizeof banner_text, "no wifi: see include/secrets.h");
    else if (st == TESS_OFF)              snprintf(banner_text, sizeof banner_text, "http://%s/  set server URL", ip);
    else if (st == TESS_DISCOVERING)      snprintf(banner_text, sizeof banner_text, "http://%s/  register %s in Tesserae", ip, config_device_id());
    else if (st == TESS_ERROR)            snprintf(banner_text, sizeof banner_text, "http://%s/  server unreachable", ip);
    else if (st == TESS_READY && !tesserae_frame() && tesserae_last_error()[0])
                                          snprintf(banner_text, sizeof banner_text, "http://%s/  bad frame from server: check kind/panel", ip);
    else if (st == TESS_READY && !tesserae_frame())
                                          snprintf(banner_text, sizeof banner_text, "http://%s/  bind a dashboard to %s", ip, config_device_id());
    else if (cur_content == CONTENT_LIVE || (tesserae_frame() && cur_content == CONTENT_DASHBOARD))
                                          banner_text[0] = '\0';
    else                                  snprintf(banner_text, sizeof banner_text, "http://%s/", ip);
}

static void banner_update(void)
{
    char before[sizeof banner_text];
    memcpy(before, banner_text, sizeof before);
    banner_compose();
    if (!strcmp(before, banner_text)) return;
    int x, y, w, h, ow = 0;
    if (before[0]) banner_rect(before, &x, &y, &ow, &h);
    if (banner_text[0]) banner_rect(banner_text, &x, &y, &w, &h); else { banner_rect("x", &x, &y, &w, &h); w = 0; }
    if (ow > w) w = ow;
    if (w == 0) return;
    local_mark_area(x, y, w, h, IT8951_MODE_GC16);
    local_flush();
}

/* ---- painting --------------------------------------------------------------- */

static void repaint(void)
{
    banner_compose();
    local_set_dirty_full();
    local_flush_now();
}

static void show_content(content_t c)
{
    cur_content = c;
    printf("paint: %s\n", content_name());
    repaint();
}

/* USB flashing. The host's 1200-baud reset is ours (platformio.ini turns the
 * SDK's off): say so on the panel, which keeps the picture through the flash,
 * then hand over to the USB bootloader. An alarm makes sure the reset happens
 * even if the main loop is stuck. */
static volatile bool s_usb_flash;
static int64_t usb_boot_now(alarm_id_t id, void *u) { (void)id; (void)u; reset_usb_boot(0, 0); return 0; }
void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const *lc)
{
    (void)itf;
    if (lc->bit_rate == 1200 && !s_usb_flash) { s_usb_flash = true; add_alarm_in_ms(6000, usb_boot_now, NULL, true); }
}
static void enter_usb_boot(void)
{
    printf("rebooting into the USB bootloader\n");
    snprintf(s_fw_version_line, sizeof s_fw_version_line, "Now running %s", FW_VERSION);
    show_content(CONTENT_FLASHING);
    uint32_t t0 = to_ms_since_boot(get_absolute_time());
    do { paint_pump(); sleep_ms(20); }
    while ((s_flush_pending || painter_busy()) && to_ms_since_boot(get_absolute_time()) - t0 < 5000);
    sleep_ms(100);
    reset_usb_boot(0, 0);
}

static void clear_screen(uint8_t mode)
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
static void on_frame(void)
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

/* ---- pin discovery (field debugging; see README) ------------------------------- */

#define DIAG_PINS 30

static void diag_sample(uint32_t *out)
{
    *out = 0;
    for (uint p = 0; p < DIAG_PINS; p++) if (gpio_get(p)) *out |= 1u << p;
}

static void diag_pins(void)
{
    uint32_t dn = 0, up = 0;
    printf("diag: scanning GP0..GP%d (pull-down, then pull-up)\n", DIAG_PINS - 1);
    epd_io_deinit();
    for (uint p = 0; p < DIAG_PINS; p++) {
        if (p == EPD_PIN_RST) continue;
        gpio_init(p);
        gpio_pull_down(p);
    }
    sleep_ms(5);
    diag_sample(&dn);
    for (uint p = 0; p < DIAG_PINS; p++) if (p != EPD_PIN_RST) gpio_pull_up(p);
    sleep_ms(5);
    diag_sample(&up);

    printf("diag: driven HIGH:");
    for (uint p = 0; p < DIAG_PINS; p++) if (p != EPD_PIN_RST && (dn & up & (1u << p))) printf(" GP%u", p);
    printf("\ndiag: driven LOW :");
    for (uint p = 0; p < DIAG_PINS; p++) if (p != EPD_PIN_RST && !((dn | up) & (1u << p))) printf(" GP%u", p);
    printf("\ndiag: floating   :");
    for (uint p = 0; p < DIAG_PINS; p++) if (p != EPD_PIN_RST && !(dn & (1u << p)) && (up & (1u << p))) printf(" GP%u", p);
    printf("\n");

    for (uint p = 0; p < DIAG_PINS; p++) if (p != EPD_PIN_RST) gpio_pull_down(p);
    gpio_init(EPD_PIN_RST); gpio_put(EPD_PIN_RST, 1); gpio_set_dir(EPD_PIN_RST, GPIO_OUT);
    sleep_ms(50);
    uint32_t before, during, a5, a50, a500, a2000;
    diag_sample(&before);
    gpio_put(EPD_PIN_RST, 0); sleep_ms(10); diag_sample(&during);
    gpio_put(EPD_PIN_RST, 1);
    sleep_ms(5);    diag_sample(&a5);
    sleep_ms(45);   diag_sample(&a50);
    sleep_ms(450);  diag_sample(&a500);
    sleep_ms(1500); diag_sample(&a2000);
    uint32_t changed = (before ^ during) | (before ^ a5) | (before ^ a50) | (before ^ a500) | (before ^ a2000);
    changed &= ~(1u << EPD_PIN_RST);
    printf("diag: RST pulse on GP%d -> pins that moved (before/during/+5ms/+50ms/+500ms/+2s):", EPD_PIN_RST);
    if (!changed) printf(" none");
    for (uint p = 0; p < DIAG_PINS; p++) {
        if (!(changed & (1u << p))) continue;
        printf(" GP%u=%u%u%u%u%u%u", p, (unsigned)((before >> p) & 1), (unsigned)((during >> p) & 1), (unsigned)((a5 >> p) & 1),
               (unsigned)((a50 >> p) & 1), (unsigned)((a500 >> p) & 1), (unsigned)((a2000 >> p) & 1));
    }
    printf("\ndiag: a pin that is low during reset and rises within ~2 s afterwards is HRDY\n");
    for (uint p = 0; p < DIAG_PINS; p++) if (p != EPD_PIN_RST) gpio_disable_pulls(p);
    gpio_pull_up(BTN_PIN);
}

/* ---- inputs ----------------------------------------------------------------- */

/* Read the BOOTSEL button (pico-examples picoboard/button, BSD-3-Clause). */
__attribute__((unused)) static bool __no_inline_not_in_flash_func(bootsel_down)(void)
{
    const uint CS_PIN_INDEX = 1;
    uint32_t flags = save_and_disable_interrupts();
    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
    for (volatile int i = 0; i < 1000; ++i) {}
#if PICO_RP2040
    bool pressed = !(sio_hw->gpio_hi_in & (1u << 1));
#else
    bool pressed = !(sio_hw->gpio_hi_in & SIO_GPIO_HI_IN_QSPI_CSN_BITS);
#endif
    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
    restore_interrupts(flags);
    return pressed;
}

static bool button_down(void)
{
    return !gpio_get(BTN_PIN) || !gpio_get(BOOT_BTN_PIN);   /* both active low */
}

typedef enum { PRESS_NONE, PRESS_SHORT, PRESS_LONG } press_t;

static press_t poll_press(bool (*down)(void))
{
    if (!down()) return PRESS_NONE;
    sleep_ms(20);
    if (!down()) return PRESS_NONE;
    uint32_t t0 = to_ms_since_boot(get_absolute_time());
    while (down()) sleep_ms(10);
    uint32_t held = to_ms_since_boot(get_absolute_time()) - t0;
    return held >= tune_long_press_ms ? PRESS_LONG : PRESS_SHORT;
}

/* ---- actions and bindings -------------------------------------------------------- */

static void backlight_step(int delta)
{
    int v = (int)inputs_backlight() + delta;
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    inputs_set_backlight((uint8_t)v);
    config_set_lights((uint8_t)v, inputs_night());
    config_save();
}

static void run_action(const char *spec)
{
    if (!spec || !spec[0] || !strcmp(spec, "none")) return;
    printf("action: %s\n", spec);
    if (local_action(spec))                { local_flush(); return; }
    if (!strncmp(spec, "button:", 7))      { if (spec[7]) tesserae_button(spec + 7); }
    else if (!strcmp(spec, "refresh"))     tesserae_button("refresh");
    else if (!strcmp(spec, "fetch"))       tesserae_kick(true);
    else if (!strcmp(spec, "dashboard"))   show_content(CONTENT_DASHBOARD);
    else if (!strcmp(spec, "card"))        show_content(CONTENT_CARD);
    else if (!strcmp(spec, "clear"))       clear_screen(IT8951_MODE_INIT);
    else if (!strcmp(spec, "sleep"))       { painter_lock(); it8951_sleep(); painter_unlock(); }
    else if (!strcmp(spec, "backlight_up"))   backlight_step(+40);
    else if (!strcmp(spec, "backlight_down")) backlight_step(-40);
    else if (!strcmp(spec, "repair"))      { tesserae_forget(); banner_update(); }
    else printf("action: unknown \"%s\"\n", spec);
}

static bool action_valid(const char *spec)
{
    if (!strncmp(spec, "button:", 7)) return spec[7] != '\0';
    for (int i = 0; i < local_action_count(); i++) if (!strcmp(spec, local_action_id(i))) return true;
    return false;
}

static void run_binding(int input) { run_action(config_get()->bind[input]); }

__attribute__((unused)) static bool binding_set(int input) { const char *b = config_get()->bind[input]; return b[0] && strcmp(b, "none") != 0; }

/* BOOT / GP15 button. */
static void on_press(press_t p)
{
    if (p == PRESS_SHORT)     run_binding(CONFIG_BIND_BOOT_SHORT);
    else if (p == PRESS_LONG) run_binding(CONFIG_BIND_BOOT_LONG);
}

/* Keys: a key with no hold binding acts on the way down (snappy); one with a
 * hold binding waits to see which it was: hold fires at the threshold, press
 * fires on release if the hold did not. */
static bool s_key_deferred[4];

static void key_run(int i, bool hold)
{
    if (local_key(i, hold)) { local_flush(); return; }          /* MENU, or the alarm ringing */
    run_binding((hold ? CONFIG_BIND_KEY1_HOLD : CONFIG_BIND_KEY1) + i);
    local_flush();
}

static void on_key_down(int i)
{
    if (i < 0 || i > 3) return;
    if (local_key_has_hold(i)) { s_key_deferred[i] = true; return; }   /* tap or hold? wait and see */
    key_run(i, false);
}

static void on_key_hold(int i)
{
    if (i < 0 || i > 3 || !s_key_deferred[i]) return;
    s_key_deferred[i] = false;
    key_run(i, true);
}

static void on_key_up(int i)
{
    if (i < 0 || i > 3 || !s_key_deferred[i]) return;
    s_key_deferred[i] = false;
    key_run(i, false);
}

static void on_input(input_event_t e)
{
    s_last_input_ms = to_ms_since_boot(get_absolute_time());
    switch (e.kind) {
    case IN_TURN:
        /* the whole delta at once: the UI scales it by how fast it came */
        if (!local_turn(e.value)) run_binding(e.value > 0 ? CONFIG_BIND_KNOB_CW : CONFIG_BIND_KNOB_CCW);
        local_flush();
        break;
    case IN_PRESS_SHORT: if (local_press()) local_flush(); else run_binding(CONFIG_BIND_KNOB_PRESS); break;
    case IN_PRESS_LONG: {                     /* Back by default; any other binding replaces it */
        const char *b = config_get()->bind[CONFIG_BIND_KNOB_HOLD];
        if (!b[0] || !strcmp(b, "back")) { if (local_back_up()) local_flush(); }
        else { run_binding(CONFIG_BIND_KNOB_HOLD); local_flush(); }
        break; }
    case IN_KEY:         on_key_down(e.value); break;
    case IN_KEY_HOLD:    on_key_hold(e.value); break;
    case IN_KEY_RELEASE: on_key_up(e.value); break;
    default: break;
    }
}

static int binding_index(const char *name)
{
    for (int i = 0; i < CONFIG_BINDINGS; i++) if (!strcmp(name, config_bind_name(i))) return i;
    return -1;
}

/* ---- commands (serial + web share this) --------------------------------------- */

static void exec_command(const char *name, const char *value)
{
    int v = atoi(value);
    bool save = false;
    if      (!strcmp(name, "fetch"))     tesserae_kick(true);
    else if (!strcmp(name, "ha")) {          /* a command from Home Assistant (ha.c) */
        char n2[24], v2[200];
        if (ha_translate(value, n2, sizeof n2, v2, sizeof v2)) exec_command(n2, v2);
        return;
    }
    else if (!strcmp(name, "mqtt"))      { ha_configure(value); return; }
    else if (!strcmp(name, "live_src")) {    /* "<slot key>|<entity id or topic>" */
        const char *bar = strchr(value, '|');
        if (bar) for (int i = 0; i < LV_COUNT; i++) if (!strncmp(value, live_key(i), (size_t)(bar - value)) && live_key(i)[bar - value] == 0) live_set_source(i, bar + 1);
        return;
    }
    else if (!strcmp(name, "live_base")) { live_set_ss_base(value); return; }
    else if (!strcmp(name, "live_loc"))  {  /* "lat,lon" */
        char *e; double la = strtod(value, &e);
        if (*e == ',') { double lo = strtod(e + 1, NULL); live_set_location((float)la, (float)lo); }
        return;
    }
    else if (!strcmp(name, "button"))    tesserae_button(value);
    /* Virtual controller (web UI): the same bindings as the hardware. */
    else if (!strcmp(name, "knob")) {
        if      (!strcmp(value, "cw"))    on_input((input_event_t){ IN_TURN, +1 });
        else if (!strcmp(value, "ccw"))   on_input((input_event_t){ IN_TURN, -1 });
        else if (!strcmp(value, "press")) on_input((input_event_t){ IN_PRESS_SHORT, 0 });
        else if (!strcmp(value, "hold"))  on_input((input_event_t){ IN_PRESS_LONG, 0 });
    }
    else if (!strcmp(name, "key"))       { if (v >= 1 && v <= 4) key_run(v - 1, false); }   /* the same as the hardware */
    else if (!strcmp(name, "keyhold"))   { if (v >= 1 && v <= 4) key_run(v - 1, true); }
    else if (!strcmp(name, "page")) {
        screen_t sc = SCR_COUNT;
        for (int i = 0; i < SCR_COUNT; i++) if (!strcmp(value, local_screen_name((screen_t)i))) sc = (screen_t)i;
        if (sc != SCR_COUNT) { local_open(sc); local_flush(); }
    }
    else if (!strcmp(name, "scenario"))  { if (local_scenario(value)) local_flush(); }
    else if (!strcmp(name, "set")) {
        char key[24]; const char *bar = strchr(value, '|');
        if (bar && (size_t)(bar - value) < sizeof key) {
            snprintf(key, sizeof key, "%.*s", (int)(bar - value), value);
            if (local_set(key, bar + 1)) local_flush(); else printf("set: rejected %s\n", value);
        }
    }
    else if (!strcmp(name, "boot"))      run_binding(!strcmp(value, "long") ? CONFIG_BIND_BOOT_LONG : CONFIG_BIND_BOOT_SHORT);
    else if (!strcmp(name, "action"))    run_action(value);
    /* Remapping: bind=<input>|<action spec>, label=<1-4>|<text>. */
    else if (!strcmp(name, "bind")) {
        const char *bar = strchr(value, '|');
        char in[16];
        if (bar) {
            snprintf(in, sizeof in, "%.*s", (int)(bar - value) < 15 ? (int)(bar - value) : 15, value);
            int idx = binding_index(in);
            if (idx < 0) printf("bind: unknown input \"%s\"\n", in);
            else if (!action_valid(bar + 1)) printf("bind: unknown action \"%s\"\n", bar + 1);
            else { config_set_bind(idx, bar + 1); save = true; printf("bind: %s -> %s\n", in, bar + 1); }
        }
    }
    else if (!strcmp(name, "label")) {
        const char *bar = strchr(value, '|');
        int i = atoi(value) - 1;
        if (bar && i >= 0 && i < 4) { char lb[16]; snprintf(lb, sizeof lb, "%.13s", bar + 1); config_set_key_label(i, lb); save = true; local_set_dirty_full(); local_flush(); }
    }
    else if (!strcmp(name, "backlight")) { if (v < 0) v = 0; if (v > 255) v = 255; inputs_set_backlight((uint8_t)v); config_set_lights((uint8_t)v, inputs_night()); save = true; }
    else if (!strcmp(name, "night"))     { inputs_set_night(v != 0); config_set_lights(inputs_backlight(), v != 0); save = true; }
    else if (!strcmp(name, "dashboard")) show_content(CONTENT_DASHBOARD);
    else if (!strcmp(name, "card"))      show_content(CONTENT_CARD);
    else if (!strcmp(name, "show"))      show_content(v == 1 ? CONTENT_CARD : CONTENT_DASHBOARD);
    else if (!strcmp(name, "menu")) {
        if      (!strcmp(value, "open"))   { local_open(SCR_MENU); }
        else if (!strcmp(value, "close"))  { local_action("back"); }
        else if (!strcmp(value, "up"))     { local_turn(-1); }
        else if (!strcmp(value, "down"))   { local_turn(+1); }
        else if (!strcmp(value, "select")) { local_press(); }
        local_flush();
    }
    else if (!strcmp(name, "clear"))     clear_screen(IT8951_MODE_INIT);
    else if (!strcmp(name, "white"))     clear_screen(IT8951_MODE_GC16);
    else if (!strcmp(name, "sleep"))     { painter_lock(); it8951_sleep(); painter_unlock(); }
    else if (!strcmp(name, "spitest"))   { painter_lock(); it8951_spi_test(v > 0 ? v : 8); painter_unlock(); local_set_dirty_full(); local_flush(); }
    else if (!strcmp(name, "init"))      { painter_lock(); it8951_init(); painter_unlock(); local_set_dirty_full(); local_flush(); }
    else if (!strcmp(name, "enhance"))   { painter_lock(); it8951_enhance_driving(); painter_unlock(); }
    else if (!strcmp(name, "pattern"))   {     /* diagnostics: INIT, then a full GC16 of solid black or a 16-step ramp */
        bool ramp = !strcmp(value, "ramp");
        painter_lock();
        it8951_clear(IT8951_MODE_INIT);
        it8951_show_pattern(ramp ? ramp_row : black_row, NULL, IT8951_MODE_GC16);
        painter_unlock();
        printf("pattern: %s shown (GC16); 'dashboard' to go back\n", ramp ? "16-step ramp" : "solid black");
    }
    else if (!strcmp(name, "temp"))      { painter_lock(); it8951_set_temp(v); painter_unlock(); }     /* 0 = sensed */
    else if (!strcmp(name, "epdpower"))  { painter_lock(); it8951_set_rails_off(v != 0); painter_unlock(); }   /* epdpower=1: rails off between refreshes (default); 0: leave them on */
    else if (!strcmp(name, "standby"))   { painter_lock(); it8951_set_standby(v != 0); painter_unlock(); }
    else if (!strcmp(name, "cleanfull")) { it8951_set_clean_full(v != 0); printf("it8951: INIT before full GC16 %s\n", v ? "on" : "off"); }
    else if (!strcmp(name, "vcom"))      { if (v >= 500 && v <= 5000) { painter_lock(); it8951_set_vcom((uint16_t)v); painter_unlock(); } else printf("vcom: expected 500..5000 mV\n"); }
    else if (!strcmp(name, "mirror"))    { { painter_lock(); it8951_set_mirror(v != 0); painter_unlock(); } config_set_mirror(v != 0); save = true; printf("tune: mirror %d\n", v != 0); repaint(); }
    else if (!strcmp(name, "movemode"))  { tune_move_mode = (v == IT8951_MODE_GC16) ? IT8951_MODE_GC16 : IT8951_MODE_A2; }
    else if (!strcmp(name, "echo"))      { s_echo_mode = !strcmp(value, "off") ? 0xFF : !strcmp(value, "gc16") ? IT8951_MODE_GC16 : IT8951_MODE_GL16;
                                           printf("paint: second pass %s\n", value); }
    else if (!strcmp(name, "ota"))       {     /* <url>|<size>|<sha256>[|<version>]: install that image */
        char u[1024]; snprintf(u, sizeof u, "%s", value);
        char *f[4] = { u, NULL, NULL, NULL };
        for (int i = 1; i < 4 && f[i - 1]; i++) { char *bar = strchr(f[i - 1], '|'); if (bar) { *bar = 0; f[i] = bar + 1; } }
        if (f[1] && f[2]) ota_start(f[0], (uint32_t)strtoul(f[1], NULL, 10), f[2], f[3]);
        else printf("ota: expected <url>|<size>|<sha256>[|<version>]\n");
    }
    else if (!strcmp(name, "picks"))     tesserae_picks_fetch();          /* refresh the dashboard list */
    else if (!strcmp(name, "otacheck"))  ota_releases_fetch();
    else if (!strcmp(name, "otainstall")) { if (!ota_install(v)) printf("ota: %s\n", ota_error()); }
    else if (!strcmp(name, "https"))     {     /* diagnostics: GET a URL (http or https), print the status and the start */
        static char u[1024]; snprintf(u, sizeof u, "%s", value);
        uint8_t *buf = PSRAM_XIP_BASE + 6u * 1024 * 1024 + 512u * 1024; size_t got = 0; uint32_t total = 0;
        uint32_t t0 = to_ms_since_boot(get_absolute_time());
        bool ok = rest_download_range(u, sizeof u, 0, 65536, buf, &got, &total, 30000);
        printf("https: %s, %u bytes (of %lu) in %lu ms, heap free %u\n", ok ? "ok" : "FAILED", (unsigned)got, (unsigned long)total,
               (unsigned long)(to_ms_since_boot(get_absolute_time()) - t0), (unsigned)psram_heap_free_bytes());
        if (got) { char head[121]; size_t n = got < 120 ? got : 120; memcpy(head, buf, n); head[n] = 0; for (size_t i = 0; i < n; i++) if (head[i] < 32 || head[i] > 126) head[i] = '.'; printf("https: %s\n", head); }
    }
    else if (!strcmp(name, "wifisetup")) { printf("rebooting into WiFi setup\n"); watchdog_hw->scratch[0] = SETUP_MAGIC; sleep_ms(200); watchdog_reboot(0, 0, 0); }
    else if (!strcmp(name, "showsetup")) { snprintf(s_setup_why, sizeof s_setup_why, "Preview of the setup screen."); show_content(CONTENT_SETUP); }   /* diagnostics */
    else if (!strcmp(name, "serial"))    console_input(value);
    else if (!strcmp(name, "enter"))     handle_key('\r');   /* the console's Enter: a knob press */
    else if (!strcmp(name, "spidrive"))  {     /* <mA>[f]: 2/4/8/12, 'f' = fast slew; re-inits the panel link */
        epd_io_set_drive(v, strchr(value, 'f') != NULL);
        painter_lock(); it8951_init(); painter_unlock(); local_set_dirty_full(); local_flush();
    }
    else if (!strcmp(name, "spihz"))     { if (v >= 1000000 && v <= 24000000) { painter_lock(); it8951_set_spi_hz((uint32_t)v); it8951_init(); painter_unlock(); local_set_dirty_full(); local_flush(); } }
    else if (!strcmp(name, "longpress")) { if (v >= 150 && v <= 5000) tune_long_press_ms = (uint32_t)v; }
    else if (!strcmp(name, "banner"))    { tune_banner = (v != 0); banner_update(); }
    else if (!strcmp(name, "diag"))      { painter_lock(); diag_pins(); it8951_probe_matrix(); it8951_init(); painter_unlock(); }
    /* Tesserae provisioning. Saved immediately; take effect on the next poll. */
    else if (!strcmp(name, "server"))    { config_set_server(value); save = true; tesserae_forget(); }
    else if (!strcmp(name, "device"))    { config_set_device_id(value); save = true; tesserae_forget(); }
    else if (!strcmp(name, "name"))      { config_set_device_name(value); save = true; }
    else if (!strcmp(name, "kind"))      { config_set_kind(value); save = true; tesserae_forget(); }
    else if (!strcmp(name, "pair"))      { config_set_pairing_code(value); config_set_device_token(""); save = true; tesserae_kick(true); }
    else if (!strcmp(name, "forget"))    tesserae_forget();
    else if (!strcmp(name, "wifi")) {
        /* "ssid|password": applied at the next boot (the radio is not re-joined live). */
        char ssid[33] = "", pass[65] = "";
        const char *bar = strchr(value, '|');
        if (bar) { int n = (int)(bar - value); if (n > 32) n = 32; snprintf(ssid, sizeof ssid, "%.*s", n, value); snprintf(pass, sizeof pass, "%.64s", bar + 1); }
        else       snprintf(ssid, sizeof ssid, "%.32s", value);
        config_set_wifi(ssid, pass); save = true;
        printf("config: wifi credentials stored; reboot to join '%s'\n", ssid);
    }
    else if (!strcmp(name, "factory_reset")) {  /* web app: "factory_reset=yes" */
        if (!strcmp(value, "yes")) { printf("master reset (web)\n"); config_erase_all(); sleep_ms(200); watchdog_reboot(0, 0, 0); }
    }
    else if (!strcmp(name, "reboot"))    { printf("rebooting\n"); sleep_ms(100); watchdog_reboot(0, 0, 0); }
    else if (!strcmp(name, "bootsel"))   { add_alarm_in_ms(6000, usb_boot_now, NULL, true); enter_usb_boot(); }
    else printf("cmd: unknown \"%s\"\n", name);
    if (save) printf(config_save() ? "config: saved\n" : "config: SAVE FAILED\n");
}

/* A small queue filled from the lwIP context, drained by the main loop. A
 * phone can send several changes in quick succession; none is lost unless more
 * than CMDQ arrive between two passes of the loop. */
#define CMDQ 8
static struct { char name[24]; char value[200]; } s_cmdq[CMDQ];
static volatile uint8_t s_cmd_head, s_cmd_tail;

void app_command(const char *name, const char *value)
{
    uint8_t next = (uint8_t)((s_cmd_head + 1) % CMDQ);
    if (next == s_cmd_tail) { printf("web: command queue full, dropped %s\n", name); return; }
    snprintf(s_cmdq[s_cmd_head].name, sizeof s_cmdq[0].name, "%s", name);
    snprintf(s_cmdq[s_cmd_head].value, sizeof s_cmdq[0].value, "%s", value ? value : "");
    __dmb();
    s_cmd_head = next;
}

static void drain_commands(void)
{
    while (s_cmd_tail != s_cmd_head) {
        char name[24], value[200];
        memcpy(name, s_cmdq[s_cmd_tail].name, sizeof name);
        memcpy(value, s_cmdq[s_cmd_tail].value, sizeof value);
        __dmb();
        s_cmd_tail = (uint8_t)((s_cmd_tail + 1) % CMDQ);
        printf("web: %s=%s\n", name, (strcmp(name, "wifi") && strcmp(name, "mqtt")) ? value : "(hidden)");
        s_last_input_ms = to_ms_since_boot(get_absolute_time());
        exec_command(name, value);
    }
}

/* ---- web UI glue --------------------------------------------------------------- */

static void json_escape(char *dst, size_t cap, const char *src)
{
    size_t n = 0;
    for (; *src && n + 2 < cap; src++) {
        if (*src == '"' || *src == '\\') { dst[n++] = '\\'; dst[n++] = *src; }
        else if ((unsigned char)*src < 0x20) dst[n++] = ' ';
        else dst[n++] = *src;
    }
    dst[n] = '\0';
}

size_t app_state_json(char *buf, size_t cap)
{
    const it8951_info_t *in = it8951_info();
    const config_t *c = config_get();
    char esc[200], mac[18];
    net_mac(mac, sizeof mac);
    size_t n = 0;
#define ADD(...) do { int k = snprintf(buf + n, cap > n ? cap - n : 0, __VA_ARGS__); if (k > 0) n += (size_t)k; } while (0)
    ADD("{\"fw\":\"%s\",\"panel\":{\"w\":%u,\"h\":%u,\"lut\":\"%s\",\"fw\":\"%s\",\"addr\":\"0x%08lx\"},",
        FW_VERSION, in->w, in->h, in->lut, in->fw, (unsigned long)in->img_addr);
    ADD("\"vcom_mv\":%ld,\"mirror\":%d,\"move_mode\":%u,\"spi_hz\":%lu,\"long_press_ms\":%lu,\"banner\":%d,",
        (long)it8951_cached_vcom(), it8951_mirror() ? 1 : 0, tune_move_mode,
        (unsigned long)it8951_spi_hz(), (unsigned long)tune_long_press_ms, tune_banner ? 1 : 0);
    ADD("\"ip\":\"%s\",\"mac\":\"%s\",\"rssi\":%d,\"psram\":%d,\"busy\":%d,\"paints\":%lu,\"last_stream_ms\":%lu,\"last_refresh_ms\":%lu,\"uptime_s\":%lu,",
        net_ip(), mac, net_rssi(), have_psram ? 1 : 0, painter_busy() ? 1 : 0, (unsigned long)it8951_paint_count(),
        (unsigned long)it8951_last_stream_ms(), (unsigned long)it8951_last_refresh_ms(),
        (unsigned long)(to_ms_since_boot(get_absolute_time()) / 1000));
    const local_state_t *ls = local_state();
    ADD("\"content\":\"%s\",\"screen\":\"%s\",", content_name(), local_screen_name(local_screen()));
    ADD("\"local\":{\"alarm\":\"%02d:%02d\",\"alarm_on\":%d,\"phase\":%d,\"rain_on\":%d,\"rain_lvl\":%d,\"lights_on\":%d,\"bright\":%d,"
        "\"ac_mode\":%d,\"sp\":%d.%d,\"vol\":%d,\"night\":%d,\"on_battery\":%d,\"dash_page\":%d},",
        ls->alarm_h, ls->alarm_m, ls->alarm_on, (int)ls->phase, ls->rain_on, ls->rain_lvl, ls->lights_on, ls->bright,
        ls->ac_mode, ls->sp_x2 / 2, (ls->sp_x2 % 2) * 5, ls->vol, ls->night, ls->on_battery, ls->dash_page);
    ADD("\"live\":"); n += live_json(buf + n, cap > n ? cap - n : 0); ADD(",");
    { extern volatile int g_flash_step;
      ADD("\"loop\":{\"phase\":\"%s\",\"n\":%lu,\"flash_step\":%d},", (const char *)s_phase, (unsigned long)s_loops, g_flash_step); }
    ADD("\"build\":\"%s %s\",", __DATE__, __TIME__);
    ADD("\"ota\":{\"state\":\"%s\",\"pct\":%d,\"err\":\"%s\",\"to\":\"%s\",\"slot\":%d,\"ab\":%d,\"checked_s\":%ld,\"rel\":[",
        ota_state_name(), ota_percent(), ota_error(), ota_target_version(), ota_boot_partition(), ota_partitioned() ? 1 : 0,
        ota_checked_ms() ? (long)((to_ms_since_boot(get_absolute_time()) - ota_checked_ms()) / 1000) : -1L);
    for (int i = 0; i < ota_release_count(); i++) { const ota_release_t *e = ota_release(i);
        ADD("%s{\"v\":\"%s\",\"date\":\"%s\",\"pre\":%d,\"ok\":%d}", i ? "," : "", e->ver, e->date, e->pre, e->has_manifest); }
    ADD("]},");
    ADD("\"mqtt\":"); n += ha_status_json(buf + n, cap > n ? cap - n : 0); ADD(",");
    ADD("\"audio\":{\"sr\":%d,\"listeners\":%d,\"cpu_pm\":%d},", AUDIO_SR, audio_listeners(), audio_cpu_permille());
    ADD("\"ui\":"); n += local_state_json(buf + n, cap > n ? cap - n : 0); ADD(",");
    ADD("\"inputs\":{\"encoder\":%d,\"keys\":%d,\"position\":%ld,\"backlight\":%u,\"night\":%d,\"labels\":[",
        inputs_have_encoder() ? 1 : 0, inputs_have_keys() ? 1 : 0, (long)inputs_encoder_position(),
        inputs_backlight(), inputs_night() ? 1 : 0);
    for (int i = 0; i < 4; i++) { json_escape(esc, sizeof esc, local_key_label(i)); ADD("%s\"%s\"", i ? "," : "", esc); }
    ADD("],\"bindings\":{");
    for (int i = 0; i < CONFIG_BINDINGS; i++) { json_escape(esc, sizeof esc, c->bind[i]); ADD("%s\"%s\":\"%s\"", i ? "," : "", config_bind_name(i), esc); }
    ADD("},\"actions\":[");
    for (int i = 0; i < local_action_count(); i++) ADD("%s{\"id\":\"%s\",\"label\":\"%s\"}", i ? "," : "", local_action_id(i), local_action_name(i));
    ADD("]},\"tesserae\":{");
    json_escape(esc, sizeof esc, c->server_url);      ADD("\"server\":\"%s\",", esc);
    json_escape(esc, sizeof esc, config_device_id()); ADD("\"device_id\":\"%s\",", esc);
    json_escape(esc, sizeof esc, c->device_name);     ADD("\"name\":\"%s\",", esc);
    json_escape(esc, sizeof esc, config_kind());      ADD("\"kind\":\"%s\",", esc);
    ADD("\"paired\":%d,\"has_code\":%d,\"state\":\"%s\",", c->device_token[0] ? 1 : 0, c->pairing_code[0] ? 1 : 0, tesserae_state_name());
    json_escape(esc, sizeof esc, tesserae_last_error()); ADD("\"error\":\"%s\",", esc);
    ADD("\"etag\":\"%s\",\"frames\":%lu,\"poll_s\":%ld,\"next_in_s\":%lu,\"local_time\":\"%s\",\"has_frame\":%d}}",
        tesserae_frame_etag(), (unsigned long)tesserae_frames_painted(), (long)tesserae_poll_s(),
        (unsigned long)tesserae_next_poll_in_s(), tesserae_local_time(), tesserae_frame() ? 1 : 0);
#undef ADD
    return n < cap ? n : cap - 1;
}

/* Preview: the composite generator decimated by `scale`, in PSRAM past the
 * frame buffers. Without PSRAM there is no preview (nor dashboards). */
#define PREVIEW_PSRAM_OFF  (4u * 1024 * 1024)
static size_t   preview_len;
static uint8_t *preview_buf;

const uint8_t *app_render_preview(int scale, size_t *len)
{
    if (scale == 0) { *len = preview_len; return preview_len ? preview_buf : NULL; }
    if (!have_psram) { *len = 0; return NULL; }
    if (scale < 2) scale = 2;
    if (scale > 8) scale = 8;
    preview_buf = PSRAM_XIP_BASE + PREVIEW_PSRAM_OFF;

    int w = (IT8951_W / scale) & ~1, h = IT8951_H / scale;
    uint8_t *dst = preview_buf;
    dst[0] = (uint8_t)w; dst[1] = (uint8_t)(w >> 8); dst[2] = (uint8_t)h; dst[3] = (uint8_t)(h >> 8);
    dst += 4;
    static uint8_t row[IT8951_ROW_BYTES];
    for (int j = 0; j < h; j++) {
        composite_row(j * scale, row, NULL);
        uint8_t *o = dst + (size_t)j * (size_t)(w / 2);
        for (int i = 0; i < w; i += 2) {
            int xa = i * scale, xb = (i + 1) * scale;
            uint8_t ga = (uint8_t)((row[xa >> 1] >> ((xa & 1) ? 0 : 4)) & 0x0F);
            uint8_t gb = (uint8_t)((row[xb >> 1] >> ((xb & 1) ? 0 : 4)) & 0x0F);
            o[i >> 1] = (uint8_t)((ga << 4) | gb);
        }
    }
    preview_len = 4 + (size_t)(w / 2) * (size_t)h;
    *len = preview_len;
    return preview_buf;
}

/* ---- serial ------------------------------------------------------------------- */

static void help(void)
{
    printf("keys: m menu | j/k or arrows turn knob | Enter/space press knob | Esc/q hold knob\n"
           "      f fetch frame now | l/r send key left/right | t test card | g dashboard\n"
           "      c clear (INIT) | w white (GC16) | v<mV>Enter set VCOM | V read VCOM\n"
           "      1-4 press key | i re-init | z sleep | e enhance driving | d pin discovery+sweep | p pairing status | ? help\n"
           "button (BOOT/GP45 or GP%d): short = menu/move, long (>%lu ms) = select/refresh\n",
           BTN_PIN, (unsigned long)tune_long_press_ms);
    if (net_connected()) printf("web UI: http://%s/\n", net_ip());
}

static void pairing_status(void)
{
    const config_t *c = config_get();
    char mac[18]; net_mac(mac, sizeof mac);
    printf("tesserae: server=%s id=%s kind=%s mac=%s state=\"%s\" token=%s etag=%s poll=%lds frames=%lu%s%s\n",
           c->server_url[0] ? c->server_url : "(none)", config_device_id(), config_kind(), mac, tesserae_state_name(),
           c->device_token[0] ? "yes" : "no", c->last_frame_etag[0] ? c->last_frame_etag : "-",
           (long)tesserae_poll_s(), (unsigned long)tesserae_frames_painted(),
           tesserae_last_error()[0] ? " error=" : "", tesserae_last_error());
}

/* ---- console: everything printed, kept for the web app's Console tab ---- */
#include "pico/stdio/driver.h"
/* A small SRAM ring until PSRAM is up, then 64 KB in PSRAM (log_use_psram). */
static char              s_log_sram[2048];
static char             *s_log = s_log_sram;
static uint32_t          s_log_cap = sizeof s_log_sram;
#define LOG_RING s_log_cap
static volatile uint32_t s_log_w;          /* bytes written, ever */
static void log_out(const char *buf, int len)
{
    uint32_t w = s_log_w;
    for (int i = 0; i < len; i++) s_log[(w + (uint32_t)i) % LOG_RING] = buf[i];
    s_log_w = w + (uint32_t)len;
}
static stdio_driver_t s_log_drv = { .out_chars = log_out };

static void log_use_psram(char *buf, uint32_t cap)
{
    uint32_t w = s_log_w, keep = w < s_log_cap ? w : s_log_cap;
    for (uint32_t i = 0; i < keep; i++) buf[(w - keep + i) % cap] = s_log[(w - keep + i) % s_log_cap];
    s_log = buf; s_log_cap = cap;           /* positions stay valid: only the ring grew */
}

/* Log text from position `from` (bytes ever written); returns bytes copied and
 * where the next read starts. A reader too far behind skips to the oldest kept. */
size_t app_log_read(uint32_t from, char *dst, size_t cap, uint32_t *next)
{
    uint32_t w = s_log_w;
    if (from > w || w - from > LOG_RING) from = w > LOG_RING ? w - LOG_RING : 0;
    size_t n = w - from; if (n > cap) n = cap;
    for (size_t i = 0; i < n; i++) dst[i] = s_log[(from + (uint32_t)i) % LOG_RING];
    *next = from + (uint32_t)n;
    return n;
}

/* The newest `cap` bytes of the log (for the boot screen). */
static size_t app_log_tail(char *dst, size_t cap)
{
    uint32_t w = s_log_w, from = w > cap ? w - (uint32_t)cap : 0, next;
    return app_log_read(from, dst, cap, &next);
}

static void paint_pump(void);
/* While WiFi joins: keep the boot log moving. */
static void boot_wait(void) { boot_service(); paint_pump(); }

static void handle_key(int ch);
/* Typed into the web console: the same handler as the USB serial port. */
static void console_input(const char *text)
{
    printf("> %s\n", text);
    for (const char *p = text; *p; p++) handle_key((unsigned char)*p);
    handle_key(0);                          /* ends a v<mV> entry; otherwise ignored */
}

static void handle_key(int ch)
{
    static bool collecting_vcom;
    static int  vcom_digits;
    static unsigned vcom_val;

    if (collecting_vcom) {
        if (ch >= '0' && ch <= '9') { vcom_val = vcom_val * 10 + (unsigned)(ch - '0'); vcom_digits++; putchar(ch); return; }
        collecting_vcom = false;
        putchar('\n');
        if (vcom_digits > 0 && vcom_val >= 500 && vcom_val <= 5000) { painter_lock(); it8951_set_vcom((uint16_t)vcom_val); painter_unlock(); }
        else printf("vcom: expected v<500..5000> (millivolts, e.g. v1480)\n");
        return;
    }
    if (ch == 0x1B) {
        int c2 = getchar_timeout_us(5000);
        if (c2 == '[') {
            int c3 = getchar_timeout_us(5000);
            if (c3 == 'A') on_input((input_event_t){ IN_TURN, -1 });
            else if (c3 == 'B') on_input((input_event_t){ IN_TURN, +1 });
            return;
        }
        on_input((input_event_t){ IN_PRESS_LONG, 0 });
        return;
    }
    switch (ch) {
    case 'm': local_action("menu"); local_flush(); return;
    case 'j': case '\t': on_input((input_event_t){ IN_TURN, +1 }); return;
    case 'k':            on_input((input_event_t){ IN_TURN, -1 }); return;
    case 'q':            on_input((input_event_t){ IN_PRESS_LONG, 0 }); return;
    case '\r': case '\n': case ' ':
        on_input((input_event_t){ IN_PRESS_SHORT, 0 }); return;
    case 'f': tesserae_kick(true); return;
    case 'l': tesserae_button("left"); return;
    case 'r': tesserae_button("right"); return;
    case 't': show_content(CONTENT_CARD); return;
    case 'g': show_content(CONTENT_DASHBOARD); return;
    case 'c': clear_screen(IT8951_MODE_INIT); return;
    case 'w': clear_screen(IT8951_MODE_GC16); return;
    case 'v': collecting_vcom = true; vcom_digits = 0; vcom_val = 0; printf("vcom: v"); return;
    case 'V': { painter_lock(); uint16_t v = it8951_get_vcom(); painter_unlock(); printf("vcom: -%u.%02uV (%u mV)\n", v / 1000, (v % 1000) / 10, v); return; }
    case 'i': { painter_lock(); it8951_init(); painter_unlock(); } return;
    case 'z': { painter_lock(); it8951_sleep(); painter_unlock(); } return;
    case 'e': { painter_lock(); it8951_enhance_driving(); painter_unlock(); } return;
    case 'd': { painter_lock(); diag_pins(); it8951_probe_matrix(); it8951_init(); painter_unlock(); } return;
    case 'p': pairing_status(); return;
    case '?': case 'h': help(); return;
    case '1': case '2': case '3': case '4': on_key_down(ch - '1'); on_key_up(ch - '1'); return;
    default: return;
    }
}

/* ---- config seeding ------------------------------------------------------------ */

/* secrets.h is a first-boot seed and a development override: any value it
 * defines is applied (and re-saved if it changed) on every boot. The web UI
 * edits the stored copy; drop the define from secrets.h to stop overriding. */
static void config_seed(void)
{
    bool had = config_load();
    config_t before = *config_get();
#ifdef WIFI_SSID
    config_set_wifi(WIFI_SSID, WIFI_PASS);
#endif
#ifdef SERVER_URL
    if (strcmp(before.server_url, SERVER_URL) != 0) {
        config_set_server(SERVER_URL);
        config_set_device_token("");           /* a different server means a different token */
    }
#endif
#ifdef DEVICE_ID
    if (!before.device_id[0]) config_set_device_id(DEVICE_ID);   /* a server-adopted id wins */
#endif
#ifdef DEVICE_NAME
    config_set_device_name(DEVICE_NAME);
#endif
#ifdef DEVICE_KIND
    if (strcmp(before.kind, DEVICE_KIND) != 0) { config_set_kind(DEVICE_KIND); config_set_device_token(""); }
#endif
#ifdef PAIRING_CODE
    if (!before.device_token[0]) config_set_pairing_code(PAIRING_CODE);
#endif
    if (!had || memcmp(&before, config_get(), sizeof(config_t)) != 0)
        printf(config_save() ? "config: saved (seeded from secrets.h / defaults)\n" : "config: SAVE FAILED\n");
    else
        printf("config: loaded from flash\n");
    const config_t *c = config_get();
    printf("config: wifi='%s' server='%s' id='%s' token=%s etag=%s\n",
           c->wifi_ssid, c->server_url, config_device_id(), c->device_token[0] ? "yes" : "no",
           c->last_frame_etag[0] ? c->last_frame_etag : "-");
    if (c->mirror_h != 0xFF) it8951_set_mirror(c->mirror_h != 0);
    inputs_set_backlight(c->backlight);
    inputs_set_night(c->night != 0);
}

/* ---- main --------------------------------------------------------------------- */

/* Inputs, web commands and repaints. Called from the main loop and, through
 * the REST client's idle hook, while a network request is in flight, so the
 * knob and keys never wait on WiFi. Not re-entrant. */
static void ui_service(void);

/* Tesserae is about to download into `buf`: wait while core 1 still paints it. */
static void frame_buffer_wait(const uint8_t *buf)
{
    while (painter_busy() && P.frame == buf) { ui_service(); sleep_ms(2); }
}

/* Feed the sound generator the current sleep-sound settings. */
/* The Recording sound's file: the chosen one, or the first sound file stored. */
static const store_ent_t *recording_file(const char *name)
{
    if (name[0]) return store_find(name);
    for (int i = 0; i < store_count(); i++) {
        const store_ent_t *e = store_get(i);
        size_t l = strlen(e->name);
        if (l > 4 && !strcasecmp(e->name + l - 4, ".adp")) return e;
    }
    return NULL;
}

static void audio_update(void)
{
    bool changed = false;
    float fade = local_sleep_tick(&changed);
    local_alarm_tick(&changed);
    if (changed) local_flush();
    const local_state_t *ls = local_state();
    const store_ent_t *rec = ls->rain_kind == RS_RECORDING ? recording_file(ls->rec) : NULL;
    rs_params_t p = { .on = ls->rain_on, .kind = ls->rain_kind, .level = ls->rain_lvl, .volume = ls->vol, .fade = fade,
                      .patter = ls->patter, .wash = ls->wash, .tone = ls->tone, .wind = ls->wind, .surface = ls->surface,
                      .thunder = ls->thunder, .thunder_near = ls->thunder_near, .strike = ls->strike,
                      .chorus = ls->chorus, .birds = ls->birds, .bird_vol = ls->bird_vol,
                      .audition = ls->audition, .audition_bird = ls->audition_bird,
                      .rec = rec ? store_data(rec) : NULL, .rec_len = rec ? rec->size : 0 };
    audio_set(&p);

    /* The speaker: on while there's something to hear (a bird audition or a
     * thunder strike plays for a while after it's asked for). */
    static uint32_t last_aud, last_strike, extra_until;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (ls->audition != last_aud || ls->strike != last_strike) { last_aud = ls->audition; last_strike = ls->strike; extra_until = now + 15000; }
    audio_speaker_enable(ls->speaker);
    audio_speaker_audible((ls->rain_on && ls->vol > 0) || ls->chorus > 0 || (int32_t)(extra_until - now) > 0);
    audio_speaker_tick();

    /* The waveform temperature from the settings (0 = the firmware default). */
    {   int want = ls->wf_temp > 0 ? ls->wf_temp : 20;
        if (want != it8951_temp_forced()) { painter_lock(); it8951_set_temp(want); painter_unlock(); }
        if (it8951_reinit_pending()) { printf("paint: full repaint after the controller re-init\n"); local_set_dirty_full(); local_flush(); }
    }

    /* The PowerBoost, when battery backup is fitted. */
    if (ls->power_mon && power_tick()) local_power(power_on_battery(), power_percent(), power_low());
}

static void ui_service(void)
{
    static bool in;
    if (in) return;
    in = true;
    /* While the panel changes page, leave input unread (the seesaw chips hold
     * it) and drop it when the paint ends; otherwise act on it straight away. */
    if (!(painter_busy() && s_job_screen)) {
        for (int i = 0; i < 4; i++) {
            input_event_t e = inputs_poll();
            if (e.kind == IN_NONE) break;
            on_input(e);
        }
    }
    if (s_usb_flash) enter_usb_boot();
    drain_commands();
    audio_update();
    ha_poll();
    {   /* the live page vs the server's frames: follow the chosen dashboard */
        static int last_live = -1;
        int live = local_live_active() ? 1 : 0;
        bool booting = cur_content == CONTENT_BOOT && (int32_t)(to_ms_since_boot(get_absolute_time()) - s_boot_hold_until) < 0;
        if (cur_content == CONTENT_SETUP) {
            if (s_setup_until && (int32_t)(to_ms_since_boot(get_absolute_time()) - s_setup_until) >= 0) {
                printf("setup: nothing saved; restarting to try the WiFi again\n"); sleep_ms(200); watchdog_reboot(0, 0, 0);
            }
        }
        else if (booting) boot_service();
        else if (live != last_live || cur_content == CONTENT_BOOT) { last_live = live; show_content(live ? CONTENT_LIVE : (tesserae_frame() ? CONTENT_DASHBOARD : CONTENT_LIVE)); }
    }
    if (local_live_tick()) local_flush();
    if (!painter_busy() && local_settle_tick()) local_flush();   /* the knob stopped: menus back to full quality */
    if (!painter_busy() && !s_flush_pending && it8951_rails_live()) {   /* quiet for a while: the panel's rails off (they drift the image) */
        painter_lock(); bool off = it8951_idle_tick(); painter_unlock();
        if (off) printf("it8951: panel power off (idle)\n");
    }
    switch (local_take_request()) {               /* asked for from the panel's settings */
    case LREQ_REFRESH: printf("paint: full refresh (settings)\n"); local_set_dirty_full(); local_flush(); break;
    case LREQ_RESTART: printf("rebooting (settings)\n"); sleep_ms(200); watchdog_reboot(0, 0, 0); break;
    case LREQ_SETUP:   printf("rebooting into WiFi setup\n"); watchdog_hw->scratch[0] = SETUP_MAGIC; sleep_ms(200); watchdog_reboot(0, 0, 0); break;
    case LREQ_RESET:   printf("master reset (settings)\n"); config_erase_all(); sleep_ms(200); watchdog_reboot(0, 0, 0); break;
    default: break;
    }
    net_poll();
    paint_pump();
    in = false;
}

int main(void)
{
    stdio_init_all();
    stdio_set_driver_enabled(&s_log_drv, true);   /* the web console's copy of the log */
    painter_init();
    sleep_ms(1500);
    printf("\ntesserae-device-bedside %s (built %s %s): Pico Plus 2 W + Waveshare 10.3\" IT8951, Tesserae client\n", FW_VERSION, __DATE__, __TIME__);

    size_t psram = psram_init(PSRAM_CS_PIN_PLUS2);
    have_psram = (psram >= IT8951_FRAME_BYTES);
    if (have_psram) log_use_psram((char *)(PSRAM_XIP_BASE + 7u * 1024 * 1024 + 768u * 1024), 64u * 1024);   /* 7.75 MB */
    if (have_psram) {   /* 1 MB heap for HTTPS (mbedTLS) and big JSON (the GitHub release list) */
        psram_heap_init(PSRAM_XIP_BASE + 4u * 1024 * 1024 + 512u * 1024, 1024u * 1024);
        cJSON_Hooks hooks = { psram_heap_malloc, psram_heap_free };
        cJSON_InitHooks(&hooks);
    }
    printf("psram: %s (%u bytes)\n", have_psram ? "ready" : "MISSING: dashboard frames need it", (unsigned)psram);

    inputs_init();          /* before config_seed() so the stored backlight applies */
    power_init();
    audio_init_pins();      /* the amp shut down until there's sound */
    config_seed();

    gpio_init(BTN_PIN);      gpio_set_dir(BTN_PIN, GPIO_IN);      gpio_pull_up(BTN_PIN);
    gpio_init(BOOT_BTN_PIN); gpio_set_dir(BOOT_BTN_PIN, GPIO_IN); gpio_pull_up(BOOT_BTN_PIN);

    if (!it8951_init()) {
        printf("no controller; running the pin discovery (press d to repeat, i to retry init)\n");
        diag_pins();
        it8951_probe_matrix();
    } else {
        printf("paint: INIT clear (deghost)\n");
        it8951_clear(IT8951_MODE_INIT);
    }

    /* Test card first so the panel is not blank while we associate, and so the
     * orientation can be checked by eye (black square top-left). */
    /* The panel is up: commit an image that was just installed (it has ~16 s to
     * do so before the boot ROM goes back to the old one), and make sure only
     * the running image can boot. Before core 1 starts: the commit writes flash,
     * and core 1 runs from flash (a commit under it killed core 1). */
    if (have_psram) ota_init(PSRAM_XIP_BASE + 6u * 1024 * 1024 + 512u * 1024, 1024u * 1024);   /* 1 MB chunks */
    ota_set_version(FW_VERSION);
    ota_boot_check();
    store_init();
    if (have_psram) net_set_upload_buffer(PSRAM_XIP_BASE + 5u * 1024 * 1024 + 512u * 1024);   /* 5.5 MB, 64 KB */
    if (have_psram) audio_use_buffer((int16_t *)(PSRAM_XIP_BASE + 6u * 1024 * 1024), 4u * AUDIO_SR);   /* ~4 s */
    painter_start(paint_row, audio_core1_start);
    boot_collect();
    show_content(CONTENT_BOOT);
    net_set_wait_hook(boot_wait);

    const config_t *c = config_get();
    /* WiFi setup when there is no network saved, when asked for (Settings >
     * Device > Wi-Fi setup), or when the saved one can't be joined: then for 5
     * minutes, after which the panel restarts and tries it again (a router
     * outage must not strand it in setup). */
    bool asked = watchdog_hw->scratch[0] == SETUP_MAGIC;
    watchdog_hw->scratch[0] = 0;
    bool up = false;
    if (config_has_wifi() && !asked) up = net_init(c->wifi_ssid, c->wifi_pass);
    if (!up) {
        char ap[33], id[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
        pico_get_unique_board_id_string(id, sizeof id);
        snprintf(ap, sizeof ap, "Bedside-setup-%s", id + strlen(id) - 4);
        if (!config_has_wifi()) snprintf(s_setup_why, sizeof s_setup_why, "No WiFi set up yet.");
        else if (asked) snprintf(s_setup_why, sizeof s_setup_why, "Setup asked for from the menu. Restarts in 10 minutes if nothing is saved.");
        else snprintf(s_setup_why, sizeof s_setup_why, "Couldn't join \"%s\". Trying it again in 5 minutes.", c->wifi_ssid);
        if (config_has_wifi()) s_setup_until = to_ms_since_boot(get_absolute_time()) + (asked ? 600000u : 300000u);
        net_start_setup(ap);
        show_content(CONTENT_SETUP);
    }
    net_set_wait_hook(NULL);
    s_boot_hold_until = to_ms_since_boot(get_absolute_time()) + 4000;   /* time to read the log */
    /* Two frame buffers: the one on screen, and one downloads land in. */
    tesserae_init(have_psram ? PSRAM_XIP_BASE : NULL, have_psram ? PSRAM_XIP_BASE + 2u * 1024 * 1024 : NULL,
                  have_psram ? IT8951_FRAME_BYTES : 0, on_frame);
    (void)up;
    if (have_psram) tesserae_set_scratch(PSRAM_XIP_BASE + 7u * 1024 * 1024 + 512u * 1024, 256u * 1024);   /* frames.json */
    local_init();
    ha_init();
    live_init();
    rest_set_idle_hook(ui_service);
    tesserae_set_buffer_guard(frame_buffer_wait);
    local_set_dirty_full(); local_flush();      /* top bar with the key labels */
    banner_update();

    help();
    pairing_status();

    while (true) {
        /* Stall watch: any phase of the loop that holds the UI for long is
         * logged with its name and duration (see README "Diagnostics"). */
        uint32_t t0 = to_ms_since_boot(get_absolute_time()), tp = t0;
        #define PHASE(name) do { s_phase = name; s_loops++; uint32_t _n = to_ms_since_boot(get_absolute_time()); \
            if (_n - tp > 400) { printf("stall: %s took %lu ms\n", name, (unsigned long)(_n - tp)); } tp = _n; } while (0)
        press_t p = poll_press(button_down);
        if (p != PRESS_NONE) on_press(p);
        PHASE("buttons");

        int ch = getchar_timeout_us(0);
        if (ch != PICO_ERROR_TIMEOUT) handle_key(ch);

        ui_service();
        PHASE("ui");

        tess_state_t before = tesserae_state();
        bool had_err = tesserae_last_error()[0] != '\0';
        if (!ota_busy()) tesserae_poll();           /* an update has the network to itself */
        ota_tick();
        local_ota_service();
        PHASE("server");
        if (tesserae_state() != before || (tesserae_last_error()[0] != '\0') != had_err) banner_update();
        if (tesserae_local_time()[0]) local_set_time(tesserae_local_time());
        local_persist_tick();
        paint_pump();
        cleanup_if_idle();
        PHASE("paint");
        (void)t0;

        sleep_ms(10);
    }
}
