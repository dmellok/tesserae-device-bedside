/*
 * store_host.h: what src/store.c needs from the Pico, simulated for the host
 * tests: a 16 MB flash chip in RAM, the boot ROM's partition table query, and
 * config.c's sector write (erase 4 KB, then program).
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

extern uint8_t g_flash[16 * 1024 * 1024];
#define FLASH_RAW ((const uint8_t *)g_flash)

#define PT_INFO_PT_INFO                       0x0001
#define PT_INFO_PARTITION_LOCATION_AND_FLAGS  0x0010
#define PT_INFO_SINGLE_PARTITION              0x8000
#define PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB  0
#define PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_BITS 0x00001fffu
#define PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB   13
#define PICOBIN_PARTITION_LOCATION_LAST_SECTOR_BITS  0x03ffe000u

int rom_get_partition_table_info(uint32_t *out, uint32_t words, uint32_t flags);

void     config_flash_sector(uint32_t off, const uint8_t *data, size_t len);
uint8_t *config_sector_buf(void);

/* The test sets the running image's end and the partition table. */
extern uint32_t g_image_end;
#define STORE_IMAGE_END() (g_image_end)
void host_set_partitions(int n, const uint32_t first_sector[], const uint32_t last_sector[]);
extern int g_sector_writes;
