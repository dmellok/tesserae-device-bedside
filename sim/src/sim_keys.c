/*
 * sim_keys.c: the two Qw/ST boards, simulated at the seesaw protocol (the
 * seesaw.h API, in place of src/seesaw.c), so src/inputs.c runs unchanged:
 * its polling, debounce, latched taps, holds and LED updates.
 *
 *   0x36  rotary encoder: the knob's delta, its button on pin 24, one NeoPixel
 *   0x30  NeoKey 1x4: four keys on pins 4..7 (active low, change flags latched
 *         in INTFLAG until read), four NeoPixels
 *
 * The simulator page presses and releases keys, turns and presses the knob,
 * and shows the LED colours the firmware sends.
 */
#include "sim.h"
#include "sim_sdk.h"
#include "seesaw.h"
#include "inputs.h"

#define KEY_PIN0  4
#define ENC_PIN   24

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t  s_keys_down;          /* bit i = key i held */
static uint8_t  s_key_flags;          /* changed since INTFLAG was last read */
static bool     s_knob_down;
static int32_t  s_delta;
static uint8_t  s_key_rgb[4][3], s_knob_rgb[3];
static uint32_t s_led_writes;

static bool present(uint8_t addr)
{
    if (addr == ENCODER_ADDR) return !sim_opt.no_knob;
    if (addr == NEOKEY_ADDR)  return !sim_opt.no_keys;
    return false;
}

void sim_key(int i, bool down)
{
    if (i < 0 || i > 3) return;
    pthread_mutex_lock(&s_lock);
    uint8_t m = (uint8_t)(1u << i);
    if (!!(s_keys_down & m) != down) { s_keys_down ^= m; s_key_flags |= m; }
    pthread_mutex_unlock(&s_lock);
}
void sim_knob_turn(int d)      { pthread_mutex_lock(&s_lock); s_delta += d; pthread_mutex_unlock(&s_lock); }
void sim_knob_button(bool d)   { s_knob_down = d; }

void seesaw_bus_init(void) {}
uint8_t seesaw_probe(uint8_t addr) { return present(addr) ? 0x55 : 0; }
bool seesaw_pin_mode_input_pullup(uint8_t addr, uint32_t mask) { (void)mask; return present(addr); }
bool seesaw_neopixel_init(uint8_t addr, uint8_t pin, uint8_t n) { (void)pin; (void)n; return present(addr); }

bool seesaw_write(uint8_t addr, uint8_t base, uint8_t func, const uint8_t *data, uint8_t len)
{
    (void)base; (void)func; (void)data; (void)len;
    return present(addr);
}

bool seesaw_read(uint8_t addr, uint8_t base, uint8_t func, uint8_t *out, uint8_t len, uint32_t delay_us)
{
    (void)delay_us;
    if (!present(addr)) return false;
    memset(out, 0, len);
    if (addr == NEOKEY_ADDR && base == SS_GPIO_BASE && func == SS_GPIO_INTFLAG && len == 4) {
        pthread_mutex_lock(&s_lock);
        uint32_t f = (uint32_t)s_key_flags << KEY_PIN0;
        s_key_flags = 0;
        pthread_mutex_unlock(&s_lock);
        out[0] = (uint8_t)(f >> 24); out[1] = (uint8_t)(f >> 16); out[2] = (uint8_t)(f >> 8); out[3] = (uint8_t)f;
    }
    return true;
}

bool seesaw_read_bulk(uint8_t addr, uint32_t *bits)
{
    if (!present(addr)) return false;
    uint32_t b = 0xFFFFFFFFu;                                  /* pulled up: high = not pressed */
    if (addr == NEOKEY_ADDR) b &= ~((uint32_t)s_keys_down << KEY_PIN0);
    if (addr == ENCODER_ADDR && s_knob_down) b &= ~(1u << ENC_PIN);
    *bits = b;
    return true;
}

bool seesaw_encoder_delta(uint8_t addr, int32_t *delta)
{
    if (!present(addr)) return false;
    pthread_mutex_lock(&s_lock);
    *delta = s_delta; s_delta = 0;
    pthread_mutex_unlock(&s_lock);
    return true;
}

bool seesaw_neopixel_set(uint8_t addr, const uint8_t *rgb, uint8_t n)
{
    if (!present(addr)) return false;
    pthread_mutex_lock(&s_lock);
    if (addr == NEOKEY_ADDR) for (int i = 0; i < n && i < 4; i++) memcpy(s_key_rgb[i], rgb + 3 * i, 3);
    if (addr == ENCODER_ADDR && n >= 1) memcpy(s_knob_rgb, rgb, 3);
    s_led_writes++;
    pthread_mutex_unlock(&s_lock);
    return true;
}

size_t sim_keys_json(char *b, size_t cap)
{
    pthread_mutex_lock(&s_lock);
    size_t n = (size_t)snprintf(b, cap, "{\"keys\":%d,\"knob\":%d,\"down\":%u,\"knob_down\":%d,\"led\":[", !sim_opt.no_keys, !sim_opt.no_knob,
                                s_keys_down, s_knob_down);
    for (int i = 0; i < 4; i++) n += (size_t)snprintf(b + n, cap - n, "%s[%u,%u,%u]", i ? "," : "", s_key_rgb[i][0], s_key_rgb[i][1], s_key_rgb[i][2]);
    n += (size_t)snprintf(b + n, cap - n, "],\"knob_led\":[%u,%u,%u],\"led_writes\":%u}", s_knob_rgb[0], s_knob_rgb[1], s_knob_rgb[2], s_led_writes);
    pthread_mutex_unlock(&s_lock);
    return n;
}
