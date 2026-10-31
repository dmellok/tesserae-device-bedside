/*
 * store.h: files in the flash after the firmware slots (to the settings at
 * 15 MB), so they survive updates and never ride along in an OTA download.
 *
 * Layout: two directory sectors (written alternately, the newer valid one
 * wins, so a power cut mid-save loses at most the change being saved), then
 * the files, each in whole 4 KB sectors, first fit. A file appears in the
 * directory only once it is completely written and read back, so an
 * interrupted upload leaves nothing behind.
 *
 * Uploads arrive in chunks, in order (POST /api/file?name=&off=&total=).
 * Writing parks core 1 for each sector (~50 ms): the sound may stutter.
 *
 * Sound files made by the web app (it converts any audio the browser can
 * decode): "ADP1" header {magic, sample rate, samples, block bytes}, then IMA
 * ADPCM mono in blocks of `block bytes` (1024). Each block: the first sample
 * (int16 LE, also the predictor), the step index (u8), a zero byte, then
 * 2 * (block bytes - 4) more samples, two a byte, low nibble first. Any block
 * is a starting point. The web app levels the sound and blends its last 2 s
 * into the start, so playing it end to end loops without a seam.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STORE_NAME_MAX 40
#define STORE_FILES    24

typedef struct {
    char     name[STORE_NAME_MAX];   /* NUL-terminated */
    uint32_t start;                  /* flash offset */
    uint32_t size;
    uint32_t crc;                    /* CRC-32 of the contents */
    uint32_t rsv;
} store_ent_t;

/* Find the region (after the partitions) and read the directory. Before core 1. */
void store_init(void);
bool store_ready(void);

uint32_t store_capacity(void);       /* bytes for files */
uint32_t store_used(void);           /* whole sectors in use */
uint32_t store_largest_free(void);   /* the biggest file that fits now */

int                store_count(void);
const store_ent_t *store_get(int i);
const store_ent_t *store_find(const char *name);
const uint8_t     *store_data(const store_ent_t *e);   /* read straight from flash */

/* One chunk of an upload; off must follow on from the last (0 starts one).
 * Returns 1 when the file is complete, 0 for more please, -1 on error (err). */
int  store_put(const char *name, uint32_t off, uint32_t total, const uint8_t *data, size_t len, char *err, size_t errcap);
bool store_delete(const char *name);

/* {"capacity":..,"used":..,"free":..,"files":[{"name":..,"size":..}]} */
size_t store_json(char *out, size_t cap);
