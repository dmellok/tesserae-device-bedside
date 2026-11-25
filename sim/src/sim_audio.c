/*
 * sim_audio.c: sound (the audio.h API, in place of src/audio.c).
 *
 * As on the device: a render loop on its own clock (every 10 ms, the timer on
 * core 1 there) runs the firmware's synth (rainsynth.c: rain, ocean, noise,
 * the dawn chorus, alarm sounds, the radio) into the ring, and the listeners
 * read it: the web app's /audio stream, and the speaker, which here is the
 * Mac's output (an AudioQueue) instead of the I2S amp. The speaker's tone
 * stage (spk_dsp.c) is left out: it is tuned for the panel's small driver.
 */
#include "sim.h"
#include "sim_sdk.h"
#include "audio.h"
#include "click.h"
#include "rainsynth.h"

#include <AudioToolbox/AudioToolbox.h>
#include <unistd.h>

#define BLOCK      256u
#define MAX_CATCH  (AUDIO_SR / 5)

static int16_t   s_small[AUDIO_RING];
static int16_t  *s_ring = s_small;
static uint32_t  s_ring_n = AUDIO_RING;
static volatile uint32_t s_wpos;
static rs_params_t s_params = { false, 0, 3, 5, 1.0f };
static volatile uint32_t s_params_seq;
static volatile int s_listeners;
static volatile bool s_spk_on;
static bool      s_spk_enabled = true, s_spk_audible;
static uint32_t  s_spk_quiet_since, s_busy_us, s_win_us, s_cpu_pm, s_underruns;
static AudioQueueRef s_q;
static uint32_t  s_play_pos;

void audio_use_buffer(int16_t *buf, uint32_t samples)
{
    if (!buf || samples < AUDIO_RING) return;
    memset(buf, 0, samples * sizeof(int16_t));
    s_ring = buf; s_ring_n = samples;
}
uint32_t audio_ring_samples(void) { return s_ring_n; }

static void *render_thread(void *arg)
{
    (void)arg;
    pthread_setname_np("audio");
    uint64_t t0 = sim_real_us(), done = 0;          /* real time: a time warp must not make it render minutes of sound */
    for (;;) {
        usleep(10000);
        uint64_t now = sim_real_us();
        uint64_t due = (now - t0) * AUDIO_SR / 1000000u;
        if (s_listeners <= 0 && !s_spk_on) { done = due; continue; }
        if (due - done > MAX_CATCH) done = due - MAX_CATCH;
        rs_params_t p;
        uint32_t seq;
        do { seq = s_params_seq; __dmb(); p = s_params; __dmb(); } while ((seq & 1u) || seq != s_params_seq);
        while (done < due) {
            int n = (int)(due - done); if (n > (int)BLOCK) n = BLOCK;
            uint32_t w = s_wpos, off = w % s_ring_n;
            if (off + (uint32_t)n > s_ring_n) n = (int)(s_ring_n - off);
            rs_render(&p, &s_ring[off], n);
            __dmb();
            s_wpos = w + (uint32_t)n;
            done += (uint64_t)n;
        }
        uint64_t end = sim_real_us();
        s_busy_us += (uint32_t)(end - now); s_win_us += 10000;
        if (s_win_us >= 1000000) { s_cpu_pm = s_busy_us * 1000u / s_win_us; s_busy_us = s_win_us = 0; }
    }
    return NULL;
}

static click_t s_click;
void audio_click(int level) { if (level > 0) click_ask(&s_click, level > 10 ? 10 : level); }

/* The speaker: the AudioQueue asks for a buffer; give it what the ring holds. */
static void aq_fill(void *u, AudioQueueRef q, AudioQueueBufferRef b)
{
    (void)u;
    int16_t *out = b->mAudioData;
    uint32_t want = b->mAudioDataBytesCapacity / 2, w = s_wpos;
    if (!s_spk_on) { memset(out, 0, want * 2); s_play_pos = w; }
    else {
        if (w - s_play_pos > s_ring_n / 2 || (int32_t)(w - s_play_pos) < 0) s_play_pos = w > 2048 ? w - 2048 : 0;   /* fell behind (or started): catch up */
        uint32_t have = w - s_play_pos, n = have < want ? have : want;
        for (uint32_t i = 0; i < n; i++) out[i] = s_ring[(s_play_pos + i) % s_ring_n];
        if (n < want) { memset(out + n, 0, (want - n) * 2); s_underruns++; }
        s_play_pos += n;
    }
    for (uint32_t i = 0; i < want; i++) {           /* the knob's click, whether or not anything else plays */
        int v = click_next(&s_click);
        if (v) { v += out[i]; out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
    }
    b->mAudioDataByteSize = want * 2;
    AudioQueueEnqueueBuffer(q, b, 0, NULL);
}

static void speaker_open(void)
{
    if (sim_opt.no_audio) { printf("sim: sound off (--no-audio)\n"); return; }
    AudioStreamBasicDescription f = {
        .mSampleRate = AUDIO_SR, .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
        .mBytesPerPacket = 2, .mFramesPerPacket = 1, .mBytesPerFrame = 2, .mChannelsPerFrame = 1, .mBitsPerChannel = 16,
    };
    if (AudioQueueNewOutput(&f, aq_fill, NULL, NULL, NULL, 0, &s_q) != noErr) { printf("sim: no sound output\n"); s_q = NULL; return; }
    for (int i = 0; i < 3; i++) {
        AudioQueueBufferRef b;
        if (AudioQueueAllocateBuffer(s_q, 2048 * 2, &b) == noErr) aq_fill(NULL, s_q, b);
    }
    AudioQueueStart(s_q, NULL);
}

void audio_core1_start(void)
{
    rs_init(time_us_32() | 1u);
    pthread_t t;
    pthread_create(&t, NULL, render_thread, NULL);
    pthread_detach(t);
    speaker_open();
}

void audio_init_pins(void) {}

void audio_set(const rs_params_t *p)
{
    if (!memcmp(p, &s_params, sizeof *p)) return;
    s_params_seq++; __dmb();
    s_params = *p;
    __dmb(); s_params_seq++;
}

void audio_listener(int delta)  { s_listeners += delta; if (s_listeners < 0) s_listeners = 0; }
int  audio_listeners(void)      { return s_listeners; }
int  audio_cpu_permille(void)   { return (s_listeners || s_spk_on) ? (int)s_cpu_pm : 0; }
uint32_t audio_write_pos(void)  { return s_wpos; }

size_t audio_read(uint32_t *pos, int16_t *dst, size_t max)
{
    uint32_t w = s_wpos; __dmb();
    if (w - *pos > s_ring_n - BLOCK * 2) *pos = w - s_ring_n / 2;
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

void audio_speaker_tune(const spk_tune_t *t) { (void)t; }
uint32_t audio_speaker_dropouts(void) { return s_underruns; }
void audio_speaker_enable(bool on) { s_spk_enabled = on; }
void audio_speaker_audible(bool a) { s_spk_audible = a; }
bool audio_speaker_playing(void) { return s_spk_on; }
uint32_t audio_speaker_fills(void) { return 0; }
uint32_t audio_i2s_diag(void) { return 0; }
void audio_speaker_tick(void)
{
    uint32_t now = to_ms_since_boot(get_absolute_time());
    bool want = s_spk_enabled && s_spk_audible;
    if (want) s_spk_quiet_since = 0;
    else if (s_spk_on && !s_spk_quiet_since) s_spk_quiet_since = now | 1u;
    s_spk_on = want || (s_spk_on && s_spk_enabled && now - s_spk_quiet_since < 3000);
}

size_t sim_audio_json(char *b, size_t cap)
{
    return (size_t)snprintf(b, cap, "{\"speaker\":%d,\"output\":%d,\"listeners\":%d,\"cpu_permille\":%d,\"underruns\":%u}",
                            s_spk_on, s_q != NULL, s_listeners, audio_cpu_permille(), s_underruns);
}
