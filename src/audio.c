/*
 * audio.c: sleep sounds on core 1. See audio.h.
 */
#include "audio.h"

#include <string.h>
#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "hardware/pio.h"
#include "hardware/pio_instructions.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"

#define BLOCK      220                    /* 10 ms */
#define MAX_CATCH  (AUDIO_SR / 5)         /* after a stall, catch up at most 200 ms */

static int16_t            s_ring_sram[AUDIO_RING];
static int16_t           *s_ring = s_ring_sram;
static uint32_t           s_ring_n = AUDIO_RING;
static volatile uint32_t  s_wpos;         /* samples written, ever */
static volatile int       s_listeners;
static rs_params_t        s_params = { false, 0, 3, 5, 1.0f };
static volatile uint32_t  s_params_seq;   /* bumped by core 0 on change */
static repeating_timer_t  s_timer;
static uint64_t           s_t0_us, s_due;  /* sample clock */
static uint32_t           s_busy_us, s_win_us, s_cpu_pm;

/* ---- the speaker: MAX98357A over I2S (docs/wiring-guide.html) ------------------------
 * PIO shifts 32-bit frames (left in the top half, the same sample on both
 * sides; the amp plays left with SD high), two DMA channels ping-pong over
 * two SRAM buffers, and the DMA interrupt (on core 1) refills the one just
 * played from the ring, a few blocks behind the writer. GP16 drives the amp's
 * SD pin: low shuts it down completely. */
#define I2S_BCLK   18                     /* LRCLK = 19 (BCLK + 1) */
#define I2S_DIN    21
#define AMP_SD     16
#define I2S_FRAMES 256                    /* ~11.6 ms per buffer */
#define SPK_LAG    (5 * BLOCK)            /* how far behind the writer the speaker reads (~50 ms) */

static uint32_t          s_i2s_buf[2][I2S_FRAMES];
static int               s_dma[2] = { -1, -1 };
static PIO               s_i2s_pio;
static uint              s_i2s_sm, s_i2s_offset;
static volatile bool     s_i2s_ready;
static volatile bool     s_spk_on;        /* core 0: play through the speaker */
static uint32_t          s_spk_pos;       /* ring read position (samples) */
static bool              s_spk_was_on;

/* pico-extras' audio_i2s program, built with the encoders (side-set: bit 0 BCLK, bit 1 LRCLK). */
static uint16_t s_i2s_insns[8];
static const struct pio_program s_i2s_prog = { .instructions = s_i2s_insns, .length = 8, .origin = -1 };
static void i2s_build_program(void)
{
#define SS(v) pio_encode_sideset(2, v)
    s_i2s_insns[0] = pio_encode_out(pio_pins, 1) | SS(2);       /* bitloop1 */
    s_i2s_insns[1] = pio_encode_jmp_x_dec(0)     | SS(3);
    s_i2s_insns[2] = pio_encode_out(pio_pins, 1) | SS(0);
    s_i2s_insns[3] = pio_encode_set(pio_x, 14)   | SS(1);
    s_i2s_insns[4] = pio_encode_out(pio_pins, 1) | SS(0);       /* bitloop0 */
    s_i2s_insns[5] = pio_encode_jmp_x_dec(4)     | SS(1);
    s_i2s_insns[6] = pio_encode_out(pio_pins, 1) | SS(2);
    s_i2s_insns[7] = pio_encode_set(pio_x, 14)   | SS(3);       /* entry point */
#undef SS
}

static void spk_fill(uint32_t *dst)
{
    if (!s_spk_on) { memset(dst, 0, I2S_FRAMES * 4); s_spk_was_on = false; return; }
    uint32_t w = s_wpos; __dmb();
    if (!s_spk_was_on) { s_spk_pos = w; s_spk_was_on = true; }           /* start fresh: older ring data is stale */
    int32_t avail = (int32_t)(w - s_spk_pos);
    if (avail < 0 || avail > (int32_t)(s_ring_n / 2)) { s_spk_pos = w - SPK_LAG; avail = SPK_LAG; }   /* lost track: resync */
    static bool warm;                                                     /* let the writer get ahead first */
    if (avail < 64) warm = true;                                          /* ran dry: refill before playing on */
    if (warm) { if (avail >= SPK_LAG) warm = false; else { memset(dst, 0, I2S_FRAMES * 4); return; } }
    for (int i = 0; i < I2S_FRAMES; i++) {
        int16_t v = 0;
        if (avail > 0) { v = s_ring[s_spk_pos % s_ring_n]; s_spk_pos++; avail--; }   /* else an underrun: silence */
        uint16_t u = (uint16_t)v;
        dst[i] = (uint32_t)u << 16 | u;
    }
}

static volatile uint32_t s_i2s_fills;     /* buffers refilled: the I2S clock is running */
uint32_t audio_speaker_fills(void) { return s_i2s_fills; }
/* Diagnostics: bit 0 the I2S state machine runs, bit 1 ready, bits 8+ the PIO index and SM, bit 16+ clock toggles seen on BCLK. */
uint32_t audio_i2s_diag(void)
{
    if (!s_i2s_ready) return 0;
    uint32_t en = (s_i2s_pio->ctrl >> s_i2s_sm) & 1u;
    int toggles = 0; bool last = gpio_get(I2S_BCLK);
    for (int i = 0; i < 2000; i++) { bool v = gpio_get(I2S_BCLK); if (v != last) { toggles++; last = v; } }
    return en | 2u | (PIO_NUM(s_i2s_pio) << 8) | (s_i2s_sm << 12) | ((uint32_t)toggles << 16);
}
static void __isr on_i2s_dma(void)
{
    for (int k = 0; k < 2; k++) {
        if (s_dma[k] < 0 || !dma_channel_get_irq1_status((uint)s_dma[k])) continue;
        dma_channel_acknowledge_irq1((uint)s_dma[k]);
        spk_fill(s_i2s_buf[k]);                                   /* the other one is playing now */
        s_i2s_fills++;
        dma_channel_set_read_addr((uint)s_dma[k], s_i2s_buf[k], false);
    }
}

static void i2s_start(void)
{
    i2s_build_program();
    uint offset;
    if (!pio_claim_free_sm_and_add_program_for_gpio_range(&s_i2s_prog, &s_i2s_pio, &s_i2s_sm, &offset, I2S_BCLK, 4, true)) return;
    pio_gpio_init(s_i2s_pio, I2S_DIN); pio_gpio_init(s_i2s_pio, I2S_BCLK); pio_gpio_init(s_i2s_pio, I2S_BCLK + 1);
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset, offset + 7);
    sm_config_set_sideset(&c, 2, false, false);
    sm_config_set_out_pins(&c, I2S_DIN, 1);
    sm_config_set_sideset_pins(&c, I2S_BCLK);
    sm_config_set_out_shift(&c, false, true, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / (float)(AUDIO_SR * 64));   /* 2 instructions per bit, 32 bits a frame */
    pio_sm_init(s_i2s_pio, s_i2s_sm, offset + 7, &c);
    pio_sm_set_consecutive_pindirs(s_i2s_pio, s_i2s_sm, I2S_BCLK, 2, true);
    pio_sm_set_consecutive_pindirs(s_i2s_pio, s_i2s_sm, I2S_DIN, 1, true);
    pio_sm_exec(s_i2s_pio, s_i2s_sm, pio_encode_jmp(offset + 7));

    s_dma[0] = dma_claim_unused_channel(true); s_dma[1] = dma_claim_unused_channel(true);
    for (int k = 0; k < 2; k++) {
        dma_channel_config d = dma_channel_get_default_config((uint)s_dma[k]);
        channel_config_set_transfer_data_size(&d, DMA_SIZE_32);
        channel_config_set_read_increment(&d, true);
        channel_config_set_write_increment(&d, false);
        channel_config_set_dreq(&d, pio_get_dreq(s_i2s_pio, s_i2s_sm, true));
        channel_config_set_chain_to(&d, (uint)s_dma[k ^ 1]);
        memset(s_i2s_buf[k], 0, sizeof s_i2s_buf[k]);
        dma_channel_configure((uint)s_dma[k], &d, &s_i2s_pio->txf[s_i2s_sm], s_i2s_buf[k], I2S_FRAMES, false);
        dma_channel_set_irq1_enabled((uint)s_dma[k], true);
    }
    irq_add_shared_handler(DMA_IRQ_1, on_i2s_dma, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    irq_set_enabled(DMA_IRQ_1, true);
    s_i2s_offset = offset;
    s_i2s_ready = true;
    dma_channel_start((uint)s_dma[0]);    /* it waits on the TX FIFO until the clock runs */
}

/* The I2S clock runs only while the amp is on: an idle 1.4 MHz bit clock on the
 * header is interference the panel's SPI lines don't need. */
static void i2s_clock(bool on)
{
    if (!s_i2s_ready) return;
    const uint32_t mask = (1u << I2S_BCLK) | (1u << (I2S_BCLK + 1)) | (1u << I2S_DIN);
    if (on) {
        pio_sm_restart(s_i2s_pio, s_i2s_sm);
        pio_sm_exec(s_i2s_pio, s_i2s_sm, pio_encode_jmp(s_i2s_offset + 7));
        pio_sm_set_enabled(s_i2s_pio, s_i2s_sm, true);
    } else {
        pio_sm_set_enabled(s_i2s_pio, s_i2s_sm, false);
        pio_sm_set_pins_with_mask(s_i2s_pio, s_i2s_sm, 0, mask);   /* park the lines low */
    }
}

/* Core 0: the amp follows what's audible, with a tail so fades finish. */
static bool     s_spk_enabled = true, s_spk_audible;
static uint32_t s_spk_quiet_since;
void audio_speaker_enable(bool on) { s_spk_enabled = on; }
void audio_speaker_audible(bool audible) { s_spk_audible = audible; }
bool audio_speaker_playing(void) { return s_spk_on; }
void audio_speaker_tick(void)
{
    uint32_t now = to_ms_since_boot(get_absolute_time());
    bool want = s_spk_enabled && s_spk_audible;
    if (want) s_spk_quiet_since = 0;
    else if (s_spk_on && !s_spk_quiet_since) s_spk_quiet_since = now | 1u;
    bool on = want || (s_spk_on && s_spk_enabled && now - s_spk_quiet_since < 3000);   /* let the ~1.2 s fade out play */
    if (on != s_spk_on) {
        s_spk_on = on;
        if (on) { i2s_clock(true); gpio_put(AMP_SD, 1); }
        else    { gpio_put(AMP_SD, 0); i2s_clock(false); }
    }
}

static bool on_timer(repeating_timer_t *t)
{
    (void)t;
    uint64_t now = time_us_64();
    uint64_t due = (now - s_t0_us) * AUDIO_SR / 1000000u;     /* samples real time has asked for */
    if (s_listeners <= 0 && !s_spk_on) { s_due = due; return true; }   /* nobody listening: keep the clock only */
    if (due - s_due > MAX_CATCH) s_due = due - MAX_CATCH;    /* long stall (flash write): skip ahead */

    rs_params_t p;
    uint32_t seq;
    do { seq = s_params_seq; __dmb(); p = s_params; __dmb(); } while ((seq & 1u) || seq != s_params_seq);

    while (s_due < due) {
        int n = (int)(due - s_due); if (n > BLOCK) n = BLOCK;
        uint32_t w = s_wpos, off = w % s_ring_n;
        if (off + (uint32_t)n > s_ring_n) n = (int)(s_ring_n - off);
        rs_render(&p, &s_ring[off], n);
        __dmb();
        s_wpos = w + (uint32_t)n;
        s_due += (uint64_t)n;
    }
    uint64_t end = time_us_64();
    s_busy_us += (uint32_t)(end - now);
    s_win_us  += 10000;
    if (s_win_us >= 1000000) { s_cpu_pm = s_busy_us * 1000u / s_win_us; s_busy_us = s_win_us = 0; }
    return true;
}

void audio_use_buffer(int16_t *buf, uint32_t samples)
{
    if (!buf || samples < AUDIO_RING) return;
    memset(buf, 0, samples * sizeof(int16_t));
    s_ring = buf; s_ring_n = samples;
}
uint32_t audio_ring_samples(void) { return s_ring_n; }

void audio_core1_start(void)
{
    rs_init(time_us_32() | 1u);
    s_t0_us = time_us_64();
    s_due = 0;
    /* A pool on its own hardware alarm, created here so its IRQ lands on core 1. */
    alarm_pool_t *pool = alarm_pool_create_with_unused_hardware_alarm(2);
    alarm_pool_add_repeating_timer_us(pool, -10000, on_timer, NULL, &s_timer);
    i2s_start();                          /* its DMA interrupt lands on core 1 too */
}

void audio_init_pins(void)
{
    gpio_init(AMP_SD); gpio_set_dir(AMP_SD, GPIO_OUT); gpio_put(AMP_SD, 0);   /* the amp off until there's sound */
}

void audio_set(const rs_params_t *p)
{
    if (!memcmp(p, &s_params, sizeof *p)) return;
    s_params_seq++; __dmb();
    s_params = *p;
    __dmb(); s_params_seq++;
}

void audio_listener(int delta) { s_listeners += delta; if (s_listeners < 0) s_listeners = 0; }
int  audio_listeners(void)     { return s_listeners; }
int  audio_cpu_permille(void)  { return (s_listeners || s_spk_on) ? (int)s_cpu_pm : 0; }
uint32_t audio_write_pos(void) { return s_wpos; }

size_t audio_read(uint32_t *pos, int16_t *dst, size_t max)
{
    uint32_t w = s_wpos; __dmb();
    if (w - *pos > s_ring_n - BLOCK * 2) *pos = w - s_ring_n / 2;       /* fell behind: skip */
    size_t n = w - *pos;
    if (n > max) n = max;
    for (size_t i = 0; i < n; ) {
        uint32_t off = (*pos + (uint32_t)i) % s_ring_n;
        size_t run = s_ring_n - off; if (run > n - i) run = n - i;
        memcpy(dst + i, &s_ring[off], run * sizeof(int16_t));
        i += run;
    }
    *pos += (uint32_t)n;
    return n;
}
