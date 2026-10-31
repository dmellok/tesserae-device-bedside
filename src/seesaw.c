/*
 * seesaw.c: Adafruit seesaw register access on Pico SDK hardware I2C. See seesaw.h.
 */
#include "seesaw.h"

#include <string.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"

#define BUS        i2c0
#define TIMEOUT_US 20000

static bool s_up;

void seesaw_bus_init(void)
{
    if (s_up) return;
    i2c_init(BUS, QWST_I2C_HZ);
    gpio_set_function(QWST_I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(QWST_I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(QWST_I2C_SDA);          /* the QT boards have their own pull-ups too */
    gpio_pull_up(QWST_I2C_SCL);
    s_up = true;
}

bool seesaw_write(uint8_t addr, uint8_t base, uint8_t func, const uint8_t *data, uint8_t len)
{
    uint8_t buf[2 + 32];
    if (len > 32) return false;
    buf[0] = base; buf[1] = func;
    if (len) memcpy(buf + 2, data, len);
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt) sleep_us(1500);
        if (i2c_write_timeout_us(BUS, addr, buf, 2u + len, false, TIMEOUT_US) == (int)(2u + len)) return true;
    }
    return false;
}

/* The seesaw chips refuse I2C for a moment while they are busy (driving their
 * NeoPixels, servicing an interrupt), so one NAK is not an error: retry a few
 * times before giving up. */
bool seesaw_read(uint8_t addr, uint8_t base, uint8_t func, uint8_t *out, uint8_t len, uint32_t delay_us)
{
    uint8_t reg[2] = { base, func };
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt) sleep_us(1500);
        if (i2c_write_timeout_us(BUS, addr, reg, 2, false, TIMEOUT_US) != 2) continue;
        sleep_us(delay_us);
        if (i2c_read_timeout_us(BUS, addr, out, len, false, TIMEOUT_US) == (int)len) return true;
    }
    return false;
}

uint8_t seesaw_probe(uint8_t addr)
{
    uint8_t rst = 0xFF, id = 0;
    if (!seesaw_write(addr, SS_STATUS_BASE, SS_STATUS_SWRST, &rst, 1)) return 0;
    sleep_ms(10);
    for (int i = 0; i < 5; i++) {
        if (seesaw_read(addr, SS_STATUS_BASE, SS_STATUS_HW_ID, &id, 1, 500)) break;
        sleep_ms(10);
    }
    if (id == 0x55 || (id >= 0x84 && id <= 0x89)) return id;
    return 0;
}

static void put_be32(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)(v >> 24); b[1] = (uint8_t)(v >> 16); b[2] = (uint8_t)(v >> 8); b[3] = (uint8_t)v;
}

bool seesaw_pin_mode_input_pullup(uint8_t addr, uint32_t mask)
{
    uint8_t m[4];
    put_be32(m, mask);
    return seesaw_write(addr, SS_GPIO_BASE, SS_GPIO_DIRCLR, m, 4) &&
           seesaw_write(addr, SS_GPIO_BASE, SS_GPIO_PULLENSET, m, 4) &&
           seesaw_write(addr, SS_GPIO_BASE, SS_GPIO_BULK_SET, m, 4);   /* pull direction: up */
}

bool seesaw_read_bulk(uint8_t addr, uint32_t *bits)
{
    uint8_t b[4];
    if (!seesaw_read(addr, SS_GPIO_BASE, SS_GPIO_BULK, b, 4, 500)) return false;
    *bits = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    return true;
}

bool seesaw_encoder_delta(uint8_t addr, int32_t *delta)
{
    uint8_t b[4];
    if (!seesaw_read(addr, SS_ENCODER_BASE, SS_ENC_DELTA, b, 4, 2000)) return false;
    *delta = (int32_t)(((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3]);
    return true;
}

bool seesaw_neopixel_init(uint8_t addr, uint8_t pin, uint8_t n)
{
    uint8_t speed = 1;                                   /* 800 kHz */
    uint8_t len[2] = { (uint8_t)((n * 3) >> 8), (uint8_t)(n * 3) };
    return seesaw_write(addr, SS_NEOPIXEL_BASE, SS_NEO_PIN, &pin, 1) &&
           seesaw_write(addr, SS_NEOPIXEL_BASE, SS_NEO_SPEED, &speed, 1) &&
           seesaw_write(addr, SS_NEOPIXEL_BASE, SS_NEO_BUF_LENGTH, len, 2);
}

bool seesaw_neopixel_set(uint8_t addr, const uint8_t *rgb, uint8_t n)
{
    /* BUF: uint16 BE byte offset, then GRB bytes. Up to 30 bytes per write. */
    uint8_t buf[2 + 30];
    for (uint8_t off = 0; off < n * 3; off += 30) {
        uint8_t chunk = (uint8_t)((n * 3 - off) < 30 ? (n * 3 - off) : 30);
        buf[0] = (uint8_t)(off >> 8); buf[1] = off;
        for (uint8_t i = 0; i < chunk; i += 3) {
            uint8_t px = (uint8_t)((off + i) / 3);
            buf[2 + i]     = rgb[px * 3 + 1];   /* G */
            buf[2 + i + 1] = rgb[px * 3 + 0];   /* R */
            buf[2 + i + 2] = rgb[px * 3 + 2];   /* B */
        }
        if (!seesaw_write(addr, SS_NEOPIXEL_BASE, SS_NEO_BUF, buf, (uint8_t)(2 + chunk))) return false;
    }
    bool ok = seesaw_write(addr, SS_NEOPIXEL_BASE, SS_NEO_SHOW, NULL, 0);
    sleep_us(400);   /* the chip is deaf to I2C while it clocks the pixels out */
    return ok;
}
