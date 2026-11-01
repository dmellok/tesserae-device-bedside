/*
 * test_paint.c: a bare paint test for the IT8951 + 10.3" panel. Nothing else
 * runs: no WiFi, no audio, no UI. The controller is driven with Waveshare's
 * reference sequence (EPD_IT8951.c from waveshareteam/IT8951-ePaper), and
 * one thing at a time can be changed from the USB serial port while someone
 * looks at the glass.
 *
 * Build and load:  pio run -e paintest;  tools/flash.sh paintest
 * Then (115200 baud, DTR on):
 *   ?        this help            s   status: dev info, VCOM, temperature (two words)
 *   r        INIT, then a 16-step wedge (vertical bars) with GC16
 *   R        the wedge with GC16, no INIT first
 *   h        INIT, then a 16-step wedge as horizontal bands
 *   b / w    INIT, then solid black / white with GC16
 *   k        INIT, then a checkerboard of 64 px squares (black/white)
 *   c        INIT clear only
 *   4 / 8    pixel format for loads: 4 bpp packed (ours) / 8 bpp (Waveshare's default)
 *   p        toggle packed write (one preamble per image) vs one preamble per word
 *   t<N>     force the waveform temperature to N C (t0 = back to the sensor)
 *   v<mV>    set VCOM, e.g. v1480
 *   m<N>     display mode for the next paints (0 INIT, 1 DU, 2 GC16, 3 GL16, 6 A2)
 *   e        Waveshare's "enhanced driving" (reg 0x0038 = 0x0602)
 *   i        reset and re-init the controller
 *   z<Hz>    SPI clock, e.g. z9375000 (re-inits)
 *   a        partial: reload the SAME pixels into the bottom-left 704x125 and refresh that area (mode m)
 *   A        the same partial without reloading (display only)
 *   n<N>     N partials ('a') two seconds apart, to watch for drift
 *   g        full GC16 of the current pattern with no INIT (a "second pass")
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "epd_io.h"

#define W 1872
#define H 1404

#define CMD_SYS_RUN      0x0001
#define CMD_REG_RD       0x0010
#define CMD_REG_WR       0x0011
#define CMD_LD_IMG_AREA  0x0021
#define CMD_LD_IMG_END   0x0022
#define CMD_DPY_AREA     0x0034
#define CMD_VCOM         0x0039
#define CMD_TEMP         0x0040
#define CMD_DEV_INFO     0x0302
#define REG_I80CPCR      0x0004
#define REG_LISAR        0x0208
#define REG_LUTAFSR      0x1224

static uint32_t img_addr;
static int  bpp = 4, mode = 2, packed = 1;
static int  force_temp = 0;
static uint32_t spi_hz = 12500000;
static int  last_pat = 0;

/* ---- the I80-over-SPI protocol, as Waveshare does it ---- */
static void busy(void) { while (!epd_hrdy()) tight_loop_contents(); }
static void wcmd(uint16_t c) { busy(); epd_cs(true); epd_spi_write16(0x6000); busy(); epd_spi_write16(c); epd_cs(false); }
static void wdat(uint16_t d) { busy(); epd_cs(true); epd_spi_write16(0x0000); busy(); epd_spi_write16(d); epd_cs(false); }
static uint16_t rdat(void)   { busy(); epd_cs(true); epd_spi_write16(0x1000); busy(); (void)epd_spi_read16(); busy(); uint16_t v = epd_spi_read16(); epd_cs(false); return v; }
static void rdat_n(uint16_t *out, int n) { busy(); epd_cs(true); epd_spi_write16(0x1000); busy(); (void)epd_spi_read16(); busy(); for (int i = 0; i < n; i++) out[i] = epd_spi_read16(); epd_cs(false); }
static uint16_t rreg(uint16_t r) { wcmd(CMD_REG_RD); wdat(r); return rdat(); }
static void wreg(uint16_t r, uint16_t v) { wcmd(CMD_REG_WR); wdat(r); wdat(v); }
static void wait_lut(void) { uint32_t t0 = to_ms_since_boot(get_absolute_time()); while (rreg(REG_LUTAFSR)) { if (to_ms_since_boot(get_absolute_time()) - t0 > 30000) { printf("LUT engine still busy after 30 s\n"); break; } sleep_ms(10); } }

static uint16_t get_vcom(void) { wcmd(CMD_VCOM); wdat(0); return rdat(); }
static void set_vcom(uint16_t mv) { wcmd(CMD_VCOM); wdat(1); wdat(mv); }
static void temp_get(uint16_t *t) { wcmd(CMD_TEMP); wdat(0); sleep_ms(5); rdat_n(t, 2); }
static void temp_force(int c) { wcmd(CMD_TEMP); if (c > 0) { wdat(1); wdat((uint16_t)c); } else wdat(2); }

static bool init(void)
{
    epd_io_set_drive(12, true);
    if (!epd_io_init(EPD_PIN_CS, EPD_PIN_MISO, spi_hz)) { printf("epd_io_init failed\n"); return false; }
    epd_reset();
    if (!epd_wait_hrdy(4000)) { printf("HRDY never came up after reset\n"); return false; }
    wcmd(CMD_SYS_RUN);
    wcmd(CMD_DEV_INFO);
    uint16_t info[20]; rdat_n(info, 20);
    img_addr = (uint32_t)info[2] | ((uint32_t)info[3] << 16);
    char fw[17], lut[17]; memcpy(fw, &info[4], 16); fw[16] = 0; memcpy(lut, &info[12], 16); lut[16] = 0;
    printf("dev info: %ux%u, image buffer 0x%06lx, FW \"%s\", LUT \"%s\"\n", info[0], info[1], (unsigned long)img_addr, fw, lut);
    if (info[0] != W || info[1] != H) { printf("unexpected panel size: the link is not clean\n"); return false; }
    wreg(REG_I80CPCR, 0x0001);                 /* packed writes */
    uint16_t v = get_vcom();
    if (v != 1480) { set_vcom(1480); v = get_vcom(); }
    printf("VCOM -%u.%03u V\n", v / 1000, v % 1000);
    if (force_temp > 0) temp_force(force_temp);
    uint16_t t[2]; temp_get(t);
    printf("temperature: sensor %u, forced %u (0xFFFF = none)%s\n", t[0], t[1], force_temp ? " (forced here)" : " (controller's own choice)");
    return true;
}

/* ---- patterns, one row at a time ---- */
typedef enum { PAT_RAMP_V, PAT_RAMP_H, PAT_BLACK, PAT_WHITE, PAT_CHECK } pat_t;
static uint8_t grey_at(pat_t p, int x, int y)
{
    switch (p) {
    case PAT_RAMP_V:  return (uint8_t)(x * 16 / W);
    case PAT_RAMP_H:  return (uint8_t)(y * 16 / H);
    case PAT_BLACK:   return 0;
    case PAT_WHITE:   return 15;
    case PAT_CHECK:   return (((x / 64) + (y / 64)) & 1) ? 15 : 0;
    }
    return 15;
}

static void load_begin(void)
{
    wait_lut();
    wreg(REG_LISAR + 2, (uint16_t)(img_addr >> 16));
    wreg(REG_LISAR, (uint16_t)(img_addr & 0xFFFF));
    /* arg 0: endian (bit 8: 0 little), bpp (bits 4-5: 2 = 4bpp, 3 = 8bpp), rotate (bits 0-1) */
    uint16_t a0 = (uint16_t)((0 << 8) | ((bpp == 8 ? 3 : 2) << 4) | 0);
    wcmd(CMD_LD_IMG_AREA); wdat(a0); wdat(0); wdat(0); wdat(W); wdat(H);
    if (packed) { busy(); epd_cs(true); epd_spi_write16(0x0000); busy(); }
}
static void load_row(const uint8_t *bytes, int n)        /* n bytes = n/2 words, each word's bytes swapped (little-endian host words) */
{
    static uint8_t wire[W];
    for (int k = 0; k < n; k += 2) { wire[k] = bytes[k + 1]; wire[k + 1] = bytes[k]; }
    if (packed) { busy(); epd_spi_write(wire, (size_t)n); }
    else for (int k = 0; k < n; k += 2) wdat((uint16_t)((wire[k] << 8) | wire[k + 1]));
}
static void load_end(void) { if (packed) epd_cs(false); wcmd(CMD_LD_IMG_END); }

static void load_begin_area(int x, int y, int w, int h)
{
    wait_lut();
    wreg(REG_LISAR + 2, (uint16_t)(img_addr >> 16));
    wreg(REG_LISAR, (uint16_t)(img_addr & 0xFFFF));
    uint16_t a0 = (uint16_t)((0 << 8) | ((bpp == 8 ? 3 : 2) << 4) | 0);
    wcmd(CMD_LD_IMG_AREA); wdat(a0); wdat((uint16_t)x); wdat((uint16_t)y); wdat((uint16_t)w); wdat((uint16_t)h);
    if (packed) { busy(); epd_cs(true); epd_spi_write16(0x0000); busy(); }
}
/* A partial of the current pattern over (x, y, w, h): the same pixels the full
 * paint put there, so the controller sees old == new for every pixel. */
static void paint_area(pat_t p, int x, int y, int w, int h, bool reload)
{
    uint32_t t0 = to_ms_since_boot(get_absolute_time());
    static uint8_t row[W];
    if (reload) {
        load_begin_area(x, y, w, h);
        for (int yy = y; yy < y + h; yy++) {
            if (bpp == 8) { for (int i = 0; i < w; i++) row[i] = (uint8_t)(grey_at(p, x + i, yy) * 17); load_row(row, w); }
            else { for (int i = 0; i < w; i += 2) row[i / 2] = (uint8_t)((grey_at(p, x + i, yy) << 4) | grey_at(p, x + i + 1, yy)); load_row(row, w / 2); }
        }
        load_end();
    }
    uint32_t t1 = to_ms_since_boot(get_absolute_time());
    if (force_temp > 0) temp_force(force_temp);
    wcmd(CMD_DPY_AREA); wdat((uint16_t)x); wdat((uint16_t)y); wdat((uint16_t)w); wdat((uint16_t)h); wdat((uint16_t)mode);
    sleep_ms(20);
    wait_lut();
    printf("partial %dx%d at %d,%d%s, mode %d: load %lu ms, refresh %lu ms\n", w, h, x, y, reload ? " (same pixels reloaded)" : " (display only)", mode,
           (unsigned long)(t1 - t0), (unsigned long)(to_ms_since_boot(get_absolute_time()) - t1));
}

static void paint(pat_t p, bool init_first)
{
    last_pat = (int)p;
    uint32_t t0 = to_ms_since_boot(get_absolute_time());
    if (init_first) {
        wait_lut();
        wcmd(CMD_DPY_AREA); wdat(0); wdat(0); wdat(W); wdat(H); wdat(0);   /* INIT: erase to white, no image needed */
        wait_lut();
    }
    uint32_t t1 = to_ms_since_boot(get_absolute_time());
    static uint8_t row[W];
    load_begin();
    for (int y = 0; y < H; y++) {
        if (bpp == 8) { for (int x = 0; x < W; x++) row[x] = (uint8_t)(grey_at(p, x, y) * 17); load_row(row, W); }
        else { for (int x = 0; x < W; x += 2) row[x / 2] = (uint8_t)((grey_at(p, x, y) << 4) | grey_at(p, x + 1, y)); load_row(row, W / 2); }
    }
    load_end();
    uint32_t t2 = to_ms_since_boot(get_absolute_time());
    if (force_temp > 0) temp_force(force_temp);
    wcmd(CMD_DPY_AREA); wdat(0); wdat(0); wdat(W); wdat(H); wdat((uint16_t)mode);
    sleep_ms(20);
    wait_lut();
    uint32_t t3 = to_ms_since_boot(get_absolute_time());
    printf("painted: %s%s, %d bpp, %s, mode %d: INIT %lu ms, load %lu ms, refresh %lu ms\n",
           init_first ? "INIT + " : "", p == PAT_RAMP_V ? "wedge" : p == PAT_RAMP_H ? "bands" : p == PAT_BLACK ? "black" : p == PAT_WHITE ? "white" : "checker",
           bpp, packed ? "packed" : "word by word", mode, (unsigned long)(t1 - t0), (unsigned long)(t2 - t1), (unsigned long)(t3 - t2));
}

static void status(void)
{
    uint16_t v = get_vcom(), t[2]; temp_get(t);
    printf("status: VCOM -%u.%03u V, temperature sensor %u / forced %u, I80CPCR %u, reg 0x0038 = 0x%04x, %d bpp, mode %d, %s, SPI %lu Hz\n",
           v / 1000, v % 1000, t[0], t[1], rreg(REG_I80CPCR), rreg(0x0038), bpp, mode, packed ? "packed" : "word by word", (unsigned long)spi_hz);
}

int main(void)
{
    stdio_init_all();
    sleep_ms(1500);
    printf("\npaint test: IT8951 + 10.3\" panel, Waveshare's sequence. ? for help\n");
    bool up = init();
    char line[32]; int n = 0;
    while (true) {
        int ch = getchar_timeout_us(100000);
        if (ch == PICO_ERROR_TIMEOUT) continue;
        if (ch == '\r' || ch == '\n') {
            line[n] = 0; n = 0;
            if (!line[0]) continue;
            char c = line[0]; int arg = atoi(line + 1);
            if (!up && c != 'i' && c != 'z' && c != '?') { printf("controller not up: i to retry\n"); continue; }
            switch (c) {
            case '?': printf("r R h b w k c 4 8 p t<N> v<mV> m<N> e i z<Hz> s\n"); break;
            case 's': status(); break;
            case 'r': paint(PAT_RAMP_V, true); break;
            case 'R': paint(PAT_RAMP_V, false); break;
            case 'h': paint(PAT_RAMP_H, true); break;
            case 'b': paint(PAT_BLACK, true); break;
            case 'w': paint(PAT_WHITE, true); break;
            case 'k': paint(PAT_CHECK, true); break;
            case 'a': paint_area((pat_t)last_pat, 0, H - 125, 704, 125, true); break;
            case 'A': paint_area((pat_t)last_pat, 0, H - 125, 704, 125, false); break;
            case 'n': for (int i = 0; i < (arg > 0 ? arg : 3); i++) { paint_area((pat_t)last_pat, 0, H - 125, 704, 125, true); sleep_ms(2000); } break;
            case 'g': paint((pat_t)last_pat, false); break;
            case 'c': wait_lut(); wcmd(CMD_DPY_AREA); wdat(0); wdat(0); wdat(W); wdat(H); wdat(0); wait_lut(); printf("INIT clear done\n"); break;
            case '4': bpp = 4; printf("4 bpp packed pixels\n"); break;
            case '8': bpp = 8; printf("8 bpp pixels\n"); break;
            case 'p': packed = !packed; printf("%s\n", packed ? "packed write" : "one preamble per word"); break;
            case 't': force_temp = arg; temp_force(force_temp); { uint16_t t[2]; temp_get(t); printf("temperature now: sensor %u, forced %u\n", t[0], t[1]); } break;
            case 'v': if (arg >= 1000 && arg <= 3000) { set_vcom((uint16_t)arg); uint16_t v = get_vcom(); printf("VCOM -%u.%03u V\n", v / 1000, v % 1000); } break;
            case 'm': mode = arg; printf("display mode %d\n", mode); break;
            case 'e': printf("reg 0x0038 was 0x%04x", rreg(0x0038)); wreg(0x0038, 0x0602); printf(", now 0x%04x\n", rreg(0x0038)); break;
            case 'i': up = init(); break;
            case 'z': if (arg >= 1000000) { spi_hz = (uint32_t)arg; up = init(); } break;
            default: printf("? for help\n");
            }
        } else if (n < (int)sizeof line - 1) line[n++] = (char)ch;
    }
}
