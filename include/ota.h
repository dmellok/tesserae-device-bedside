/*
 * ota.h: firmware updates into the spare A/B partition (see ota.c).
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { OTA_IDLE, OTA_DOWNLOADING, OTA_VERIFYING, OTA_REBOOTING, OTA_FAILED } ota_state_t;

/* buf: PSRAM scratch for one download chunk (cap bytes, e.g. 1 MB). */
void ota_init(uint8_t *buf, size_t cap);
/* Once the panel is up after boot: commit a trial image (try before you buy)
 * and invalidate the other slot, so only the running image can boot. */
void ota_boot_check(void);
/* Download url (size bytes, sha256 as 64 hex chars) into the spare slot,
 * check it and reboot into it. Runs from ota_tick(). */
bool ota_start(const char *url, uint32_t size, const char *sha_hex, const char *version);
void ota_tick(void);
bool ota_busy(void);

ota_state_t ota_state(void);
const char *ota_state_name(void);
int         ota_percent(void);
const char *ota_error(void);
const char *ota_target_version(void);
bool        ota_partitioned(void);    /* A/B partition table present */
int         ota_boot_partition(void); /* 0 = A, 1 = B, -1 = not partitioned */
