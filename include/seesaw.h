/*
 * seesaw.h: minimal driver for Adafruit "seesaw" I2C peripherals (the firmware
 * on the ATtiny8x7 / SAMD09 that the QT rotary encoder breakout and the NeoKey
 * 1x4 QT run). Register map per Adafruit_seesaw.h: every transaction is a
 * module base byte + function byte, then data; reads need a short pause
 * between the address write and the read.
 *
 * Bus: the Pico Plus 2 W's Qw/ST connector is I2C0 on GP4 (SDA) / GP5 (SCL).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifndef QWST_I2C_SDA
#define QWST_I2C_SDA 4
#endif
#ifndef QWST_I2C_SCL
#define QWST_I2C_SCL 5
#endif
#ifndef QWST_I2C_HZ
#define QWST_I2C_HZ 100000   /* conservative over 100 mm QT cables; seesaw does 400 k too */
#endif

/* Module bases */
#define SS_STATUS_BASE    0x00
#define SS_GPIO_BASE      0x01
#define SS_NEOPIXEL_BASE  0x0E
#define SS_ENCODER_BASE   0x11

/* Status functions */
#define SS_STATUS_HW_ID   0x01
#define SS_STATUS_VERSION 0x02
#define SS_STATUS_SWRST   0x7F

/* GPIO functions (32-bit big-endian pin masks) */
#define SS_GPIO_DIRSET    0x02
#define SS_GPIO_DIRCLR    0x03
#define SS_GPIO_BULK      0x04
#define SS_GPIO_BULK_SET  0x05
#define SS_GPIO_BULK_CLR  0x06
#define SS_GPIO_PULLENSET 0x0B
#define SS_GPIO_PULLENCLR 0x0C
#define SS_GPIO_INTENSET  0x08
#define SS_GPIO_INTFLAG   0x0A   /* pins that changed since the last read; cleared by reading */

/* NeoPixel functions */
#define SS_NEO_PIN        0x01
#define SS_NEO_SPEED      0x02
#define SS_NEO_BUF_LENGTH 0x03
#define SS_NEO_BUF        0x04
#define SS_NEO_SHOW       0x05

/* Encoder functions (+ encoder index) */
#define SS_ENC_POSITION   0x30
#define SS_ENC_DELTA      0x40

/* Bring up I2C0 on the Qw/ST pins. Idempotent. */
void seesaw_bus_init(void);

/* Raw register access. Return false on a NACK / timeout. */
bool seesaw_write(uint8_t addr, uint8_t base, uint8_t func, const uint8_t *data, uint8_t len);
bool seesaw_read(uint8_t addr, uint8_t base, uint8_t func, uint8_t *out, uint8_t len, uint32_t delay_us);

/* Probe: software reset, then read HW_ID. Returns the id (0x55 SAMD09,
 * 0x84..0x89 ATtiny8x7) or 0 if nothing answered. */
uint8_t seesaw_probe(uint8_t addr);

/* GPIO helpers on a 32-bit pin mask. */
bool seesaw_pin_mode_input_pullup(uint8_t addr, uint32_t mask);
bool seesaw_read_bulk(uint8_t addr, uint32_t *bits);

/* Encoder: signed delta since the last read, index 0. */
bool seesaw_encoder_delta(uint8_t addr, int32_t *delta);

/* NeoPixels: configure (GRB, 800 kHz) and write n pixels as packed RGB. */
bool seesaw_neopixel_init(uint8_t addr, uint8_t pin, uint8_t n);
bool seesaw_neopixel_set(uint8_t addr, const uint8_t *rgb, uint8_t n);
