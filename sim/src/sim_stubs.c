/*
 * sim_stubs.c: the parts of the board with nothing to simulate yet:
 *   ota.h     over-the-air updates (the boot ROM's A/B slots): the simulator
 *             can't install an image, and GitHub needs https
 *   power.h   the PowerBoost battery monitor: always on mains, 100 %
 *   psram.h   the 8 MB PSRAM: an array (see sim/override/psram.h)
 */
#include "sim.h"
#include "sim_sdk.h"
#include "ota.h"
#include "power.h"
#include "psram.h"

/* ---- OTA ---- */
static const char *s_running = "";
static char s_err[96];
void ota_init(uint8_t *buf, size_t cap) { (void)buf; (void)cap; }
void ota_boot_check(void) { printf("ota: running %s in the simulator (slot A)\n", s_running); }
void ota_set_version(const char *v) { s_running = v ? v : ""; }
bool ota_start(const char *url, uint32_t size, const char *sha, const char *ver)
{
    (void)url; (void)size; (void)sha; (void)ver;
    snprintf(s_err, sizeof s_err, "Updates can't be installed in the simulator");
    printf("ota: %s\n", s_err);
    return false;
}
void ota_tick(void) {}
bool ota_busy(void) { return false; }
ota_state_t ota_state(void) { return s_err[0] ? OTA_FAILED : OTA_IDLE; }
const char *ota_state_name(void) { return s_err[0] ? "failed" : "idle"; }
int  ota_percent(void) { return 0; }
const char *ota_error(void) { return s_err; }
const char *ota_target_version(void) { return ""; }
bool ota_partitioned(void) { return true; }
int  ota_boot_partition(void) { return 0; }
bool ota_releases_fetch(void)
{
    snprintf(s_err, sizeof s_err, "The release list needs https (GitHub), which the simulator doesn't do");
    printf("ota: %s\n", s_err);
    return false;
}
int ota_release_count(void) { return 0; }
const ota_release_t *ota_release(int i) { (void)i; return NULL; }
uint32_t ota_checked_ms(void) { return 0; }
bool ota_install(int i) { (void)i; return ota_start(NULL, 0, NULL, NULL); }
bool ota_auto_update(void) { return false; }

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
