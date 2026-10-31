/*
 * ota.c: firmware updates with the RP2350 boot ROM's A/B partitions.
 *
 * Flash holds a partition table (tools/pt.json): A and B, 7 MB each, linked as
 * an A/B pair; the config at 15 MB sits outside both. The image is built
 * "try before you buy" (PICO_CRT0_IMAGE_TYPE_TBYB), after Raspberry Pi's
 * pico_w/wifi/ota_update example:
 *   1. the boot ROM names the slot a new image goes in (not the running one);
 *   2. the image comes down in 1 MB ranges into PSRAM and is written to that
 *      slot one 4 KB sector at a time (copied to SRAM first: flash and PSRAM
 *      share the QMI, which is off while flash is written);
 *   3. the slot is read back and its SHA-256 checked against the release;
 *   4. a flash-update reboot runs the new image on trial. If it never calls
 *      rom_explicit_buy() (ota_boot_check, once the panel is up), the boot ROM
 *      goes back to the old slot.
 * After a buy the other slot is invalidated, so whichever image was chosen,
 * newer or older, is the one that boots from then on.
 */
#include "ota.h"
#include "sha256.h"
#include "net_rest.h"
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "boot/bootrom_constants.h"
#include "boot/picobin.h"
#include "boot/uf2.h"
#include "boot/picoboot_constants.h"
#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"

#define FLASH_RAW   XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE
#define SECTOR      4096u

static uint8_t   s_work[4096] __attribute__((aligned(4)));   /* boot ROM workarea (3264 was too small for a buy: error -13) */
static uint8_t   s_sect[SECTOR] __attribute__((aligned(4))); /* one sector in SRAM */
static uint8_t  *s_buf;
static size_t    s_cap;

static ota_state_t s_state;
static char      s_url[2048], s_err[80], s_ver[24];
static uint8_t   s_want[32];
static uint32_t  s_size, s_done, s_slot_start, s_slot_size;
static int       s_boot_part = -1;
static bool      s_parted;

void ota_init(uint8_t *buf, size_t cap) { s_buf = buf; s_cap = cap; }

static void fail(const char *why)
{
    snprintf(s_err, sizeof s_err, "%s", why);
    printf("ota: failed: %s\n", why);
    s_state = OTA_FAILED;
}

/* The slot that is not running (where an update goes): its number, and where it is. */
static int spare_slot(uint32_t *start, uint32_t *size)
{
    resident_partition_t tp;
    int part = rom_get_uf2_target_partition(s_work, sizeof s_work, RP2350_ARM_S_FAMILY_ID, &tp);
    if (part < 0) return part;
    uint32_t first = (tp.permissions_and_location & PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_BITS) >> PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB;
    uint32_t last  = (tp.permissions_and_location & PICOBIN_PARTITION_LOCATION_LAST_SECTOR_BITS) >> PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB;
    *start = first * SECTOR; *size = (last + 1 - first) * SECTOR;
    return part;
}

/* The buy writes to flash through the boot ROM. Called before core 1 starts
 * (core 1 runs from flash and died under it). The QMI windows (PSRAM on
 * window 1) and XIP_CTRL are kept across the call as a precaution; from RAM. */
static int __not_in_flash_func(buy_keeping_qmi)(void)
{
    uint32_t t[2], rf[2], rc[2], wf[2], wc[2];
    for (int i = 0; i < 2; i++) { t[i] = qmi_hw->m[i].timing; rf[i] = qmi_hw->m[i].rfmt; rc[i] = qmi_hw->m[i].rcmd; wf[i] = qmi_hw->m[i].wfmt; wc[i] = qmi_hw->m[i].wcmd; }
    uint32_t xc = xip_ctrl_hw->ctrl;
    int r = rom_explicit_buy(s_work, sizeof s_work);
    for (int i = 0; i < 2; i++) { qmi_hw->m[i].timing = t[i]; qmi_hw->m[i].rfmt = rf[i]; qmi_hw->m[i].rcmd = rc[i]; qmi_hw->m[i].wfmt = wf[i]; qmi_hw->m[i].wcmd = wc[i]; }
    xip_ctrl_hw->ctrl = xc;
    __asm volatile("dsb; isb" ::: "memory");
    return r;
}

void ota_boot_check(void)
{
    /* PT_INFO_PT_INFO: word 0 echoes the flags, word 1 is the partition count
     * (bits 0-7) and whether a table is present (bit 8). */
    uint32_t pt[4] = { 0 };
    int n = rom_get_partition_table_info(pt, 4, PT_INFO_PT_INFO);
    s_parted = n >= 2 && (pt[1] & 0x100) && (pt[1] & 0xFF) >= 2;
    if (!s_parted) printf("ota: partition info %d: %08lx %08lx\n", n, (unsigned long)pt[0], (unsigned long)pt[1]);
    boot_info_t bi = { 0 };
    if (rom_get_boot_info(&bi)) s_boot_part = bi.partition;
    if (!s_parted || s_boot_part < 0 || s_boot_part > 1) {
        printf("ota: no A/B partition table; updates need tools/partition.sh once over USB\n");
        s_boot_part = -1; return;
    }
    bool update = rom_get_last_boot_type() == BOOT_TYPE_FLASH_UPDATE;
    printf("ota: running from slot %c%s\n", 'A' + s_boot_part, update ? " (just updated)" : "");
    if (update) {
        int r = buy_keeping_qmi();
        if (r != 0) {
            /* Not committed: leave the old slot alone so the boot ROM can still go back. */
            printf("ota: could not commit the new image (%d); the previous one stays as a fallback\n", r);
            return;
        }
        printf("ota: new image committed\n");
    }
    /* Only the running image may boot: invalidate the other slot if it holds one.
     * The slot an update would go to is exactly the other one. */
    uint32_t start, size;
    int other = spare_slot(&start, &size);
    if (other < 0 || other == s_boot_part) { printf("ota: no other slot to clear (%d)\n", other); return; }
    if (*(const volatile uint32_t *)(FLASH_RAW + start) != 0xFFFFFFFFu) {
        memset(s_sect, 0xFF, sizeof s_sect);
        config_flash_sector(start, s_sect, 256);
        printf("ota: slot %c cleared, only this image boots now\n", 'A' + other);
    }
}

static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

bool ota_start(const char *url, uint32_t size, const char *sha_hex, const char *version)
{
    if (ota_busy()) return false;
    s_err[0] = 0;
    if (!s_parted) { fail("no A/B partitions: run tools/partition.sh once"); return false; }
    if (!s_buf) { fail("no PSRAM for the download"); return false; }
    if (!sha_hex || strlen(sha_hex) != 64) { fail("bad sha256"); return false; }
    for (int i = 0; i < 32; i++) {
        int a = hexval(sha_hex[2 * i]), b = hexval(sha_hex[2 * i + 1]);
        if (a < 0 || b < 0) { fail("bad sha256"); return false; }
        s_want[i] = (uint8_t)(a << 4 | b);
    }
    int part = spare_slot(&s_slot_start, &s_slot_size);
    if (part < 0 || part == s_boot_part) { fail("no spare slot"); return false; }
    if (size == 0 || size > s_slot_size) { fail("image does not fit the slot"); return false; }
    snprintf(s_url, sizeof s_url, "%s", url);
    snprintf(s_ver, sizeof s_ver, "%s", version ? version : "");
    s_size = size; s_done = 0;
    s_state = OTA_DOWNLOADING;
    printf("ota: %s (%lu bytes) into slot %c at 0x%06lx\n", s_ver[0] ? s_ver : "image", (unsigned long)size, 'A' + part, (unsigned long)s_slot_start);
    return true;
}

static void verify_and_reboot(void)
{
    s_state = OTA_VERIFYING;
    sha256_t h; sha256_init(&h);
    const uint8_t *img = (const uint8_t *)(FLASH_RAW + s_slot_start);
    for (uint32_t o = 0; o < s_size; o += SECTOR) {
        uint32_t n = s_size - o < SECTOR ? s_size - o : SECTOR;
        memcpy(s_sect, img + o, n);          /* uncached window: read in blocks */
        sha256_update(&h, s_sect, n);
    }
    uint8_t got[32]; sha256_final(&h, got);
    if (memcmp(got, s_want, 32)) {
        memset(s_sect, 0xFF, sizeof s_sect);
        config_flash_sector(s_slot_start, s_sect, 256);     /* never boot a bad image */
        fail("checksum mismatch after writing"); return;
    }
    printf("ota: written and verified; restarting into the new firmware\n");
    s_state = OTA_REBOOTING;
    sleep_ms(300);
    rom_reboot(REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE, 500, XIP_BASE + s_slot_start, 0);
    while (true) tight_loop_contents();
}

void ota_tick(void)
{
    if (s_state != OTA_DOWNLOADING) return;
    uint32_t want = s_size - s_done; if (want > s_cap) want = (uint32_t)s_cap;
    size_t got = 0; uint32_t total = 0;
    if (!rest_download_range(s_url, sizeof s_url, s_done, want, s_buf, &got, &total, 60000)) { fail("download failed"); return; }
    if (total && total != s_size) { fail("size differs from the release"); return; }
    if (got != want) { fail("short download"); return; }
    for (uint32_t o = 0; o < got; o += SECTOR) {
        uint32_t n = got - o < SECTOR ? got - o : SECTOR;
        memset(s_sect, 0xFF, SECTOR);
        memcpy(s_sect, s_buf + o, n);
        config_flash_sector(s_slot_start + s_done + o, s_sect, (n + 255) & ~255u);
    }
    s_done += got;
    printf("ota: %lu of %lu bytes (%d%%)\n", (unsigned long)s_done, (unsigned long)s_size, ota_percent());
    if (s_done >= s_size) verify_and_reboot();
}

bool ota_busy(void) { return s_state == OTA_DOWNLOADING || s_state == OTA_VERIFYING || s_state == OTA_REBOOTING; }
ota_state_t ota_state(void) { return s_state; }
const char *ota_state_name(void)
{
    static const char *const N[] = { "idle", "downloading", "verifying", "restarting", "failed" };
    return N[s_state];
}
int  ota_percent(void) { return s_size ? (int)((uint64_t)s_done * 100 / s_size) : 0; }
const char *ota_error(void) { return s_err; }
const char *ota_target_version(void) { return s_ver; }
bool ota_partitioned(void) { return s_parted; }
int  ota_boot_partition(void) { return s_boot_part; }
