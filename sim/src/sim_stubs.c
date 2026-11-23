/*
 * sim_stubs.c: the parts of the board with nothing to simulate yet:
 *   power.h   the PowerBoost battery monitor: always on mains, 100 %
 *   psram.h   the 8 MB PSRAM: an array (see sim/override/psram.h)
 */
#include "sim.h"
#include "sim_sdk.h"
#include "power.h"
#include "psram.h"

/* ---- power ---- */
void  power_init(void) {}
bool  power_tick(void) { return false; }
bool  power_on_battery(void) { return false; }
bool  power_low(void) { return false; }
int   power_percent(void) { return 100; }
float power_battery_v(void) { return 4.15f; }
float power_usb_v(void) { return 5.05f; }
int   power_adc_pin_mv(int ch) { (void)ch; return 0; }

/* ---- PSRAM ---- */
uint8_t sim_psram[SIM_PSRAM_BYTES] __attribute__((aligned(16)));
size_t psram_init(uint cs) { (void)cs; return SIM_PSRAM_BYTES; }
bool psram_test(size_t n) { (void)n; return true; }
