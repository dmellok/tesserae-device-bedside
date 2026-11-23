/*
 * sim_panel.c: the IT8951 e-paper controller and its glass, simulated (the
 * it8951.h and epd_io.h APIs, in place of src/it8951.c and src/epd_io.c).
 *
 * Two images, as on the real panel: the controller's image memory (what the
 * firmware loaded) and the glass (what you see). A refresh copies one into the
 * other the way the waveform would:
 *   INIT        flashes, ends white
 *   GC16        flashes, then 16 greys
 *   GL16        16 greys, no flash
 *   DU, A2      two levels only: a grey becomes black or white until a
 *               full-quality pass (GL16 / GC16, or a second pass) runs over it
 * and takes about as long as on the device (stream ~940 px/ms at the product's
 * 3.75 MHz SPI, then the waveform: INIT 1.7 s, GC16 / GL16 0.76 s, DU 0.43 s),
 * unless --instant. The flashes show on the glass, so timing bugs (a paint
 * that lands late, a menu wiped by a full refresh) show up as they do on the
 * device. The driver's own rule is kept too: a full GC16 starts with an INIT
 * once anything has been shown since the last one (s_ref_stale).
 */
#include "sim.h"
#include "sim_sdk.h"
#include "it8951.h"
#include "epd_io.h"

#include <unistd.h>

static uint8_t s_mem[IT8951_FRAME_BYTES];       /* the controller's image buffer */
static uint8_t s_glass[IT8951_FRAME_BYTES];     /* what the panel shows */
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile uint32_t s_version = 1;

static bool     s_up, s_ref_stale, s_clean_full = true, s_mirror, s_rails_on, s_rails_off_between = true, s_asleep, s_standby;
static int      s_temp_forced = 20, s_bitmap;
static uint16_t s_vcom = 1450;
static uint32_t s_spi_hz = 3750000, s_paints, s_last_stream, s_last_refresh, s_last_refresh_ms;
static it8951_info_t s_info = { IT8951_W, IT8951_H, 0x001236E0, "SIM", "SIM" };

/* The last refreshes, for the simulator page's overlay. */
#define EVENTS 16
static struct { uint32_t t; int x, y, w, h, mode; } s_ev[EVENTS];
static int s_nev;
static volatile bool s_busy;

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

/* Power on: a white panel (a real one keeps the last picture; this one starts clean). */
__attribute__((constructor)) static void glass_init(void) { memset(s_mem, 0xFF, sizeof s_mem); memset(s_glass, 0xFF, sizeof s_glass); }

uint32_t       sim_glass_version(void) { return s_version; }
const uint8_t *sim_glass(void)         { return s_glass; }
void           sim_glass_lock(bool l)  { if (l) pthread_mutex_lock(&s_lock); else pthread_mutex_unlock(&s_lock); }

static inline uint8_t px_get(const uint8_t *b, int x, int y) { uint8_t v = b[(size_t)y * IT8951_ROW_BYTES + (x >> 1)]; return (x & 1) ? (v & 0x0F) : (v >> 4); }
static inline void px_set(uint8_t *b, int x, int y, uint8_t g)
{
    uint8_t *p = &b[(size_t)y * IT8951_ROW_BYTES + (x >> 1)];
    *p = (x & 1) ? (uint8_t)((*p & 0xF0) | g) : (uint8_t)((*p & 0x0F) | (g << 4));
}

static void wait_ms_real(uint32_t ms) { if (!sim_opt.instant && ms) usleep(ms * 1000u); }

/* Clip a rectangle to the panel; x widened to whole 16-pixel words as the driver loads them. */
static bool clip(int *x, int *y, int *w, int *h)
{
    int x0 = *x & ~15, x1 = (*x + *w + 15) & ~15, y0 = *y, y1 = *y + *h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > IT8951_W) x1 = IT8951_W;
    if (y1 > IT8951_H) y1 = IT8951_H;
    if (x1 <= x0 || y1 <= y0) return false;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    return true;
}

/* Rows from the firmware's generator into the image buffer; returns the time it takes to stream them. */
static uint32_t load(int x, int y, int w, int h, it8951_row_fn fn, void *ctx)
{
    static uint8_t row[IT8951_ROW_BYTES];
    for (int j = y; j < y + h; j++) {
        fn(j, row, ctx);
        memcpy(&s_mem[(size_t)j * IT8951_ROW_BYTES + x / 2], &row[x / 2], (size_t)w / 2);
    }
    return (uint32_t)((uint64_t)w * (uint64_t)h / 937u) + 5u;
}

static void note(int x, int y, int w, int h, int mode)
{
    int i = s_nev++ % EVENTS;
    s_ev[i].t = now_ms(); s_ev[i].x = x; s_ev[i].y = y; s_ev[i].w = w; s_ev[i].h = h; s_ev[i].mode = mode;
}

static void fill(uint8_t *b, int x, int y, int w, int h, uint8_t g)
{
    for (int j = y; j < y + h; j++) memset(&b[(size_t)j * IT8951_ROW_BYTES + x / 2], g * 0x11, (size_t)w / 2);
}

static uint32_t waveform_ms(int mode)
{
    switch (mode) {
    case IT8951_MODE_INIT: return 1719;
    case IT8951_MODE_DU:   return 430;
    case IT8951_MODE_A2:   return 300;
    default:               return 762;    /* GC16, GL16 */
    }
}

/* Run the waveform over a box of the glass; `only` limits the two-level modes
 * to the rectangles that were loaded (partial update: unchanged pixels don't move). */
static void refresh(int x, int y, int w, int h, int mode, const int (*only)[4], int n_only)
{
    uint32_t t = waveform_ms(mode);
    s_rails_on = true;
    note(x, y, w, h, mode);
    bool flash = mode == IT8951_MODE_INIT || mode == IT8951_MODE_GC16;
    if (flash && !sim_opt.instant) {                   /* black, then white, then the image */
        pthread_mutex_lock(&s_lock); fill(s_glass, x, y, w, h, 0x0); s_version++; pthread_mutex_unlock(&s_lock);
        wait_ms_real(t * 3 / 10);
        pthread_mutex_lock(&s_lock); fill(s_glass, x, y, w, h, 0xF); s_version++; pthread_mutex_unlock(&s_lock);
        wait_ms_real(t * 3 / 10);
    } else wait_ms_real(t / 3);
    pthread_mutex_lock(&s_lock);
    if (mode == IT8951_MODE_DU || mode == IT8951_MODE_A2) {
        for (int k = 0; k < (only ? n_only : 1); k++) {
            int rx = only ? only[k][0] : x, ry = only ? only[k][1] : y, rw = only ? only[k][2] : w, rh = only ? only[k][3] : h;
            for (int j = ry; j < ry + rh; j++)
                for (int i = rx; i < rx + rw; i++) px_set(s_glass, i, j, px_get(s_mem, i, j) >= 8 ? 0xF : 0x0);
        }
    } else {
        for (int j = y; j < y + h; j++) memcpy(&s_glass[(size_t)j * IT8951_ROW_BYTES + x / 2], &s_mem[(size_t)j * IT8951_ROW_BYTES + x / 2], (size_t)w / 2);
    }
    s_version++;
    pthread_mutex_unlock(&s_lock);
    wait_ms_real(flash ? t * 4 / 10 : t * 2 / 3);
    s_last_refresh_ms = now_ms();
}

static void init_white(void)
{
    uint32_t t0 = now_ms();
    fill(s_mem, 0, 0, IT8951_W, IT8951_H, 0xF);
    wait_ms_real(1405);                                 /* the white frame at 2 bpp */
    uint32_t t1 = now_ms();
    refresh(0, 0, IT8951_W, IT8951_H, IT8951_MODE_INIT, NULL, 0);
    s_ref_stale = false;
    printf("it8951: white at 2bpp in %lu ms, INIT in %lu ms\n", (unsigned long)(t1 - t0), (unsigned long)(now_ms() - t1));
}

static void paint(int x, int y, int w, int h, it8951_row_fn fn, void *ctx, uint8_t mode)
{
    if (!clip(&x, &y, &w, &h)) return;
    s_busy = true;
    if (s_clean_full && mode == IT8951_MODE_GC16 && s_ref_stale && x <= 0 && y <= 0 && w >= IT8951_W && h >= IT8951_H) {
        printf("it8951: INIT first: the controller's reference is stale\n");
        init_white();
    }
    uint32_t t0 = now_ms();
    wait_ms_real(load(x, y, w, h, fn, ctx));
    uint32_t t1 = now_ms();
    refresh(x, y, w, h, mode, NULL, 0);
    s_ref_stale = mode != IT8951_MODE_INIT;
    uint32_t t2 = now_ms();
    s_last_stream = t1 - t0; s_last_refresh = t2 - t1; s_paints++;
    s_busy = false;
    printf("it8951: %dx%d@%d,%d streamed in %lu ms, refreshed (mode %u) in %lu ms\n",
           w, h, x, y, (unsigned long)(t1 - t0), mode, (unsigned long)(t2 - t1));
}

void it8951_show_pattern(it8951_row_fn fn, void *ctx, uint8_t mode)                         { paint(0, 0, IT8951_W, IT8951_H, fn, ctx, mode); }
void it8951_update_area(int x, int y, int w, int h, it8951_row_fn fn, void *ctx, uint8_t mode) { paint(x, y, w, h, fn, ctx, mode); }

static void frame_row(int y, uint8_t out[IT8951_ROW_BYTES], void *ctx) { memcpy(out, (const uint8_t *)ctx + (size_t)y * IT8951_ROW_BYTES, IT8951_ROW_BYTES); }
void it8951_show_frame(const uint8_t *frame, uint8_t mode) { paint(0, 0, IT8951_W, IT8951_H, frame_row, (void *)frame, mode); }

/* Several rectangles, one refresh over their bounding box (as the driver does). */
void it8951_update_areas(const int (*rects)[4], int n, it8951_row_fn fn, void *ctx, uint8_t mode)
{
    int a[24][4], loaded = 0;
    uint32_t t0 = now_ms(), stream = 0;
    s_busy = true;
    for (int i = 0; i < n && loaded < 24; i++) {
        int x = rects[i][0], y = rects[i][1], w = rects[i][2], h = rects[i][3];
        if (!clip(&x, &y, &w, &h)) continue;
        stream += load(x, y, w, h, fn, ctx);
        a[loaded][0] = x; a[loaded][1] = y; a[loaded][2] = w; a[loaded][3] = h; loaded++;
    }
    if (!loaded) { s_busy = false; return; }
    wait_ms_real(stream);
    uint32_t t1 = now_ms();
    int x0 = IT8951_W, y0 = IT8951_H, x1 = 0, y1 = 0;
    for (int i = 0; i < loaded; i++) {
        if (a[i][0] < x0) x0 = a[i][0];
        if (a[i][1] < y0) y0 = a[i][1];
        if (a[i][0] + a[i][2] > x1) x1 = a[i][0] + a[i][2];
        if (a[i][1] + a[i][3] > y1) y1 = a[i][1] + a[i][3];
    }
    refresh(x0, y0, x1 - x0, y1 - y0, mode, (const int (*)[4])a, loaded);
    s_ref_stale = true;
    uint32_t t2 = now_ms();
    s_last_stream = t1 - t0; s_last_refresh = t2 - t1; s_paints++;
    s_busy = false;
    printf("it8951: %d area(s) in parallel: streamed in %lu ms, refreshed (mode %u) in %lu ms\n",
           loaded, (unsigned long)(t1 - t0), mode, (unsigned long)(t2 - t1));
}

void it8951_redisplay_areas(const int (*rects)[4], int n, uint8_t mode)
{
    uint32_t t0 = now_ms();
    s_busy = true;
    for (int i = 0; i < n && i < 24; i++) {
        int x = rects[i][0], y = rects[i][1], w = rects[i][2], h = rects[i][3];
        if (clip(&x, &y, &w, &h)) refresh(x, y, w, h, mode, NULL, 0);
    }
    s_paints++;
    s_busy = false;
    printf("it8951: second pass over %d area(s) (mode %u) in %lu ms\n", n, mode, (unsigned long)(now_ms() - t0));
}

void it8951_clear(uint8_t mode)
{
    s_busy = true;
    fill(s_mem, 0, 0, IT8951_W, IT8951_H, 0xF);
    wait_ms_real(1405);
    refresh(0, 0, IT8951_W, IT8951_H, mode, NULL, 0);
    s_ref_stale = mode != IT8951_MODE_INIT;
    s_paints++;
    s_busy = false;
}

bool it8951_init(void)
{
    s_up = true; s_asleep = false;
    printf("it8951: simulated panel %ux%u, image buffer 0x%08lx, firmware %s, LUT %s\n",
           s_info.w, s_info.h, (unsigned long)s_info.img_addr, s_info.fw, s_info.lut);
    return true;
}
bool it8951_probe_blind(unsigned cs, unsigned miso, int mode, it8951_info_t *out, uint16_t raw[4]) { (void)cs; (void)miso; (void)mode; (void)raw; if (out) *out = s_info; return true; }
void it8951_probe_matrix(void) { printf("it8951: (simulated: no pins to probe)\n"); }
const it8951_info_t *it8951_info(void) { return &s_info; }
uint16_t it8951_get_vcom(void) { return s_vcom; }
void     it8951_set_vcom(uint16_t mv) { s_vcom = mv; printf("it8951: VCOM -%u mV (simulated)\n", mv); }
int32_t  it8951_cached_vcom(void) { return s_vcom; }
void     it8951_set_mirror(bool m) { s_mirror = m; }
bool     it8951_mirror(void) { return s_mirror; }
void     it8951_stream_window(int *x0, int *x1) { *x0 = 0; *x1 = IT8951_W; }
int      it8951_spi_test(int rows) { printf("it8951: SPI test, %d rows: 0 mismatched words (simulated)\n", rows); return 0; }
void     it8951_set_rails_off(bool on) { s_rails_off_between = on; printf("it8951: panel power off when idle %s\n", on ? "on" : "off"); if (!on) s_rails_on = true; }
bool     it8951_rails_off(void) { return s_rails_off_between; }
bool     it8951_rails_live(void) { return s_rails_on; }
bool     it8951_idle_tick(void)
{
    if (!s_rails_off_between || !s_rails_on || !s_up || s_asleep) return false;
    if (now_ms() - s_last_refresh_ms < 4000) return false;
    s_rails_on = false;
    return true;
}
void     it8951_set_spi_hz(uint32_t hz) { s_spi_hz = hz; }
uint32_t it8951_spi_hz(void) { return s_spi_hz; }
uint32_t it8951_paint_count(void) { return s_paints; }
uint32_t it8951_last_stream_ms(void) { return s_last_stream; }
uint32_t it8951_last_refresh_ms(void) { return s_last_refresh; }
void it8951_sleep(void) { s_asleep = true; s_rails_on = false; printf("it8951: sleep\n"); }
void it8951_enhance_driving(void) { printf("it8951: enhance driving (simulated)\n"); }
void it8951_set_temp(int c) { s_temp_forced = c; }
int  it8951_temp_forced(void) { return s_temp_forced; }
int  it8951_temp_sensed(void) { return 23; }
int  it8951_temp_forced_rb(void) { return s_temp_forced; }
int  it8951_sense_temp(void) { return 23; }
bool it8951_reinit_pending(void) { return false; }
void it8951_set_clean_full(bool on) { s_clean_full = on; }
bool it8951_clean_full(void) { return s_clean_full; }
void it8951_set_bitmap(int mode) { s_bitmap = mode; }
int  it8951_bitmap(void) { return s_bitmap; }
void it8951_set_standby(bool on) { s_standby = on; printf("it8951: standby between updates %s\n", on ? "on" : "off"); }

size_t sim_panel_json(char *b, size_t cap)
{
    static const char *const MODES[8] = { "INIT", "DU", "GC16", "GL16", "4", "5", "A2", "7" };
    uint32_t now = now_ms();
    size_t n = (size_t)snprintf(b, cap, "{\"version\":%u,\"busy\":%d,\"rails\":%d,\"paints\":%u,\"events\":[",
                                s_version, s_busy, s_rails_on, s_paints);
    int first = s_nev > EVENTS ? s_nev - EVENTS : 0;
    for (int k = first; k < s_nev && n < cap - 96; k++) {
        int i = k % EVENTS;
        n += (size_t)snprintf(b + n, cap - n, "%s{\"age\":%u,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"mode\":\"%s\"}",
                              k > first ? "," : "", now - s_ev[i].t, s_ev[i].x, s_ev[i].y, s_ev[i].w, s_ev[i].h, MODES[s_ev[i].mode & 7]);
    }
    n += (size_t)snprintf(b + n, cap - n, "]}");
    return n;
}

/* ---- epd_io: the SPI link to the controller; nothing to drive here ---- */
bool epd_io_init(uint cs, uint miso, uint32_t hz) { (void)cs; (void)miso; (void)hz; return true; }
void epd_io_set_drive(int ma, bool fast) { (void)ma; (void)fast; }
int  epd_io_drive_ma(void) { return EPD_DRIVE_MA; }
bool epd_io_fast_slew(void) { return EPD_FAST_SLEW; }
void epd_io_set_mode(int m) { (void)m; }
int  epd_io_mode(void) { return 0; }
void epd_io_set_swap(bool s) { (void)s; }
bool epd_io_swap(void) { return false; }
void epd_io_deinit(void) {}
uint epd_io_cs_pin(void) { return EPD_PIN_CS; }
uint epd_io_miso_pin(void) { return EPD_PIN_MISO; }
void epd_reset(void) {}
bool epd_hrdy(void) { return true; }
bool epd_wait_hrdy(uint32_t t) { (void)t; return true; }
void epd_cs(bool a) { (void)a; }
void epd_spi_write(const uint8_t *s, size_t n) { (void)s; (void)n; }
void epd_spi_read(uint8_t *d, size_t n) { memset(d, 0, n); }
void epd_spi_write16(uint16_t v) { (void)v; }
uint16_t epd_spi_read16(void) { return 0; }
void epd_spi_write_async(const uint8_t *s, size_t n) { (void)s; (void)n; }
void epd_spi_write_wait(void) {}
