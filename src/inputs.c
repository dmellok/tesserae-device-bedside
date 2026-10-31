/*
 * inputs.c: encoder + NeoKey polling. See inputs.h.
 */
#include "inputs.h"
#include "seesaw.h"

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"

#define ENC_BUTTON_PIN   24
#define ENC_NEOPIN       6
#define KEY_FIRST_PIN    4
#define KEY_MASK         (0xFu << KEY_FIRST_PIN)
#define KEY_NEOPIN       3
#define REPROBE_MS       1000
#define LOST_AFTER       30     /* consecutive failed polls (~0.5 s) before a board counts as unplugged */
#define DEBOUNCE_POLLS   2            /* consecutive agreeing polls before a key event */

static bool     s_enc, s_keys;
static int      s_enc_fail, s_key_fail;   /* consecutive failed reads */
static uint32_t s_last_poll_ms, s_last_probe_ms;
static int32_t  s_position;

/* knob button */
static bool     s_btn_down;
static uint32_t s_btn_t0;
static bool     s_btn_long_sent;

/* keys */
static uint8_t  s_key_state;          /* debounced, bit = pressed */
static uint8_t  s_key_raw_prev;
static uint8_t  s_key_agree;
static uint32_t s_key_t0[4];          /* down time per key */
static uint8_t  s_key_held;           /* bit = hold event already sent */

/* backlight */
static uint8_t  s_level = 40;
static bool     s_night;
static uint8_t  s_lit_prev = 0xFF;
static uint8_t  s_enabled = 0x0F;
static uint32_t s_last_key_poll_ms;
static input_event_t s_pend[4];       /* synthesized tap: press then release */
static int      s_npend;

static inline uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

/* Palette (settable): three keys in one colour and the menu key in another
 * (by default warm white and orange, the case design); all in the night colour
 * at night. The knob's ring follows the keys, or stays dark. */
static void lights_update(bool force);
static uint8_t s_col_key[3] = { 255, 200, 150 }, s_col_menu[3] = { 255, 110, 10 }, s_col_night[3] = { 255, 0, 0 };
static bool    s_knob_light = true;

void inputs_set_colours(const uint8_t key[3], const uint8_t menu[3], const uint8_t night[3], bool knob)
{
    memcpy(s_col_key, key, 3); memcpy(s_col_menu, menu, 3); memcpy(s_col_night, night, 3);
    s_knob_light = knob;
    lights_update(true);
}

static void key_colour(int i, bool pressed, uint8_t out[3])
{
    const uint8_t *c = s_night ? s_col_night : i == 3 ? s_col_menu : s_col_key;
    uint8_t r = c[0], g = c[1], b = c[2];
    uint32_t lvl = pressed ? 255u : ((s_enabled >> i) & 1) ? s_level : 0u;
    if (pressed && s_level == 0) lvl = 60;                  /* feedback even when dark */
    out[0] = (uint8_t)(r * lvl / 255); out[1] = (uint8_t)(g * lvl / 255); out[2] = (uint8_t)(b * lvl / 255);
}

static void lights_update(bool force)
{
    uint8_t lit = s_key_state;
    if (!force && lit == s_lit_prev) return;
    s_lit_prev = lit;
    if (s_keys) {
        uint8_t px[12];
        for (int i = 0; i < 4; i++) key_colour(i, (lit >> i) & 1, px + i * 3);
        seesaw_neopixel_set(NEOKEY_ADDR, px, 4);
    }
    if (s_enc) {
        uint8_t px[3];
        key_colour(0, s_btn_down, px);
        if (!s_knob_light && !s_btn_down) px[0] = px[1] = px[2] = 0;
        /* knob ring: half the key brightness */
        px[0] /= 2; px[1] /= 2; px[2] /= 2;
        seesaw_neopixel_set(ENCODER_ADDR, px, 1);
    }
}

static void probe(void)
{
    s_last_probe_ms = now_ms();
    if (!s_enc) {
        uint8_t id = seesaw_probe(ENCODER_ADDR);
        if (id) {
            s_enc = seesaw_pin_mode_input_pullup(ENCODER_ADDR, 1u << ENC_BUTTON_PIN) &&
                    seesaw_neopixel_init(ENCODER_ADDR, ENC_NEOPIN, 1);
            int32_t d; seesaw_encoder_delta(ENCODER_ADDR, &d);   /* discard boot delta */
            printf("inputs: rotary encoder at 0x%02x (seesaw id 0x%02x)%s\n", ENCODER_ADDR, id, s_enc ? "" : " init FAILED");
        }
    }
    if (!s_keys) {
        uint8_t id = seesaw_probe(NEOKEY_ADDR);
        if (id) {
            uint8_t m[4] = { 0, 0, 0, (uint8_t)KEY_MASK };
            s_keys = seesaw_pin_mode_input_pullup(NEOKEY_ADDR, KEY_MASK) &&
                     seesaw_write(NEOKEY_ADDR, SS_GPIO_BASE, SS_GPIO_INTENSET, m, 4) &&   /* latch changes */
                     seesaw_neopixel_init(NEOKEY_ADDR, KEY_NEOPIN, 4);
            printf("inputs: NeoKey 1x4 at 0x%02x (seesaw id 0x%02x)%s\n", NEOKEY_ADDR, id, s_keys ? "" : " init FAILED");
        }
    }
    if (s_enc || s_keys) lights_update(true);
}

void inputs_init(void)
{
    seesaw_bus_init();
    probe();
    if (!s_enc && !s_keys) printf("inputs: no Qw/ST boards found (GP%d/GP%d); will keep looking\n", QWST_I2C_SDA, QWST_I2C_SCL);
}

static input_event_t ev(input_kind_t k, int v) { input_event_t e = { k, v }; return e; }

static bool read_intflag(uint8_t *keys)
{
    uint8_t b[4];
    if (!seesaw_read(NEOKEY_ADDR, SS_GPIO_BASE, SS_GPIO_INTFLAG, b, 4, 500)) return false;
    uint32_t f = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    *keys = (uint8_t)((f >> KEY_FIRST_PIN) & 0xF);
    return true;
}

void inputs_discard(void)
{
    int32_t d; uint8_t k;
    if (s_enc) seesaw_encoder_delta(ENCODER_ADDR, &d);
    if (s_keys) read_intflag(&k);
    s_npend = 0;
    s_last_key_poll_ms = now_ms();
}

input_event_t inputs_poll(void)
{
    if (s_npend) { input_event_t e = s_pend[0]; memmove(&s_pend[0], &s_pend[1], sizeof s_pend[0] * (size_t)(--s_npend)); return e; }
    uint32_t t = now_ms();
    if (t - s_last_poll_ms < INPUTS_POLL_MS) return ev(IN_NONE, 0);
    s_last_poll_ms = t;

    if ((!s_enc || !s_keys) && t - s_last_probe_ms > REPROBE_MS) probe();

    /* ---- knob ---- */
    if (s_enc) {
        int32_t d = 0;
        if (!seesaw_encoder_delta(ENCODER_ADDR, &d)) {
            if (++s_enc_fail >= LOST_AFTER) { printf("inputs: encoder lost after %d failed reads\n", s_enc_fail); s_enc = false; s_enc_fail = 0; }
        } else if ((s_enc_fail = 0), d != 0) {
            s_position += d;
            return ev(IN_TURN, (int)d);
        }
        uint32_t bits;
        if (s_enc && seesaw_read_bulk(ENCODER_ADDR, &bits)) {
            bool down = !(bits & (1u << ENC_BUTTON_PIN));
            if (down && !s_btn_down) { s_btn_down = true; s_btn_t0 = t; s_btn_long_sent = false; lights_update(true); }
            else if (down && !s_btn_long_sent && t - s_btn_t0 >= INPUTS_LONG_PRESS_MS) {
                s_btn_long_sent = true;
                return ev(IN_PRESS_LONG, 0);
            } else if (!down && s_btn_down) {
                s_btn_down = false; lights_update(true);
                if (!s_btn_long_sent) return ev(IN_PRESS_SHORT, 0);
            }
        }
    }

    /* ---- keys ---- */
    if (s_keys) {
        uint32_t bits;
        if (!seesaw_read_bulk(NEOKEY_ADDR, &bits)) {
            if (++s_key_fail >= LOST_AFTER) { printf("inputs: NeoKey lost after %d failed reads\n", s_key_fail); s_keys = false; s_key_fail = 0; }
            return ev(IN_NONE, 0);
        }
        s_key_fail = 0;
        uint8_t raw = (uint8_t)(~(bits >> KEY_FIRST_PIN) & 0xF);   /* active low -> bit = pressed */
        /* A key that went down and up again since the last poll (the loop was
         * busy painting) shows only in the latched flags. Report it as a tap if
         * the gap was short; otherwise it has expired. */
        uint8_t flags = 0;
        uint32_t gap = t - s_last_key_poll_ms;
        s_last_key_poll_ms = t;
        if (read_intflag(&flags)) {
            uint8_t missed = (uint8_t)(flags & ~raw & ~s_key_state);
            if (missed && gap > 3 * INPUTS_POLL_MS) {
                for (int i = 0; i < 4 && s_npend < 3; i++) {
                    if (!((missed >> i) & 1)) continue;
                    if (gap > INPUTS_TAP_EXPIRE_MS) { printf("inputs: key %d tap expired (%lu ms)\n", i + 1, (unsigned long)gap); continue; }
                    s_pend[s_npend++] = ev(IN_KEY, i);
                    s_pend[s_npend++] = ev(IN_KEY_RELEASE, i);
                }
                if (s_npend) { input_event_t e = s_pend[0]; memmove(&s_pend[0], &s_pend[1], sizeof s_pend[0] * (size_t)(--s_npend)); return e; }
            }
        }
        if (raw == s_key_raw_prev) { if (s_key_agree < 255) s_key_agree++; }
        else { s_key_raw_prev = raw; s_key_agree = 0; }
        if (s_key_agree >= DEBOUNCE_POLLS && raw != s_key_state) {
            uint8_t changed = raw ^ s_key_state;
            for (int i = 0; i < 4; i++) {
                if (!(changed & (1u << i))) continue;
                s_key_state = (uint8_t)((s_key_state & ~(1u << i)) | (raw & (1u << i)));
                lights_update(false);
                if ((raw >> i) & 1) { s_key_t0[i] = t; s_key_held &= (uint8_t)~(1u << i); return ev(IN_KEY, i); }
                return ev(IN_KEY_RELEASE, i);
            }
        }
        for (int i = 0; i < 4; i++) {
            if (((s_key_state >> i) & 1) && !((s_key_held >> i) & 1) && t - s_key_t0[i] >= INPUTS_LONG_PRESS_MS) {
                s_key_held |= (uint8_t)(1u << i);
                return ev(IN_KEY_HOLD, i);
            }
        }
    }
    return ev(IN_NONE, 0);
}

bool    inputs_have_encoder(void)     { return s_enc; }
bool    inputs_have_keys(void)        { return s_keys; }
int32_t inputs_encoder_position(void) { return s_position; }

void    inputs_set_backlight(uint8_t level) { s_level = level; lights_update(true); }
void    inputs_set_night(bool night)        { s_night = night; lights_update(true); }
void    inputs_set_key_enabled(uint8_t mask)  { if (mask != s_enabled) { s_enabled = mask; lights_update(true); } }
uint8_t inputs_backlight(void)              { return s_level; }
bool    inputs_night(void)                  { return s_night; }
