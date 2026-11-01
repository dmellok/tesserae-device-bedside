/*
 * store_host.c: the simulated flash and boot ROM for test_store. See store_host.h.
 */
#include "store_host.h"
#include <string.h>

uint8_t  g_flash[16 * 1024 * 1024];
uint32_t g_image_end = 3u * 1024 * 1024;
int      g_sector_writes;

static int      s_np;
static uint32_t s_first[8], s_last[8];

void host_set_partitions(int n, const uint32_t first[], const uint32_t last[])
{
    s_np = n;
    for (int i = 0; i < n; i++) { s_first[i] = first[i]; s_last[i] = last[i]; }
}

int rom_get_partition_table_info(uint32_t *out, uint32_t words, uint32_t flags)
{
    if (flags == PT_INFO_PT_INFO) {
        if (words < 2) return -1;
        out[0] = PT_INFO_PT_INFO;
        out[1] = (uint32_t)s_np | (s_np ? 0x100u : 0);
        return 2;
    }
    if (flags & PT_INFO_SINGLE_PARTITION) {
        int i = (int)(flags >> 24);
        if (i >= s_np || words < 3) return -1;
        out[0] = PT_INFO_PARTITION_LOCATION_AND_FLAGS | PT_INFO_SINGLE_PARTITION;
        out[1] = (s_first[i] << PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB) | (s_last[i] << PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB);
        out[2] = 0;
        return 3;
    }
    return -1;
}

void config_flash_sector(uint32_t off, const uint8_t *data, size_t len)
{
    memset(g_flash + (off & ~4095u), 0xFF, 4096);      /* erase the sector, then program */
    memcpy(g_flash + off, data, len);
    g_sector_writes++;
}

static uint8_t s_stage[4096];
uint8_t *config_sector_buf(void) { return s_stage; }
