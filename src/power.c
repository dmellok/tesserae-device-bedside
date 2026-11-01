/*
 * power.c: mains and battery from the PowerBoost's sense lines. See power.h.
 */
#include "power.h"

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/adc.h"

#define PIN_BAT   26
#define PIN_USB   27
#define PIN_LBO   28
#define DIVIDER   2.0f                 /* 100 k / 100 k */
#define MAINS_V   3.8f                 /* USB-C input above this: on mains (it sits at ~5 V or 0 V) */

static float    s_bat = -1, s_usb = -1, s_lbo = -1;   /* smoothed volts at the source */
static bool     s_on_batt, s_low;
static int      s_pct = -1;
static uint32_t s_last_ms;

void power_init(void)
{
    adc_init();
    adc_gpio_init(PIN_BAT); adc_gpio_init(PIN_USB); adc_gpio_init(PIN_LBO);
}

static float read_v(int ch)
{
    adc_select_input((uint)ch);
    uint32_t sum = 0;
    for (int i = 0; i < 16; i++) sum += adc_read();
    return (float)sum / 16.0f * 3.3f / 4095.0f * DIVIDER;
}

/* Resting LiPo voltage to charge, roughly; good to ~10 %, which is what a badge needs. */
static int lipo_percent(float v)
{
    static const float V[] = { 3.30f, 3.50f, 3.61f, 3.69f, 3.75f, 3.79f, 3.84f, 3.90f, 3.98f, 4.06f, 4.15f };
    if (v <= V[0]) return 0;
    for (int i = 1; i < 11; i++)
        if (v < V[i]) return (i - 1) * 10 + (int)(10.0f * (v - V[i - 1]) / (V[i] - V[i - 1]));
    return 100;
}

bool power_tick(void)
{
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (s_last_ms && now - s_last_ms < 1000) return false;
    s_last_ms = now;
    float b = read_v(0), u = read_v(1), l = read_v(2);
    /* Smooth the battery (a refresh or the amp sags it); mains changes take effect at once. */
    s_bat = s_bat < 0 ? b : s_bat + (b - s_bat) * 0.2f;
    s_usb = u; s_lbo = l;
    bool on_batt = s_usb < MAINS_V;
    int  pct = lipo_percent(s_bat);
    /* Low: the PowerBoost's LBO line (once it has shown a real battery voltage,
     * so an unwired, floating pin can't raise a false alarm), or the cell itself. */
    static bool lbo_wired;
    if (s_lbo > 2.8f) lbo_wired = true;
    bool low = on_batt && (s_bat < 3.4f || (lbo_wired && s_lbo < 1.0f));
    /* Hysteresis on the percentage so the badge doesn't flicker between two values. */
    if (s_pct < 0 || pct >= s_pct + 2 || pct <= s_pct - 2 || on_batt != s_on_batt) s_pct = pct;
    bool changed = on_batt != s_on_batt || low != s_low;
    static int reported = -1;
    if (s_pct != reported) { changed = true; reported = s_pct; }
    if (on_batt != s_on_batt) printf("power: %s (battery %d mV, %d%%)\n", on_batt ? "on battery" : "mains back", (int)(s_bat * 1000), s_pct);
    s_on_batt = on_batt; s_low = low;
    return changed;
}

bool  power_on_battery(void) { return s_on_batt; }
bool  power_low(void)        { return s_low; }
int   power_percent(void)    { return s_pct < 0 ? 0 : s_pct; }
float power_battery_v(void)  { return s_bat < 0 ? 0 : s_bat; }
float power_usb_v(void)      { return s_usb < 0 ? 0 : s_usb; }
