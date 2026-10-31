/*
 * gesture.c: APDS-9960 gestures over I2C, polled. See gesture.h.
 * Register settings follow SparkFun's APDS-9960 library (gesture defaults).
 */
#include "gesture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"

#define BUS        i2c0                 /* the Qw/ST chain, set up by seesaw.c */
#define ADDR       0x39
#define TIMEOUT_US 5000

#define R_ENABLE   0x80
#define R_WTIME    0x83
#define R_PPULSE   0x8E
#define R_CONTROL  0x8F
#define R_CONFIG2  0x90
#define R_ID       0x92
#define R_GPENTH   0xA0
#define R_GEXTH    0xA1
#define R_GCONF1   0xA2
#define R_GCONF2   0xA3
#define R_GOFFSET_U 0xA4
#define R_GOFFSET_D 0xA5
#define R_GPULSE   0xA6
#define R_GOFFSET_L 0xA7
#define R_GOFFSET_R 0xA9
#define R_GCONF3   0xAA
#define R_GCONF4   0xAB
#define R_GFLVL    0xAE
#define R_GSTATUS  0xAF
#define R_GFIFO_U  0xFC

#define MAX_FRAMES 32
#define OUT_THRESH 10                   /* a frame counts when every diode reads above this */
#define SENS       50                   /* how far the balance must move (percent points) */

static bool     s_present, s_on;
static uint8_t  s_f[MAX_FRAMES][4];     /* U, D, L, R */
static int      s_nf;
static bool     s_active;
static uint32_t s_next_ms, s_idle_since;

static bool wr(uint8_t reg, uint8_t v)
{
    uint8_t b[2] = { reg, v };
    return i2c_write_timeout_us(BUS, ADDR, b, 2, false, TIMEOUT_US) == 2;
}
static bool rd(uint8_t reg, uint8_t *out, size_t n)
{
    if (i2c_write_timeout_us(BUS, ADDR, &reg, 1, true, TIMEOUT_US) != 1) return false;
    return i2c_read_timeout_us(BUS, ADDR, out, n, false, TIMEOUT_US * 4) == (int)n;
}

bool gesture_init(void)
{
    uint8_t id = 0;
    if (!rd(R_ID, &id, 1) || (id != 0xAB && id != 0x9C && id != 0xA8)) {
        printf("gesture: no APDS-9960 at 0x%02x\n", ADDR);
        return s_present = false;
    }
    bool ok = wr(R_ENABLE, 0x00)
        && wr(R_WTIME, 0xFF)            /* 2.78 ms wait between cycles */
        && wr(R_PPULSE, 0x89)           /* proximity: 16 us, 10 pulses */
        && wr(R_CONTROL, 0x09)          /* LED 100 mA, proximity gain 4x, ALS gain 4x */
        && wr(R_CONFIG2, 0x31)          /* LED boost 300 % */
        && wr(R_GPENTH, 40)             /* enter gesture mode when a hand is this near */
        && wr(R_GEXTH, 30)              /* and leave below this */
        && wr(R_GCONF1, 0x40)           /* FIFO threshold: 4 frames */
        && wr(R_GCONF2, 0x41)           /* gesture gain 4x, LED 100 mA, 2.8 ms between frames */
        && wr(R_GOFFSET_U, 0) && wr(R_GOFFSET_D, 0) && wr(R_GOFFSET_L, 0) && wr(R_GOFFSET_R, 0)
        && wr(R_GPULSE, 0xC9)           /* gesture: 32 us, 10 pulses */
        && wr(R_GCONF3, 0x00)           /* all four diodes */
        && wr(R_GCONF4, 0x00);
    printf("gesture: APDS-9960 (id 0x%02x)%s\n", id, ok ? "" : ", setup failed");
    s_present = ok;
    return ok;
}

bool gesture_present(void) { return s_present; }

void gesture_enable(bool on)
{
    if (!s_present || on == s_on) return;
    s_on = on;
    s_nf = 0; s_active = false;
    /* On: power, proximity (it arms the gesture engine), wait timer, gestures. */
    if (on) { wr(R_GCONF4, 0x01); wr(R_ENABLE, 0x4D); }
    else    { wr(R_ENABLE, 0x00); }
    printf("gesture: %s\n", on ? "on" : "off");
}

/* The direction from the first and last frames where the hand covered all four diodes. */
static gesture_t decode(void)
{
    int first = -1, last = -1;
    for (int i = 0; i < s_nf; i++) {
        const uint8_t *f = s_f[i];
        if (f[0] > OUT_THRESH && f[1] > OUT_THRESH && f[2] > OUT_THRESH && f[3] > OUT_THRESH) {
            if (first < 0) first = i;
            last = i;
        }
    }
    if (first < 0) return GEST_NONE;
    if (first == last) return GEST_WAVE;
    const uint8_t *a = s_f[first], *b = s_f[last];
    int ud0 = ((a[0] - a[1]) * 100) / (a[0] + a[1]), lr0 = ((a[2] - a[3]) * 100) / (a[2] + a[3]);
    int ud1 = ((b[0] - b[1]) * 100) / (b[0] + b[1]), lr1 = ((b[2] - b[3]) * 100) / (b[2] + b[3]);
    int dud = ud1 - ud0, dlr = lr1 - lr0;
    if (abs(dud) < SENS && abs(dlr) < SENS) return GEST_WAVE;     /* in and out, no clear direction */
    if (abs(dud) >= abs(dlr)) return dud > 0 ? GEST_DOWN : GEST_UP;
    return dlr > 0 ? GEST_RIGHT : GEST_LEFT;
}

gesture_t gesture_poll(void)
{
    if (!s_present || !s_on) return GEST_NONE;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if ((int32_t)(now - s_next_ms) < 0) return GEST_NONE;
    s_next_ms = now + (s_active ? 10 : 40);          /* quick while a hand is passing */

    uint8_t st = 0;
    if (!rd(R_GSTATUS, &st, 1)) return GEST_NONE;
    if (st & 0x01) {                                  /* GVALID: frames waiting */
        uint8_t lvl = 0;
        if (rd(R_GFLVL, &lvl, 1) && lvl) {
            if (lvl > 32) lvl = 32;
            uint8_t buf[128];
            if (rd(R_GFIFO_U, buf, (size_t)lvl * 4)) {
                for (int i = 0; i < lvl; i++) {
                    if (s_nf >= MAX_FRAMES) {   /* a long pass: keep the first frame and the newer half */
                        memmove(s_f[1], s_f[MAX_FRAMES / 2], sizeof s_f[0] * (MAX_FRAMES / 2)); s_nf = MAX_FRAMES / 2 + 1; }
                    memcpy(s_f[s_nf++], buf + i * 4, 4);
                }
            }
        }
        s_active = true; s_idle_since = now;
        return GEST_NONE;
    }
    /* The hand has gone (no new frames for a moment): what was it? */
    if (s_active && now - s_idle_since > 60) {
        gesture_t g = decode();
        s_active = false; s_nf = 0;
        if (g != GEST_NONE) printf("gesture: %s\n", gesture_name(g));
        return g;
    }
    return GEST_NONE;
}

const char *gesture_name(gesture_t g)
{
    switch (g) {
    case GEST_LEFT: return "left"; case GEST_RIGHT: return "right"; case GEST_UP: return "up";
    case GEST_DOWN: return "down"; case GEST_WAVE: return "wave"; default: return "none";
    }
}
