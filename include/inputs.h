/*
 * inputs.h: the knob and the four keys (proposal, "Physical controls").
 *
 *   Knob  Adafruit I2C QT Rotary Encoder breakout (#5880 / #4991): seesaw at
 *         0x36, detented encoder, push button on seesaw pin 24, one NeoPixel
 *         on pin 6.
 *   Keys  Adafruit NeoKey 1x4 QT (#4980): seesaw at 0x30, four MX sockets on
 *         seesaw pins 4..7 (active low), four NeoPixel backlights on pin 3.
 *
 * Both hang off the Qw/ST connector (I2C0, GP4/GP5) and are polled every
 * INPUTS_POLL_MS from the main loop (no INT line on Qw/ST). A board that is
 * missing at boot is re-probed every few seconds, so the cable can be plugged
 * in live. Events are debounced here and handed to main.c as a small enum;
 * what they *do* (menu, server button names) lives in main.c.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifndef ENCODER_ADDR
#define ENCODER_ADDR 0x36
#endif
#ifndef NEOKEY_ADDR
#define NEOKEY_ADDR  0x30
#endif
#ifndef INPUTS_POLL_MS
#define INPUTS_POLL_MS 15
#endif
#ifndef INPUTS_LONG_PRESS_MS
#define INPUTS_LONG_PRESS_MS 600
#endif

typedef enum {
    IN_NONE = 0,
    IN_TURN,          /* value = detents, + clockwise */
    IN_PRESS_SHORT,   /* knob */
    IN_PRESS_LONG,    /* knob held */
    IN_KEY,           /* key went down; value = key index 0..3 (left to right) */
    IN_KEY_HOLD,      /* key held INPUTS_LONG_PRESS_MS; value = key index */
    IN_KEY_RELEASE,   /* key came up; value = key index. main.c decides whether a
                         press means "on down" (no hold action) or "on release" */
} input_kind_t;

typedef struct {
    input_kind_t kind;
    int          value;
} input_event_t;

void inputs_init(void);

/* Throw away anything that happened since the last poll: knob detents and key
 * taps made while the screen was changing to another page. */
void inputs_discard(void);

/* A key tapped and released between two polls is still reported (the NeoKey
 * latches the change), but only if the gap was shorter than this; older taps
 * expire rather than replaying late. */
#ifndef INPUTS_TAP_EXPIRE_MS
#define INPUTS_TAP_EXPIRE_MS 300
#endif

/* Poll the bus if INPUTS_POLL_MS has elapsed; returns at most one event per
 * call (call repeatedly). */
input_event_t inputs_poll(void);

bool inputs_have_encoder(void);
bool inputs_have_keys(void);
int32_t inputs_encoder_position(void);   /* detents since boot */

/* Backlights. level 0..255 scales everything; night switches the palette to a
 * dim red so the keys do not spoil night vision. A key being held is lit
 * brighter for feedback. Persisted by the caller if wanted. */
void    inputs_set_backlight(uint8_t level);
void    inputs_set_night(bool night);
/* Key colours (RGB): the three keys, the menu key, everything at night; and
 * whether the knob's ring lights (it always flashes on a press). */
void    inputs_set_colours(const uint8_t key[3], const uint8_t menu[3], const uint8_t night[3], bool knob);
/* Keys with nothing to do on the current screen go dark (bit i = key i lit). */
void    inputs_set_key_enabled(uint8_t mask);
uint8_t inputs_backlight(void);
bool    inputs_night(void);
