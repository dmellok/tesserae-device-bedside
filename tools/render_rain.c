/* Render the sleep-sound synth on a host to audition it and check levels.
 *   cc -O2 -Iinclude tools/render_rain.c src/rainsynth.c -lm -o /tmp/render_rain
 *   /tmp/render_rain out_dir          -> rain_l1.wav ... noise_l5.wav, plus a level report
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rainsynth.h"
#include "birds.h"

/* the chorus clips, loaded from assets/birds/birds.ulaw (the firmware embeds them) */
uint8_t bird_data[4 * 1024 * 1024];

static void wav(const char *path, const int16_t *s, int n)
{
    FILE *f = fopen(path, "wb");
    uint32_t sr = RAIN_SR, br = sr * 2, sz = (uint32_t)n * 2, ch = 36 + sz;
    fwrite("RIFF", 1, 4, f); fwrite(&ch, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    uint32_t fl = 16; uint16_t pcm = 1, mono = 1, ba = 2, bits = 16;
    fwrite(&fl, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&mono, 2, 1, f); fwrite(&sr, 4, 1, f);
    fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&sz, 4, 1, f); fwrite(s, 2, (size_t)n, f); fclose(f);
}

static void level(const char *label, const int16_t *buf, int from, int n)
{
    double ss = 0; int pk = 0, clip = 0;
    for (int i = from; i < n; i++) { ss += (double)buf[i] * buf[i]; int a = abs(buf[i]); if (a > pk) pk = a; if (a > 32000) clip++; }
    double rms = sqrt(ss / (n - from));
    printf("%-22s rms %5.1f dBFS  peak %5.1f dBFS  clipped %d\n", label, 20 * log10(rms / 32768 + 1e-9), 20 * log10(pk / 32768.0 + 1e-9), clip);
}

static void scene(const char *dir, const char *name, rs_params_t p, int secs, int strike_at)
{
    int n = RAIN_SR * secs;
    int16_t *buf = malloc((size_t)n * 2);
    rs_init(4321);
    for (int i = 0; i < n; i += 220) {
        if (strike_at >= 0 && i >= RAIN_SR * strike_at && p.strike == 0) p.strike = 1;
        rs_render(&p, buf + i, (n - i) < 220 ? n - i : 220);
    }
    level(name, buf, RAIN_SR * 2, n);
    if (strike_at >= 0) {   /* loudness in 1 s windows around the strike */
        printf("   1 s windows:");
        for (int s0 = strike_at - 1; s0 < secs; s0++) {
            double ss = 0; for (int i = s0 * RAIN_SR; i < (s0 + 1) * RAIN_SR; i++) ss += (double)buf[i] * buf[i];
            printf(" %.0f", 20 * log10(sqrt(ss / RAIN_SR) / 32768 + 1e-9));
        }
        printf("\n");
    }
    char path[256]; snprintf(path, sizeof path, "%s/%s.wav", dir, name); wav(path, buf, n);
    free(buf);
}

int main(int argc, char **argv)
{
    FILE *bf = fopen("assets/birds/birds.ulaw", "rb");
    if (bf) { fread(bird_data, 1, sizeof bird_data, bf); fclose(bf); } else fprintf(stderr, "no assets/birds/birds.ulaw: run from the repo root\n");
    const char *dir = argc > 1 ? argv[1] : ".";
    const char *names[4] = { "rain", "ocean", "noise", "wind" };
    int secs = 12, n = RAIN_SR * secs;
    int16_t *buf = malloc((size_t)n * 2);
    for (int k = 0; k < 4; k++) for (int l = 1; l <= 5; l += 2) {
        rs_init(1234);
        rs_params_t p = { .on = true, .kind = k, .level = l, .volume = 10, .fade = 1.0f, RS_DEFAULTS };
        for (int i = 0; i < n; i += 220) rs_render(&p, buf + i, (n - i) < 220 ? n - i : 220);
        double ss = 0; int pk = 0, clip = 0;
        for (int i = RAIN_SR * 3; i < n; i++) { ss += (double)buf[i] * buf[i]; int a = abs(buf[i]); if (a > pk) pk = a; if (a > 32000) clip++; }
        double rms = sqrt(ss / (n - RAIN_SR * 3));
        printf("%-5s L%d  rms %5.1f dBFS  peak %5.1f dBFS  clipped %d\n", names[k], l,
               20 * log10(rms / 32768 + 1e-9), 20 * log10(pk / 32768.0 + 1e-9), clip);
        char path[256]; snprintf(path, sizeof path, "%s/%s_l%d.wav", dir, names[k], l);
        wav(path, buf, n);
    }
    /* a recording (argv[2], an .adp from the web app), looped past its end */
    if (argc > 2) {
        FILE *rf = fopen(argv[2], "rb");
        static uint8_t rec[4 << 20]; size_t rl = rf ? fread(rec, 1, sizeof rec, rf) : 0;
        if (rf) fclose(rf);
        scene(dir, "recording", (rs_params_t){ .on = true, .kind = RS_RECORDING, .level = 3, .volume = 10, .fade = 1, RS_DEFAULTS, .rec = rec, .rec_len = (uint32_t)rl }, 30, -1);
    }
    /* transitions: rain L3 -> ocean -> off, to hear the glides */
    rs_init(99);
    for (int i = 0; i < n; i += 220) {
        rs_params_t p = { .on = i < n * 3 / 4, .kind = i < n / 2 ? 0 : 1, .level = 3, .volume = 10, .fade = 1.0f, RS_DEFAULTS };
        rs_render(&p, buf + i, (n - i) < 220 ? n - i : 220);
    }
    char path[256]; snprintf(path, sizeof path, "%s/transitions.wav", dir); wav(path, buf, n);

    const char *sn[4] = { "surface_open", "surface_leaves", "surface_roof", "surface_window" };
    for (int k = 0; k < 4; k++) scene(dir, sn[k], (rs_params_t){ .on = true, .kind = 0, .level = 3, .volume = 10, .fade = 1, RS_DEFAULTS, .surface = k }, 10, -1);
    scene(dir, "rain_windy", (rs_params_t){ .on = true, .kind = 0, .level = 3, .volume = 10, .fade = 1, RS_DEFAULTS, .wind = 90 }, 12, -1);
    scene(dir, "rain_dark_soft", (rs_params_t){ .on = true, .kind = 0, .level = 2, .volume = 10, .fade = 1, RS_DEFAULTS, .tone = 10, .patter = 30 }, 10, -1);
    scene(dir, "thunder_distant", (rs_params_t){ .on = true, .kind = 0, .level = 3, .volume = 10, .fade = 1, RS_DEFAULTS, .thunder_near = 10 }, 20, 4);
    scene(dir, "thunder_close", (rs_params_t){ .on = true, .kind = 0, .level = 3, .volume = 10, .fade = 1, RS_DEFAULTS, .thunder_near = 95 }, 20, 4);
    scene(dir, "storm", (rs_params_t){ .on = true, .kind = 0, .level = 4, .volume = 10, .fade = 1, RS_DEFAULTS, .wind = 60, .thunder = 100, .thunder_near = 60 }, 60, -1);

    /* the dawn chorus: 3 min build over rain, then 30 s full */
    {
        int secs = 210, n2 = RAIN_SR * secs;
        int16_t *b2 = malloc((size_t)n2 * 2);
        rs_init(777);
        for (int i = 0; i < n2; i += 220) {
            float t = (float)i / RAIN_SR;
            rs_params_t p = { .on = true, .kind = 0, .level = 3, .volume = 4, .fade = 1, RS_DEFAULTS };
            p.chorus = t < 180 ? 0.05f + 0.95f * t / 180.0f : 1.0f;
            rs_render(&p, b2 + i, (n2 - i) < 220 ? n2 - i : 220);
        }
        printf("chorus build, 15 s windows (dBFS):");
        for (int w = 0; w < secs / 15; w++) {
            double ss = 0; int pk = 0;
            for (int i = w * 15 * RAIN_SR; i < (w + 1) * 15 * RAIN_SR; i++) { ss += (double)b2[i] * b2[i]; if (abs(b2[i]) > pk) pk = abs(b2[i]); }
            printf(" %.0f/%.0f", 20 * log10(sqrt(ss / (15.0 * RAIN_SR)) / 32768 + 1e-9), 20 * log10(pk / 32768.0 + 1e-9));
        }
        printf("  (rms/peak)\n");
        char path[256]; snprintf(path, sizeof path, "%s/dawn_chorus.wav", dir); wav(path, b2, n2);
        free(b2);
    }
    /* each bird alone, two calls */
    for (int b = 0; b < BIRD_COUNT; b++) {
        int n2 = RAIN_SR * 16; int16_t *b2 = malloc((size_t)n2 * 2);
        rs_init(100 + b);
        rs_params_t p = { .on = false, .kind = 0, .level = 3, .volume = 6, .fade = 1, RS_DEFAULTS };
        p.audition_bird = b;
        for (int i = 0; i < n2; i += 220) {
            if (i / 220 == (RAIN_SR / 2) / 220 || i / 220 == (RAIN_SR * 8) / 220) p.audition++;
            rs_render(&p, b2 + i, (n2 - i) < 220 ? n2 - i : 220);
        }
        char nm[64]; snprintf(nm, sizeof nm, "bird_%s", bird_name(b));
        level(nm, b2, 0, n2);
        char path[256]; snprintf(path, sizeof path, "%s/%s.wav", dir, nm); wav(path, b2, n2);
        free(b2);
    }
    return 0;
}
