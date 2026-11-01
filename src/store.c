/*
 * store.c: files in the flash after the firmware slots. See store.h.
 */
#include "store.h"

#include <stdio.h>
#include <string.h>
#ifdef STORE_HOST                         /* tests/: a simulated flash chip and boot ROM */
#include "store_host.h"
#else
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "boot/picobin.h"
#include "hardware/flash.h"
#include "config.h"
#define FLASH_RAW  ((const uint8_t *)XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE)
extern char __flash_binary_end;
#define STORE_IMAGE_END() ((uint32_t)(uintptr_t)&__flash_binary_end - XIP_BASE)
#endif
#define SECTOR     4096u
#define STORE_END  (15u * 1024 * 1024)       /* the settings start here (config.c) */
#define DIR_MAGIC  0x31524944u               /* "DIR1" */

typedef struct {
    uint32_t    magic, crc, seq, n;          /* crc over seq..e[] */
    store_ent_t e[STORE_FILES];
} store_dir_t;
_Static_assert(sizeof(store_dir_t) <= SECTOR, "directory fits a sector");

static uint32_t    s_base, s_data;           /* region start (directory), first data sector */
static store_dir_t s_dir;
static int         s_dir_slot;               /* which copy holds s_dir */
#define s_sect (config_sector_buf())

static struct { char name[STORE_NAME_MAX]; uint32_t start, total, done, crc; bool on; } s_up;

static uint32_t crc32_add(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--) { crc ^= *p++; for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1)); }
    return ~crc;
}
static uint32_t dir_crc(const store_dir_t *d) { return crc32_add(0, (const uint8_t *)&d->seq, sizeof *d - 8); }
static uint32_t round_up(uint32_t n) { return (n + SECTOR - 1) & ~(SECTOR - 1); }

bool store_ready(void) { return s_data != 0; }

void store_init(void)
{
    s_base = s_data = 0;                     /* until the region checks out */
    /* The partitions' furthest sector. PT_INFO: word 0 echoes the flags, word 1
     * holds the count (bits 0-7) and whether a table is present (bit 8). Then
     * one partition at a time: word 0 the flags, word 1 its location. */
    uint32_t pt[4] = { 0 };
    int n = rom_get_partition_table_info(pt, 4, PT_INFO_PT_INFO);
    if (n < 2 || !(pt[1] & 0x100)) { printf("store: no partition table; no file storage\n"); return; }
    int parts = (int)(pt[1] & 0xFF);
    uint32_t end = 0;
    for (int i = 0; i < parts; i++) {
        uint32_t w[3] = { 0 };
        if (rom_get_partition_table_info(w, 3, PT_INFO_PARTITION_LOCATION_AND_FLAGS | PT_INFO_SINGLE_PARTITION | ((uint32_t)i << 24)) < 2
            || !(w[0] & PT_INFO_PARTITION_LOCATION_AND_FLAGS)) { printf("store: partition %d unreadable\n", i); return; }
        uint32_t first = (w[1] & PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_BITS) >> PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB;
        uint32_t last  = (w[1] & PICOBIN_PARTITION_LOCATION_LAST_SECTOR_BITS) >> PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB;
        printf("store: partition %d at 0x%06lx-0x%06lx\n", i, (unsigned long)(first * SECTOR), (unsigned long)((last + 1) * SECTOR));
        if ((last + 1) * SECTOR > end) end = (last + 1) * SECTOR;
    }
    /* Never under the running firmware, whatever the table says. */
    uint32_t img_end = STORE_IMAGE_END();
    if (end < 1024u * 1024 || end < img_end) { printf("store: partitions end at 0x%06lx, implausible; no file storage\n", (unsigned long)end); return; }
    if (!end || end + 4 * SECTOR > STORE_END) { printf("store: no room after the partitions\n"); return; }
    s_base = end; s_data = end + 2 * SECTOR;

    const store_dir_t *d[2] = { (const store_dir_t *)(FLASH_RAW + s_base), (const store_dir_t *)(FLASH_RAW + s_base + SECTOR) };
    int best = -1;
    for (int i = 0; i < 2; i++) {
        if (d[i]->magic != DIR_MAGIC || d[i]->n > STORE_FILES || d[i]->crc != dir_crc(d[i])) continue;
        if (best < 0 || (int32_t)(d[i]->seq - d[best]->seq) > 0) best = i;
    }
    if (best >= 0) { s_dir = *d[best]; s_dir_slot = best; }
    else           { memset(&s_dir, 0, sizeof s_dir); s_dir.magic = DIR_MAGIC; s_dir_slot = 1; }
    printf("store: %lu KB at 0x%06lx, %lu file(s), %lu KB free\n", (unsigned long)(store_capacity() / 1024),
           (unsigned long)s_data, (unsigned long)s_dir.n, (unsigned long)((store_capacity() - store_used()) / 1024));
}

uint32_t store_capacity(void) { return s_data ? STORE_END - s_data : 0; }
uint32_t store_used(void)
{
    uint32_t u = 0;
    for (uint32_t i = 0; i < s_dir.n; i++) u += round_up(s_dir.e[i].size);
    return u;
}

int                store_count(void) { return (int)s_dir.n; }
const store_ent_t *store_get(int i) { return (i >= 0 && (uint32_t)i < s_dir.n) ? &s_dir.e[i] : NULL; }
const uint8_t     *store_data(const store_ent_t *e) { return FLASH_RAW + e->start; }
const store_ent_t *store_find(const char *name)
{
    for (uint32_t i = 0; i < s_dir.n; i++) if (!strcmp(s_dir.e[i].name, name)) return &s_dir.e[i];
    return NULL;
}

/* Write the directory to the other copy (the current one stays valid until this lands). */
static void dir_save(void)
{
    s_dir.seq++; s_dir.magic = DIR_MAGIC; s_dir.crc = dir_crc(&s_dir);
    int slot = s_dir_slot ^ 1;
    memset(s_sect, 0xFF, SECTOR);
    memcpy(s_sect, &s_dir, sizeof s_dir);
    config_flash_sector(s_base + (uint32_t)slot * SECTOR, s_sect, (sizeof s_dir + 255) & ~255u);
    s_dir_slot = slot;
}

/* First fit among the gaps between files (kept sorted by start); skip = a file to treat as gone. */
static uint32_t find_gap(uint32_t need, const store_ent_t *skip, uint32_t *largest)
{
    uint32_t at = s_data, found = 0, big = 0;
    for (uint32_t i = 0; i <= s_dir.n; i++) {
        const store_ent_t *e = i < s_dir.n ? &s_dir.e[i] : NULL;
        if (skip && e == skip) continue;
        uint32_t next = e ? e->start : STORE_END;
        uint32_t gap = next > at ? next - at : 0;
        if (gap > big) big = gap;
        if (!found && gap >= need) found = at;
        if (e) at = e->start + round_up(e->size);
    }
    if (largest) *largest = big;
    return found;
}
uint32_t store_largest_free(void) { uint32_t big = 0; if (s_data) find_gap(~0u, NULL, &big); return big; }

static bool name_ok(const char *n)
{
    size_t l = strlen(n);
    if (!l || l >= STORE_NAME_MAX) return false;
    for (; *n; n++) if (*n < 32 || *n == '/' || *n == '\\' || *n == '"') return false;
    return true;
}

int store_put(const char *name, uint32_t off, uint32_t total, const uint8_t *data, size_t len, char *err, size_t errcap)
{
#define FAIL(m) do { snprintf(err, errcap, "%s", m); s_up.on = false; return -1; } while (0)
    if (!s_data) FAIL("no file storage on this panel");
    if (off == 0) {
        if (!name_ok(name)) FAIL("that name won't do");
        if (!total) FAIL("empty file");
        if (s_dir.n >= STORE_FILES && !store_find(name)) FAIL("too many files");
        uint32_t big = 0;
        /* A file being replaced counts as free: the new copy lands first, then the directory swaps. */
        uint32_t at = find_gap(round_up(total), NULL, &big);
        if (!at) {
            const store_ent_t *old = store_find(name);
            if (old && find_gap(round_up(total), old, NULL)) FAIL("not enough room while the old copy is there; delete it first");
            FAIL("not enough room");
        }
        snprintf(s_up.name, sizeof s_up.name, "%s", name);
        s_up.start = at; s_up.total = total; s_up.done = 0; s_up.crc = 0; s_up.on = true;
        printf("store: receiving %s (%lu bytes) at 0x%06lx\n", name, (unsigned long)total, (unsigned long)at);
    } else if (!s_up.on || strcmp(name, s_up.name) || off != s_up.done || total != s_up.total) {
        FAIL("upload out of step; start again");
    }
    if (off % SECTOR) FAIL("chunks must be 4 KB multiples");
    if (off + len > total) FAIL("more data than the file size");
    for (size_t o = 0; o < len; o += SECTOR) {
        size_t n = len - o < SECTOR ? len - o : SECTOR;
        memset(s_sect, 0xFF, SECTOR);
        memcpy(s_sect, data + o, n);
        uint32_t at = s_up.start + off + (uint32_t)o;
        config_flash_sector(at, s_sect, (n + 255) & ~255u);
        if (memcmp(FLASH_RAW + at, s_sect, n)) FAIL("flash write did not read back");
    }
    s_up.crc = crc32_add(s_up.crc, data, len);
    s_up.done += (uint32_t)len;
    if (s_up.done < total) return 0;

    /* Complete: into the directory (replacing a same-named file), sorted by start. */
    store_ent_t ne = { .start = s_up.start, .size = total, .crc = s_up.crc };
    snprintf(ne.name, sizeof ne.name, "%s", s_up.name);
    uint32_t w = 0;
    for (uint32_t i = 0; i < s_dir.n; i++) if (strcmp(s_dir.e[i].name, ne.name)) s_dir.e[w++] = s_dir.e[i];
    s_dir.n = w;
    uint32_t pos = 0;
    while (pos < s_dir.n && s_dir.e[pos].start < ne.start) pos++;
    memmove(&s_dir.e[pos + 1], &s_dir.e[pos], (s_dir.n - pos) * sizeof ne);
    s_dir.e[pos] = ne; s_dir.n++;
    dir_save();
    s_up.on = false;
    printf("store: saved %s (%lu bytes, crc %08lx)\n", ne.name, (unsigned long)total, (unsigned long)ne.crc);
    return 1;
#undef FAIL
}

bool store_delete(const char *name)
{
    uint32_t w = 0;
    for (uint32_t i = 0; i < s_dir.n; i++) if (strcmp(s_dir.e[i].name, name)) s_dir.e[w++] = s_dir.e[i];
    if (w == s_dir.n) return false;
    s_dir.n = w;
    dir_save();
    printf("store: deleted %s\n", name);
    return true;
}

size_t store_json(char *out, size_t cap)
{
    size_t n = (size_t)snprintf(out, cap, "{\"capacity\":%lu,\"used\":%lu,\"largest_free\":%lu,\"max_files\":%d,\"files\":[",
                                (unsigned long)store_capacity(), (unsigned long)store_used(), (unsigned long)store_largest_free(), STORE_FILES);
    for (uint32_t i = 0; i < s_dir.n && n < cap - 96; i++)
        n += (size_t)snprintf(out + n, cap - n, "%s{\"name\":\"%s\",\"size\":%lu,\"crc\":\"%08lx\"}", i ? "," : "",
                              s_dir.e[i].name, (unsigned long)s_dir.e[i].size, (unsigned long)s_dir.e[i].crc);
    if (n < cap - 3) n += (size_t)snprintf(out + n, cap - n, "]}");
    return n;
}
