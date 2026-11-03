/*
 * it8951.c: IT8951 driver for the Waveshare 10.3" HAT. See it8951.h.
 *
 * Every host->controller exchange is a 16-bit preamble followed by 16-bit
 * words, MSB first, with the HRDY line checked before the preamble and again
 * before the payload (Waveshare EPD_IT8951.c). Pixel data goes as one long
 * "write data" transaction: preamble 0x0000, then the whole frame with CS held
 * low (packed-write mode, I80CPCR = 1), pausing between rows if HRDY drops.
 */
#include "it8951.h"
#include "epd_io.h"

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"

/* I80 command set */
#define CMD_SYS_RUN       0x0001
#define CMD_STANDBY       0x0002
#define CMD_POWER_CTR     0x0038   /* panel power (the TPS65185 rails and VCOM): arg 0 off, 1 on; the controller keeps running */
#define CMD_SLEEP         0x0003
#define CMD_REG_RD        0x0010
#define CMD_REG_WR        0x0011
#define CMD_MEM_BST_RD_T  0x0012   /* burst read trigger: addr lo, addr hi, count lo, count hi (words) */
#define CMD_MEM_BST_RD_S  0x0013   /* then the data */
#define CMD_MEM_BST_END   0x0015
#define CMD_LD_IMG_AREA   0x0021
#define CMD_LD_IMG_END    0x0022
#define CMD_DPY_AREA      0x0034
#define CMD_GET_DEV_INFO  0x0302
#define CMD_VCOM          0x0039
#define CMD_TEMP          0x0040   /* read / force the temperature that picks the waveform */

/* Registers */
#define REG_I80CPCR       0x0004   /* bit 0: packed-write enable              */
#define REG_ENHANCE       0x0038   /* 0x0602 = enhanced driving (Waveshare)   */
#define REG_LISAR         0x0208   /* load-image start address, low word; +2 = high */
#define REG_LUTAFSR       0x1224   /* LUT engine status: non-zero = refreshing */

/* SPI preambles */
#define PRE_CMD   0x6000
#define PRE_DATA  0x0000
#define PRE_READ  0x1000

/* LD_IMG_AREA argument 0: endian << 8 | bpp << 4 | rotate */
#define LDIMG_L_ENDIAN  0
#define LDIMG_2BPP      0
#define LDIMG_4BPP      2
#define LDIMG_8BPP      3
#define LDIMG_ROT0      0
/* Bitmap (1 bit per pixel) mode, as Waveshare's 1bp refresh uses it: the
 * area is loaded as 8bpp at x/8 and w/8 (each byte eight pixels), then bit 2
 * of UP1SR+2 tells the display engine to expand it, BGVR giving the grey for
 * a 1 (high byte) and a 0 (low byte). */
#define REG_UP1SR2      0x113A
#define REG_BGVR        0x1250

static it8951_info_t s_info;
static bool     s_up;                   /* identified and in SYS_RUN           */
static bool     s_asleep;
static uint32_t s_hrdy_timeout = 5000;  /* ms; shortened while probing pins    */
static int32_t  s_vcom_mv = -1;         /* run-time override, -1 = none        */
static bool     s_mirror  = (EPD_MIRROR_H != 0);
static uint32_t s_spi_hz  = EPD_SPI_HZ;
static uint32_t s_paints, s_last_stream, s_last_refresh;

void     it8951_set_mirror(bool m)     { s_mirror = m; }
bool     it8951_mirror(void)           { return s_mirror; }
void     it8951_set_spi_hz(uint32_t hz){ s_spi_hz = hz; }
uint32_t it8951_spi_hz(void)           { return s_spi_hz; }
uint32_t it8951_paint_count(void)      { return s_paints; }
uint32_t it8951_last_stream_ms(void)   { return s_last_stream; }
uint32_t it8951_last_refresh_ms(void)  { return s_last_refresh; }

static inline uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

/* ---- word-level transport ------------------------------------------------ */

static bool s_blind;                    /* ignore HRDY, pace with fixed delays */

static unsigned s_stalls;               /* times HRDY was found low (controller reacted) */

static bool hrdy(void)
{
    if (s_blind) { sleep_us(200); return true; }
    if (!epd_hrdy()) s_stalls++;
    if (epd_wait_hrdy(s_hrdy_timeout)) return true;
    printf("it8951: HRDY timeout (GP%d stayed low %lu ms)\n", EPD_PIN_HRDY,
           (unsigned long)s_hrdy_timeout);
    return false;
}

static void write_cmd(uint16_t cmd)
{
    hrdy(); epd_cs(true);
    epd_spi_write16(PRE_CMD);
    hrdy();
    epd_spi_write16(cmd);
    epd_cs(false);
}

static void write_data(uint16_t d)
{
    hrdy(); epd_cs(true);
    epd_spi_write16(PRE_DATA);
    hrdy();
    epd_spi_write16(d);
    epd_cs(false);
}

static void read_words(uint16_t *buf, size_t n)
{
    hrdy(); epd_cs(true);
    epd_spi_write16(PRE_READ);
    hrdy();
    (void)epd_spi_read16();            /* dummy word */
    hrdy();
    for (size_t i = 0; i < n; i++) buf[i] = epd_spi_read16();
    epd_cs(false);
}

static uint16_t read_word(void)
{
    uint16_t v;
    read_words(&v, 1);
    return v;
}

static void write_cmd_args(uint16_t cmd, const uint16_t *args, int n)
{
    write_cmd(cmd);
    for (int i = 0; i < n; i++) write_data(args[i]);
}

static uint16_t read_reg(uint16_t reg)
{
    write_cmd(CMD_REG_RD);
    write_data(reg);
    return read_word();
}

static void write_reg(uint16_t reg, uint16_t val)
{
    write_cmd(CMD_REG_WR);
    write_data(reg);
    write_data(val);
}

/* ---- identification / init ---------------------------------------------- */

/* GET_DEV_INFO returns 20 words: W, H, addr_L, addr_H, FW[8], LUT[8]. The two
 * strings arrive byte-swapped within each word (low byte first). */
static uint16_t s_raw_info[20];         /* last GET_DEV_INFO words, for diagnostics */

static bool get_dev_info(it8951_info_t *out)
{
    uint16_t *w = s_raw_info;
    write_cmd(CMD_GET_DEV_INFO);
    if (s_blind) sleep_ms(5);
    read_words(w, 20);

    memset(out, 0, sizeof *out);
    out->w = w[0];
    out->h = w[1];
    out->img_addr = (uint32_t)w[2] | ((uint32_t)w[3] << 16);
    for (int i = 0; i < 8; i++) {
        out->fw[2 * i]      = (char)(w[4 + i] & 0xFF);
        out->fw[2 * i + 1]  = (char)(w[4 + i] >> 8);
        out->lut[2 * i]     = (char)(w[12 + i] & 0xFF);
        out->lut[2 * i + 1] = (char)(w[12 + i] >> 8);
    }
    bool clean = out->lut[0] != 0 && out->fw[0] != 0;   /* bit errors garble these first */
    for (int i = 0; i < 16; i++) {       /* keep the log printable */
        if (out->fw[i]  && (out->fw[i]  < 0x20 || out->fw[i]  > 0x7E)) { out->fw[i]  = '?'; clean = false; }
        if (out->lut[i] && (out->lut[i] < 0x20 || out->lut[i] > 0x7E)) { out->lut[i] = '?'; clean = false; }
    }
    return clean && out->w >= 16 && out->w <= 4096 && out->h >= 16 && out->h <= 4096 &&
           out->img_addr != 0 && out->img_addr != 0xFFFFFFFFu;
}

static bool bring_up(uint cs, uint miso, uint32_t hz, int mode)
{
    epd_io_set_mode(mode);
    if (!epd_io_init(cs, miso, hz)) {
        printf("it8951: no free PIO state machine\n");
        return false;
    }
    epd_reset();
    s_hrdy_timeout = 1000;
    /* The IT8951 boots its firmware after reset and can hold HRDY low for
     * ~2 s before it will take commands. */
    uint32_t t0 = now_ms();
    if (!epd_wait_hrdy(4000))
        printf("it8951: HRDY (GP%d) still low 4 s after reset\n", EPD_PIN_HRDY);
    else
        printf("it8951: HRDY high %lu ms after reset\n", (unsigned long)(now_ms() - t0));
    write_cmd(CMD_SYS_RUN);

    it8951_info_t info;
    bool ok = get_dev_info(&info);
    if (ok) {
        /* Bit errors at too high an SPI clock still give plausible numbers
         * (18.75 MHz read 2040x1406 here), so insist on three identical reads. */
        for (int k = 0; k < 2 && ok; k++) {
            it8951_info_t again;
            if (!get_dev_info(&again) || memcmp(&again, &info, sizeof info) != 0) {
                printf("it8951: device info unstable at %lu Hz (%ux%u vs %ux%u) -- SPI too fast for this wiring\n",
                       (unsigned long)hz, info.w, info.h, again.w, again.h);
                ok = false;
            }
        }
    }
    s_hrdy_timeout = 5000;
    if (!ok) {
        printf("it8951: no valid device info with CS=GP%u MISO=GP%u mode %d @%lu Hz: words %04x %04x %04x %04x %04x %04x\n",
               cs, miso, mode, (unsigned long)hz, s_raw_info[0], s_raw_info[1], s_raw_info[2],
               s_raw_info[3], s_raw_info[4], s_raw_info[5]);
        epd_io_deinit();
        return false;
    }
    s_info = info;
    return true;
}

static bool s_ref_stale;              /* something has been shown since the panel was last INIT'd */
/* Rest the panel power between updates. Left in SYS_RUN, the drive rails and
 * VCOM stay on the glass and the image washes out over the following minutes;
 * the E1003 driver (same glass) and Waveshare's demo send STANDBY after every
 * refresh. SYS_RUN in load_begin() powers it back up. */
static bool s_standby = false;      /* off: on this board HRDY stays low after STANDBY and the next command times out (30 Oct) */
static void write_cmd(uint16_t cmd);
/* The panel's drive rails and VCOM must be off between refreshes: left on,
 * there is a constant small voltage across the ink while the pixels are
 * isolated, and the image drifts lighter over minutes to hours (seen on the
 * E1003 with this glass, and the shape of every fade here: good right after a
 * paint, faded after the screen had sat). STANDBY would do it but hangs HRDY
 * on this board; POWER_CTR switches just the panel power and leaves the
 * controller answering. On before each refresh, off once it has finished. */
static bool s_rails_off_between = false;   /* off: the first refresh after power-up looked weak (2 Nov); epdpower=1 to try it */
static bool s_rails_on = true;              /* after init the controller has them on */
static uint32_t s_last_refresh_ms;
static void panel_power(bool on)
{
    if (on == s_rails_on) return;
    write_cmd(CMD_POWER_CTR); write_data(on ? 0x0001 : 0x0000);
    hrdy();                                  /* the controller sequences the PMIC meanwhile */
    if (on) sleep_ms(10);
    s_rails_on = on;
}
/* Switching the rails costs ~0.45 s per refresh, so they stay on while paints
 * keep coming (menu steps) and go off once the panel has been quiet for a
 * few seconds: it8951_idle_tick() from the main loop. */
static void rest_panel(void)
{
    s_last_refresh_ms = now_ms();
    if (s_standby) write_cmd(CMD_STANDBY);
}
static void ready_panel(void) { if (s_rails_off_between) panel_power(true); }
bool it8951_idle_tick(void)
{
    if (!s_rails_off_between || !s_rails_on || !s_up || s_asleep) return false;
    if (now_ms() - s_last_refresh_ms < 4000) return false;
    panel_power(false);
    return true;
}
bool it8951_rails_live(void) { return s_rails_on; }
void it8951_set_standby(bool on) { s_standby = on; printf("it8951: standby between updates %s\n", on ? "on" : "off"); }
void it8951_set_rails_off(bool on) { s_rails_off_between = on; printf("it8951: panel power off when idle %s\n", on ? "on" : "off"); if (!on) panel_power(true); }
bool it8951_rails_off(void) { return s_rails_off_between; }
static void apply_temp(void);
static int32_t s_vcom_cached = -1;

uint16_t it8951_get_vcom(void)
{
    write_cmd(CMD_VCOM);
    write_data(0x0000);
    uint16_t v = read_word();
    s_vcom_cached = v;
    return v;
}

int32_t it8951_cached_vcom(void) { return s_vcom_cached; }

void it8951_set_vcom(uint16_t mv)
{
    s_vcom_mv = mv;
    write_cmd(CMD_VCOM);
    write_data(0x0001);
    write_data(mv);
    uint16_t rb = it8951_get_vcom();
    printf("it8951: VCOM set to -%u.%02uV (readback %u mV)\n", mv / 1000, (mv % 1000) / 10, rb);
}

static void apply_vcom(void)
{
    uint16_t cur = it8951_get_vcom();
    int32_t want = s_vcom_mv;
#ifdef EPD_VCOM_MV
    if (want < 0) want = EPD_VCOM_MV;
#endif
    if (want < 0) {
        printf("it8951: VCOM = -%u.%02uV (controller's own value)\n", cur / 1000, (cur % 1000) / 10);
        printf("it8951: *** EPD_VCOM_MV is not set. Read the voltage printed on the panel's FPC\n"
               "it8951: *** cable and build with -DEPD_VCOM_MV=<mV> (-1.50V -> 1500), or type\n"
               "it8951: *** v1500<Enter> here. Running with the wrong VCOM degrades the panel. ***\n");
        return;
    }
    if (cur != (uint16_t)want) it8951_set_vcom((uint16_t)want);
    else printf("it8951: VCOM = -%u.%02uV\n", cur / 1000, (cur % 1000) / 10);
}

bool it8951_init(void)
{
    /* Try the configured pins at full speed in mode 0 first, then slower, then
     * mode 3, then the CS/MISO pins swapped. */
    struct { uint8_t cs, miso; uint32_t hz; int mode; } cand[] = {
        { EPD_PIN_CS, EPD_PIN_MISO, s_spi_hz,   0 },
        { EPD_PIN_CS, EPD_PIN_MISO, 9375000,    0 },
        { EPD_PIN_CS, EPD_PIN_MISO, 1000000,    0 },
        { EPD_PIN_CS, EPD_PIN_MISO, 1000000,    3 },
        { EPD_PIN_MISO, EPD_PIN_CS, 1000000,    0 },
        { EPD_PIN_MISO, EPD_PIN_CS, 1000000,    3 },
    };
    int n = (EPD_PIN_PROBE && EPD_PIN_CS != EPD_PIN_MISO) ? 6 : 4;

    s_up = false;
    s_asleep = false;
    for (int i = 0; i < n && !s_up; i++) {
        printf("it8951: trying CS=GP%u MISO=GP%u mode %d @%lu Hz\n", cand[i].cs, cand[i].miso,
               cand[i].mode, (unsigned long)cand[i].hz);
        s_up = bring_up(cand[i].cs, cand[i].miso, cand[i].hz, cand[i].mode);
    }
    if (!s_up) {
        printf("it8951: controller not found. Check 5V to the HAT, the FPC, RST on GP%d and HRDY on GP%d,\n"
               "it8951: and that SCLK/MOSI really are GP%d/GP%d on your adapter (see epd_io.h).\n",
               EPD_PIN_RST, EPD_PIN_HRDY, EPD_PIN_SCLK, EPD_PIN_MOSI);
        return false;
    }

    printf("it8951: found on CS=GP%u MISO=GP%u (mode %d): panel %ux%u, image buffer 0x%08lx, FW \"%s\", LUT \"%s\"\n",
           epd_io_cs_pin(), epd_io_miso_pin(), epd_io_mode(), s_info.w, s_info.h,
           (unsigned long)s_info.img_addr, s_info.fw, s_info.lut);
    if (s_info.w != IT8951_W || s_info.h != IT8951_H)
        printf("it8951: WARNING: this build is laid out for %dx%d; the panel reports %ux%u\n",
               IT8951_W, IT8951_H, s_info.w, s_info.h);

    write_reg(REG_I80CPCR, 0x0001);      /* packed (bulk) data writes */
    apply_vcom();
    apply_temp();
    s_ref_stale = true;                  /* the controller's DRAM may still hold an old image */
    return true;
}

const it8951_info_t *it8951_info(void)
{
    return &s_info;
}

void it8951_probe_matrix(void)
{
    static const uint8_t cs_c[]   = { 9, 15, 8, 12 };
    static const uint8_t miso_c[] = { 8, 12, 9, 15, 0, 28 };
    printf("it8951: probe matrix using HRDY=GP%d (300 ms waits), RST=GP%d\n", EPD_PIN_HRDY, EPD_PIN_RST);
    int found = 0;
    for (int swap = 0; swap < 2 && !found; swap++)
    for (int m = 0; m < 2 && !found; m++)
    for (unsigned i = 0; i < sizeof cs_c && !found; i++)
    for (unsigned j = 0; j < sizeof miso_c && !found; j++) {
        if (cs_c[i] == miso_c[j]) continue;
        int mode = m ? 3 : 0;
        epd_io_set_swap(swap != 0);
        epd_io_set_mode(mode);
        if (!epd_io_init(cs_c[i], miso_c[j], 1000000)) continue;
        epd_reset();
        epd_wait_hrdy(2000);
        s_hrdy_timeout = 300;
        s_stalls = 0;
        write_cmd(CMD_SYS_RUN);
        it8951_info_t info;
        bool ok = get_dev_info(&info);
        unsigned stalls = s_stalls;
        bool quiet = true;
        for (int k = 0; k < 6; k++) if (s_raw_info[k] != 0x0000 && s_raw_info[k] != 0xFFFF) quiet = false;
        printf("it8951:   %s SCK=GP%u MOSI=GP%u CS=GP%u MISO=GP%u mode %d -> HRDY dips %u, words %04x %04x %04x %04x %04x %04x\n",
               ok ? "VALID" : quiet ? "quiet" : "NOISE",
               swap ? EPD_PIN_MOSI : EPD_PIN_SCLK, swap ? EPD_PIN_SCLK : EPD_PIN_MOSI,
               cs_c[i], miso_c[j], mode, stalls,
               s_raw_info[0], s_raw_info[1], s_raw_info[2], s_raw_info[3], s_raw_info[4], s_raw_info[5]);
        if (ok) {
            printf("it8951:   >>> panel %ux%u addr 0x%08lx FW \"%s\" LUT \"%s\"  -- build with -DEPD_PIN_CS=%u -DEPD_PIN_MISO=%u%s%s\n",
                   info.w, info.h, (unsigned long)info.img_addr, info.fw, info.lut, cs_c[i], miso_c[j],
                   swap ? " (and swap SCLK/MOSI)" : "", mode ? " (mode 3)" : "");
            found++;
        }
        s_hrdy_timeout = 5000;
        epd_io_deinit();
    }
    epd_io_set_swap(false);
    epd_io_set_mode(0);
    if (!found) printf("it8951:   no pairing produced device info\n");
}

bool it8951_probe_blind(unsigned cs, unsigned miso, int mode, it8951_info_t *out, uint16_t raw[4])
{
    epd_io_set_mode(mode);
    if (!epd_io_init(cs, miso, 1000000)) return false;   /* slow, forgiving */
    s_blind = true;
    epd_reset();
    sleep_ms(2500);                      /* IT8951 firmware boot */
    write_cmd(CMD_SYS_RUN);
    sleep_ms(20);
    bool ok = get_dev_info(out);
    s_blind = false;
    for (int i = 0; i < 4; i++) raw[i] = s_raw_info[i];
    epd_io_deinit();
    return ok;
}

/* The temperature the controller would sense, and the one we force. E-ink
 * waveforms depend on temperature; the E1003 firmware (same glass) and
 * FastEPD force 14 C, as too warm a LUT is short and under-drives the glass.
 * 0 = let the controller sense it. Forced here: the controller's sensor is
 * on the HAT, not the glass, and after a warm day of refreshes it picked a
 * waveform too warm for 20 C glass and the panel faded (1 Nov 2026; 20
 * looked best, 24 close, 30 worse). /api/cmd?temp=N to try others. */
static int s_force_temp_c = 20;
static int s_sensed_temp_c = -1, s_forced_rb = -1;
static bool s_reinit_pending;

/* Command 0x0040: arg 0 = get, which answers TWO words (the sensor's reading,
 * then the last forced value); arg 1 = force. Every driver for this glass
 * (Seeed, FrameOS, Zephyr) re-sends the force before each display, since it
 * lives in the controller's RAM and the controller can miss it; so does this one. */
static void read_temp(void)
{
    uint16_t t[2] = { 0xFFFF, 0xFFFF };
    write_cmd(CMD_TEMP); write_data(0x0000);
    sleep_ms(5);                                         /* the sensor read is slow; HRDY alone has not been enough */
    read_words(t, 2);
    s_sensed_temp_c = t[0] <= 85 ? (int)(int16_t)t[0] : -1;
    s_forced_rb     = t[1] <= 85 ? (int)(int16_t)t[1] : -1;
}
static void force_temp(void)
{
    if (s_force_temp_c <= 0) return;
    write_cmd(CMD_TEMP); write_data(0x0001); write_data((uint16_t)s_force_temp_c);
}
static void apply_temp(void)
{
    read_temp();
    int before = s_sensed_temp_c;
    force_temp();
    read_temp();
    if (s_sensed_temp_c >= 0 || s_forced_rb >= 0)
        printf("it8951: temperature: sensor %d C (board, not the glass), forced %d C, controller reports forced %d C\n",
               before, s_force_temp_c, s_forced_rb);
    else printf("it8951: temperature: no readback from this firmware; waveform forced to %d C\n", s_force_temp_c);
}
void it8951_set_temp(int c)
{
    s_force_temp_c = c < 0 ? 0 : c > 50 ? 50 : c;
    if (s_up && !s_asleep) apply_temp();
}
int  it8951_temp_forced_rb(void) { return s_forced_rb; }
bool it8951_reinit_pending(void) { bool p = s_reinit_pending; s_reinit_pending = false; return p; }
int it8951_temp_forced(void) { return s_force_temp_c; }
int it8951_temp_sensed(void) { return s_sensed_temp_c; }
/* Read the board sensor again (the caller holds the painter); -1 when it can't. */
int it8951_sense_temp(void)
{
    if (!s_up || s_asleep) return -1;
    read_temp();
    return s_sensed_temp_c;
}

/* Before a paint: the controller must be up, and still holding its settings.
 * A controller that reset itself (brown-out, a glitch) comes back with its
 * own VCOM and no forced temperature, and every paint after that is faded
 * until a reboot. Two disagreeing VCOM reads in a row means a re-init, and
 * main.c then repaints the whole screen. */
static bool ensure_up(void)
{
    if (!(s_up && !s_asleep)) {
        printf("it8951: controller %s, re-initialising\n", s_asleep ? "asleep" : "not up");
        return it8951_init();
    }
    int32_t want = s_vcom_mv;
#ifdef EPD_VCOM_MV
    if (want < 0) want = EPD_VCOM_MV;
#endif
    if (want > 0) {
        uint16_t v = it8951_get_vcom();
        if (v != (uint16_t)want && it8951_get_vcom() != (uint16_t)want) {
            printf("it8951: VCOM reads %u mV, expected %ld: the controller lost its settings, re-initialising\n", v, (long)want);
            s_reinit_pending = true;
            return it8951_init();
        }
    }
    return true;
}

/* ---- image load + refresh ------------------------------------------------ */

static void wait_display_ready(uint32_t timeout_ms)
{
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (read_reg(REG_LUTAFSR) != 0) {
        if (time_reached(deadline)) {
            printf("it8951: LUT engine still busy after %lu ms\n", (unsigned long)timeout_ms);
            return;
        }
        sleep_ms(10);
    }
}

static inline uint8_t swapnib(uint8_t b) { return (uint8_t)((b << 4) | (b >> 4)); }

/* Logical packed span (nbytes, even) -> the byte order the IT8951 sees.
 * Waveshare's 10.3" path (a) mirrors the row (Paint MIRROR_HORIZONTAL,
 * "epd_mode 1") and (b) sends each 16-bit word high byte first from a
 * little-endian buffer, i.e. swaps byte pairs. Mirroring a packed span =
 * reverse the bytes and swap the nibbles in each. */
static void row_to_wire(const uint8_t *row, int nbytes, uint8_t *wire)
{
    if (s_mirror) {
        for (int k = 0; k < nbytes; k += 2) {
            wire[k]     = swapnib(row[nbytes - 2 - k]);
            wire[k + 1] = swapnib(row[nbytes - 1 - k]);
        }
    } else {
        for (int k = 0; k < nbytes; k += 2) {
            wire[k]     = row[k + 1];
            wire[k + 1] = row[k];
        }
    }
}

/* Open a load into the image buffer for panel-space rectangle (px, y, w, h)
 * in pixel format fmt (LDIMG_*BPP) and start the single long "write data"
 * transaction. */
static void load_begin(int px, int y, int w, int h, int fmt)
{
    write_cmd(CMD_SYS_RUN);              /* power the panel back up from standby */
    wait_display_ready(30000);
    write_reg(REG_LISAR + 2, (uint16_t)(s_info.img_addr >> 16));
    write_reg(REG_LISAR,     (uint16_t)(s_info.img_addr & 0xFFFF));
    const uint16_t args[5] = {
        (uint16_t)((LDIMG_L_ENDIAN << 8) | (fmt << 4) | LDIMG_ROT0),
        (uint16_t)px, (uint16_t)y, (uint16_t)w, (uint16_t)h,
    };
    write_cmd_args(CMD_LD_IMG_AREA, args, 5);

    hrdy(); epd_cs(true);
    epd_spi_write16(PRE_DATA);
    hrdy();
}

/* One bit per pixel, for the two-level loads (DU, A2) when bitmap mode is on:
 * four times fewer bytes than 4bpp. s_bitmap_inv swaps which bit is ink, in
 * case the controller reads BGVR the other way round. */
static bool s_bitmap, s_bitmap_inv, s_job_bitmap;
void it8951_set_bitmap(int mode) { s_bitmap = mode != 0; s_bitmap_inv = mode == 2; }
int  it8951_bitmap(void)         { return s_bitmap ? (s_bitmap_inv ? 2 : 1) : 0; }
static uint8_t bitrev8(uint8_t b) { b = (uint8_t)((b & 0xF0) >> 4 | (b & 0x0F) << 4); b = (uint8_t)((b & 0xCC) >> 2 | (b & 0x33) << 2); return (uint8_t)((b & 0xAA) >> 1 | (b & 0x55) << 1); }
/* row: packed 4bpp pixels from logical x0, lw of them; out: lw/8 bytes, bit 7 the leftmost pixel, 1 = ink. */
static void row_to_bits(const uint8_t *row, int lw, uint8_t *out)
{
    int nb = lw / 8;
    for (int k = 0; k < nb; k++) {
        uint8_t b = 0;
        for (int i = 0; i < 4; i++) {
            uint8_t p = row[k * 4 + i];
            if ((p >> 4) < 8)   b |= (uint8_t)(0x80 >> (2 * i));
            if ((p & 0x0F) < 8) b |= (uint8_t)(0x40 >> (2 * i));
        }
        if (s_bitmap_inv) b = (uint8_t)~b;
        out[k] = b;
    }
    if (s_mirror) {                      /* the panel reads the row backwards: bytes and bits */
        for (int k = 0; k < nb / 2; k++) { uint8_t t = out[k]; out[k] = out[nb - 1 - k]; out[nb - 1 - k] = t; }
        for (int k = 0; k < nb; k++) out[k] = bitrev8(out[k]);
    }
    for (int k = 0; k + 1 < nb; k += 2) { uint8_t t = out[k]; out[k] = out[k + 1]; out[k + 1] = t; }   /* the word's low byte first, as 4bpp */
}

static void display(int px, int y, int w, int h, uint8_t mode);
/* The INIT pass before a full GC16 only needs the image buffer white: load
 * it at 2 bits per pixel, half the bytes of the frame it clears for. */
static void init_white(void)
{
    static uint8_t ff[IT8951_W / 4];
    memset(ff, 0xFF, sizeof ff);
    uint32_t t0 = now_ms();
    load_begin(0, 0, IT8951_W, IT8951_H, LDIMG_2BPP);
    for (int y = 0; y < IT8951_H; y++) {
        epd_spi_write_wait();
        if (!epd_hrdy()) hrdy();
        epd_spi_write_async(ff, sizeof ff);
    }
    epd_spi_write_wait();
    epd_cs(false);
    write_cmd(CMD_LD_IMG_END);
    uint32_t t1 = now_ms();
    display(0, 0, IT8951_W, IT8951_H, IT8951_MODE_INIT);
    s_ref_stale = false;
    printf("it8951: white at 2bpp in %lu ms, INIT in %lu ms\n", (unsigned long)(t1 - t0), (unsigned long)(now_ms() - t1));
}

/* Two wire buffers: row N goes out by DMA while row N+1 is being built. */
static uint8_t s_wire[2][IT8951_ROW_BYTES];

static void load_row(const uint8_t *span, int nbytes, int slot)
{
    uint8_t *wire = s_wire[slot & 1];
    row_to_wire(span, nbytes, wire);
    epd_spi_write_wait();                /* previous row fully clocked out */
    if (!epd_hrdy()) hrdy();             /* controller is pacing us */
    epd_spi_write_async(wire, (size_t)nbytes);
}

static void load_end(void)
{
    epd_spi_write_wait();
    epd_cs(false);
    write_cmd(CMD_LD_IMG_END);
}

/* Around a display of areas loaded one bit per pixel: tell the engine, and the greys for 1 and 0. */
static void bitmap_on(void)  { write_reg(REG_UP1SR2, (uint16_t)(read_reg(REG_UP1SR2) | 0x0004)); write_reg(REG_BGVR, 0x00F0); }
static void bitmap_off(void) { write_reg(REG_UP1SR2, (uint16_t)(read_reg(REG_UP1SR2) & ~0x0004)); }

static void display(int px, int y, int w, int h, uint8_t mode)
{
    const uint16_t args[5] = { (uint16_t)px, (uint16_t)y, (uint16_t)w, (uint16_t)h, mode };
    force_temp();                        /* the waveform is picked at display time */
    ready_panel();
    if (s_job_bitmap) bitmap_on();
    write_cmd_args(CMD_DPY_AREA, args, 5);
    sleep_ms(20);                        /* let the LUT engine pick the job up */
    wait_display_ready(30000);
    if (s_job_bitmap) { bitmap_off(); s_job_bitmap = false; }
    rest_panel();
}

/* Stream rows y0..y1-1, logical columns x0..x0+w-1 (x0 and w multiples of 16),
 * then refresh that rectangle with `mode`. */
/* Stream rows y..y+h-1, logical columns x..x+w-1 (widened to 16 px) into the
 * controller's image buffer, without refreshing. Returns the aligned
 * rectangle in logical coordinates through *ax..*ah. */
/* The columns being streamed right now, so a row generator can skip work
 * outside them (a narrow area costs rows, not width, otherwise). */
static int s_win_x0 = 0, s_win_x1 = IT8951_W;
/* DU and A2 can only show black or white, but the controller records the
 * values it was given as the image on the glass, and every later paint
 * drives from that record. Loading greys for a DU refresh leaves the record
 * wrong (a black pixel on record as dark grey is under-driven next time: the
 * panel fades). So those loads carry the pixels as the glass will show them. */
static bool s_load_bw;
static inline uint8_t bw_byte(uint8_t b) { return (uint8_t)(((b & 0xF0) >= 0x80 ? 0xF0 : 0x00) | ((b & 0x0F) >= 0x08 ? 0x0F : 0x00)); }
void it8951_stream_window(int *x0, int *x1) { *x0 = s_win_x0; *x1 = s_win_x1; }

static bool load_area(int x, int y, int w, int h, it8951_row_fn fn, void *ctx, int *ax, int *ay, int *aw, int *ah)
{
    if (!ensure_up()) return false;
    int x0 = x & ~15, x1 = (x + w + 15) & ~15;
    if (x0 < 0) x0 = 0;
    if (x1 > IT8951_W) x1 = IT8951_W;
    int y0 = y < 0 ? 0 : y, y1 = (y + h > IT8951_H) ? IT8951_H : y + h;
    if (x1 <= x0 || y1 <= y0) return false;
    const int lw = x1 - x0, lh = y1 - y0;
    const int px = s_mirror ? (IT8951_W - x0 - lw) : x0;        /* panel-space x */
    uint8_t row[IT8951_ROW_BYTES];
    static uint8_t bits[2][IT8951_W / 8];
    bool onebit = s_load_bw && s_bitmap;
    s_win_x0 = x0; s_win_x1 = x1;
    if (onebit) { load_begin(px / 8, y0, lw / 8, lh, LDIMG_8BPP); s_job_bitmap = true; }
    else load_begin(px, y0, lw, lh, LDIMG_4BPP);
    for (int yy = y0; yy < y1; yy++) {
        fn(yy, row, ctx);
        if (onebit) {
            uint8_t *b = bits[yy & 1];
            row_to_bits(row + x0 / 2, lw, b);
            epd_spi_write_wait();
            if (!epd_hrdy()) hrdy();
            epd_spi_write_async(b, (size_t)(lw / 8));
            continue;
        }
        if (s_load_bw) for (int k = x0 / 2; k < x1 / 2; k++) row[k] = bw_byte(row[k]);
        load_row(row + x0 / 2, lw / 2, yy);
    }
    load_end();
    s_win_x0 = 0; s_win_x1 = IT8951_W;
    *ax = x0; *ay = y0; *aw = lw; *ah = lh;
    return true;
}

/* SPI link check: write `rows` rows of a known pattern into the controller's
 * image buffer (no refresh), read them back with a memory burst read, and
 * count the mismatches. Writes are otherwise never verified; this is how a
 * faster SPI clock is proven safe without eyes on the glass. */
static void test_row(int y, uint8_t out[IT8951_ROW_BYTES], void *ctx)
{
    uint32_t seed = (uint32_t)(uintptr_t)ctx + (uint32_t)y * 2654435761u;
    for (int i = 0; i < IT8951_ROW_BYTES; i++) { seed = seed * 1103515245u + 12345u; out[i] = (uint8_t)(seed >> 16); }
}
int it8951_spi_test(int rows)
{
    if (!ensure_up()) return -1;
    if (rows < 1) rows = 1;
    if (rows > 4) rows = 4;
    /* The controller keeps one BYTE per pixel (a 4-bit load is expanded, value
     * << 4), so a row is IT8951_W bytes in its memory and a burst read returns
     * two pixels per word: (pixel0 << 8) | pixel1. */
    const int y0 = 0;
    uint32_t seed = now_ms() | 1u;
    int ax, ay, aw, ah;
    if (!load_area(0, y0, IT8951_W, rows, test_row, (void *)(uintptr_t)seed, &ax, &ay, &aw, &ah)) return -1;
    wait_display_ready(5000);
    const uint32_t addr = s_info.img_addr + (uint32_t)y0 * IT8951_W;
    const uint32_t cnt = (uint32_t)rows * IT8951_W / 2;              /* words */
    static uint16_t got[4 * IT8951_W / 2];
    const uint16_t args[4] = { (uint16_t)(addr & 0xFFFF), (uint16_t)(addr >> 16), (uint16_t)(cnt & 0xFFFF), (uint16_t)(cnt >> 16) };
    write_cmd_args(CMD_MEM_BST_RD_T, args, 4);
    write_cmd(CMD_MEM_BST_RD_S);
    read_words(got, cnt);
    write_cmd(CMD_MEM_BST_END);
    s_ref_stale = true;
    uint8_t row[IT8951_ROW_BYTES];
    int bad = 0, first_bad = -1;
    uint16_t fb_got = 0, fb_exp = 0;
    for (int r = 0; r < rows; r++) {
        test_row(y0 + r, row, (void *)(uintptr_t)seed);
        for (int x = 0; x < IT8951_W; x += 2) {
            /* pixel x is the high nibble of row[x/2] (draw.h), x+1 the low; mirrored panels reverse the row */
            int lx0 = s_mirror ? IT8951_W - 1 - x : x, lx1 = s_mirror ? IT8951_W - 2 - x : x + 1;
            uint8_t p0 = (lx0 & 1) ? (row[lx0 >> 1] & 0x0F) : (row[lx0 >> 1] >> 4);
            uint8_t p1 = (lx1 & 1) ? (row[lx1 >> 1] & 0x0F) : (row[lx1 >> 1] >> 4);
            uint16_t exp = (uint16_t)((p0 << 12) | (p1 << 4));
            uint16_t g = got[r * IT8951_W / 2 + x / 2];
            if (g != exp) { if (first_bad < 0) { first_bad = r * IT8951_W + x; fb_got = g; fb_exp = exp; } bad++; }
        }
    }
    printf("it8951: SPI write check at %lu Hz: %d of %lu pixel pairs wrong", (unsigned long)it8951_spi_hz(), bad, (unsigned long)cnt);
    if (bad) printf(" (first at pixel %d: got %04x, expected %04x)", first_bad, fb_got, fb_exp);
    printf("%s\n", bad == 0 ? " (clean)" : "");
    return bad;
}

/* GC16 works out every pixel's drive from the image the controller holds as
 * "what is on the glass". On this panel a full GC16 after any earlier
 * refresh under-drives what the old image had dark, and the whole screen
 * comes out faded (the E1003 firmware found the same: dmellok/tesserae#274).
 * INIT drives the panel to white whatever the reference, so a full GC16
 * that follows it starts true. */
static bool s_clean_full = true;     /* INIT before a full GC16 once anything has been shown: without it, traces of the old screen showed as faint vertical lines (/api/cmd?cleanfull=0 to turn off) */
void it8951_set_clean_full(bool on) { s_clean_full = on; }
bool it8951_clean_full(void) { return s_clean_full; }

static void paint(int x, int y, int w, int h, it8951_row_fn fn, void *ctx, uint8_t mode)
{
    int ax, ay, aw, ah;
    s_load_bw = (mode == IT8951_MODE_DU || mode == IT8951_MODE_A2);
    if (s_clean_full && mode == IT8951_MODE_GC16 && s_ref_stale && x <= 0 && y <= 0 && w >= IT8951_W && h >= IT8951_H) {
        printf("it8951: INIT first: the controller's reference is stale\n");
        if (!ensure_up()) return;
        init_white();
    }
    uint32_t t0 = now_ms();
    if (!load_area(x, y, w, h, fn, ctx, &ax, &ay, &aw, &ah)) return;
    uint32_t t1 = now_ms();
    const int px = s_mirror ? (IT8951_W - ax - aw) : ax;
    display(px, ay, aw, ah, mode);
    s_ref_stale = mode != IT8951_MODE_INIT;
    uint32_t t2 = now_ms();
    s_last_stream = t1 - t0; s_last_refresh = t2 - t1; s_paints++;
    printf("it8951: %dx%d@%d,%d streamed in %lu ms, refreshed (mode %u) in %lu ms\n",
           aw, ah, ax, ay, (unsigned long)(t1 - t0), mode, (unsigned long)(t2 - t1));
}

/* Several rectangles, refreshed in parallel: load each into the image buffer,
 * then start one display job per rectangle back to back (the IT8951 runs
 * several LUT engines at once) and wait for them all. Each job covers only
 * its own rectangle: refreshing pixels that did not change with GL16 greys
 * them, so the rectangles are never merged into a bounding box. */
void it8951_update_areas(const int (*rects)[4], int n, it8951_row_fn fn, void *ctx, uint8_t mode)
{
    int a[24][4], loaded = 0;
    uint32_t t0 = now_ms();
    s_load_bw = (mode == IT8951_MODE_DU || mode == IT8951_MODE_A2);
    for (int i = 0; i < n && loaded < 24; i++) {
        int ax, ay, aw, ah;
        if (!load_area(rects[i][0], rects[i][1], rects[i][2], rects[i][3], fn, ctx, &ax, &ay, &aw, &ah)) continue;
        a[loaded][0] = ax; a[loaded][1] = ay; a[loaded][2] = aw; a[loaded][3] = ah; loaded++;
    }
    s_load_bw = false;
    if (!loaded) return;
    uint32_t t1 = now_ms();
    force_temp();
    /* One refresh over the rectangles' bounding box. The controller queues
     * DPY_AREAs and runs the waveform once per area, so N areas cost N
     * refreshes (4 areas: 2.8 s); a partial update only drives pixels whose
     * value changed, so one box over the loaded areas gives the same picture
     * in one run. */
    int x0 = IT8951_W, y0 = IT8951_H, x1 = 0, y1 = 0;
    for (int i = 0; i < loaded; i++) {
        if (a[i][0] < x0) x0 = a[i][0];
        if (a[i][1] < y0) y0 = a[i][1];
        if (a[i][0] + a[i][2] > x1) x1 = a[i][0] + a[i][2];
        if (a[i][1] + a[i][3] > y1) y1 = a[i][1] + a[i][3];
    }
    ready_panel();
    if (s_job_bitmap) bitmap_on();
    {
        const int px = s_mirror ? (IT8951_W - x1) : x0;
        const uint16_t args[5] = { (uint16_t)px, (uint16_t)y0, (uint16_t)(x1 - x0), (uint16_t)(y1 - y0), mode };
        write_cmd_args(CMD_DPY_AREA, args, 5);
    }
    s_ref_stale = true;
    sleep_ms(20);
    wait_display_ready(30000);
    if (s_job_bitmap) { bitmap_off(); s_job_bitmap = false; }
    rest_panel();
    uint32_t t2 = now_ms();
    s_last_stream = t1 - t0; s_last_refresh = t2 - t1; s_paints++;
    printf("it8951: %d area(s) in parallel: streamed in %lu ms, refreshed (mode %u) in %lu ms\n",
           loaded, (unsigned long)(t1 - t0), mode, (unsigned long)(t2 - t1));
}

void it8951_redisplay_areas(const int (*rects)[4], int n, uint8_t mode)
{
    if (!ensure_up() || n <= 0) return;
    uint32_t t0 = now_ms();
    wait_display_ready(30000);
    ready_panel();
    for (int i = 0; i < n && i < 24; i++) {
        int x0 = rects[i][0] & ~15, x1 = (rects[i][0] + rects[i][2] + 15) & ~15;
        if (x0 < 0) x0 = 0;
        if (x1 > IT8951_W) x1 = IT8951_W;
        int y0 = rects[i][1] < 0 ? 0 : rects[i][1], y1 = rects[i][1] + rects[i][3] > IT8951_H ? IT8951_H : rects[i][1] + rects[i][3];
        if (x1 <= x0 || y1 <= y0) continue;
        const int px = s_mirror ? (IT8951_W - x1) : x0;
        const uint16_t args[5] = { (uint16_t)px, (uint16_t)y0, (uint16_t)(x1 - x0), (uint16_t)(y1 - y0), mode };
        write_cmd_args(CMD_DPY_AREA, args, 5);
    }
    sleep_ms(20);
    wait_display_ready(30000);
    rest_panel();
    s_paints++;
    printf("it8951: second pass over %d area(s) (mode %u) in %lu ms\n", n, mode, (unsigned long)(now_ms() - t0));
}

void it8951_show_pattern(it8951_row_fn fn, void *ctx, uint8_t mode)
{
    paint(0, 0, IT8951_W, IT8951_H, fn, ctx, mode);
}

void it8951_update_area(int x, int y, int w, int h, it8951_row_fn fn, void *ctx, uint8_t mode)
{
    paint(x, y, w, h, fn, ctx, mode);
}

static void frame_row(int y, uint8_t out[IT8951_ROW_BYTES], void *ctx)
{
    memcpy(out, (const uint8_t *)ctx + (size_t)y * IT8951_ROW_BYTES, IT8951_ROW_BYTES);
}

void it8951_show_frame(const uint8_t *frame, uint8_t mode)
{
    it8951_show_pattern(frame_row, (void *)frame, mode);
}

static void white_row(int y, uint8_t out[IT8951_ROW_BYTES], void *ctx)
{
    (void)y; (void)ctx;
    memset(out, 0xFF, IT8951_ROW_BYTES);
}

void it8951_clear(uint8_t mode)
{
    it8951_show_pattern(white_row, NULL, mode);
}

void it8951_sleep(void)
{
    if (!s_up) return;
    write_cmd(CMD_SLEEP);
    s_asleep = true;
    printf("it8951: sleeping (next paint re-inits)\n");
}

void it8951_enhance_driving(void)
{
    if (!ensure_up()) return;
    uint16_t before = read_reg(REG_ENHANCE);
    write_reg(REG_ENHANCE, 0x0602);
    printf("it8951: enhanced driving: reg 0x0038 0x%04x -> 0x%04x\n", before, read_reg(REG_ENHANCE));
}
