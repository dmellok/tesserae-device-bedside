/*
 * test_store.c: the file store (src/store.c) on a simulated flash chip:
 * uploads, read-back, replace, delete, first fit, surviving a restart and a
 * torn directory write, and never touching the firmware slots.
 */
#include "test.h"
#include "store.h"
#include "host/store_host.h"
#include <stdlib.h>

#define MB (1024u * 1024u)
/* The bench table: A at 8 KB, B after it, 5.5 MB each (tools/pt.json). */
static const uint32_t FIRST[2] = { 2, 1410 }, LAST[2] = { 1409, 2817 };
static const uint32_t SLOTS_END = 2818u * 4096u;                     /* 0xB02000 */

static void fresh(void)
{
    memset(g_flash, 0xFF, sizeof g_flash);
    host_set_partitions(2, FIRST, LAST);
    g_image_end = 3 * MB;
    store_init();
}

static uint32_t crc32_ref(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & -(c & 1)); }
    return ~c;
}

static void fill(uint8_t *b, size_t n, unsigned seed)
{
    for (size_t i = 0; i < n; i++) { seed = seed * 1103515245u + 12345u; b[i] = (uint8_t)(seed >> 16); }
}

/* Upload in the web app's 32 KB chunks; returns the last store_put result. */
static int upload(const char *name, const uint8_t *data, uint32_t len, char *err)
{
    int r = -1;
    for (uint32_t off = 0; off < len; off += 32768) {
        uint32_t n = len - off < 32768 ? len - off : 32768;
        r = store_put(name, off, len, data + off, n, err, 96);
        if (r < 0) return r;
    }
    return r;
}

static void finds_the_region_after_the_slots(void)
{
    fresh();
    CHECK(store_ready());
    CHECK_EQ(store_capacity(), 15 * MB - (SLOTS_END + 8192));       /* two directory sectors first */
    CHECK_EQ(store_used(), 0);
    CHECK_EQ(store_count(), 0);
}

static void no_partition_table_no_store(void)
{
    memset(g_flash, 0xFF, sizeof g_flash);
    host_set_partitions(0, NULL, NULL);
    store_init();
    CHECK(!store_ready());
    char err[96] = "";
    uint8_t b[16] = { 0 };
    CHECK_EQ(store_put("x", 0, 16, b, 16, err, sizeof err), -1);
    CHECK(strstr(err, "no file storage") != NULL);
}

static void refuses_a_table_inside_the_running_image(void)
{
    memset(g_flash, 0xFF, sizeof g_flash);
    uint32_t f[1] = { 2 }, l[1] = { 100 };                          /* ends at 404 KB */
    host_set_partitions(1, f, l);
    g_image_end = 3 * MB;                                            /* the image runs past it */
    store_init();
    CHECK(!store_ready());
}

static void upload_and_read_back(void)
{
    fresh();
    static uint8_t data[100000];
    fill(data, sizeof data, 7);
    char err[96] = "";
    CHECK_EQ(upload("rain.adp", data, sizeof data, err), 1);
    CHECK_EQ(store_count(), 1);
    const store_ent_t *e = store_find("rain.adp");
    CHECK(e != NULL);
    if (!e) return;
    CHECK_EQ(e->size, sizeof data);
    CHECK_EQ(e->crc, crc32_ref(data, sizeof data));
    CHECK(memcmp(store_data(e), data, sizeof data) == 0);
    CHECK_EQ(store_used(), 102400);                                  /* whole 4 KB sectors */
    CHECK(e->start >= SLOTS_END + 8192);
}

static void listed_only_when_complete(void)
{
    fresh();
    static uint8_t data[70000];
    fill(data, sizeof data, 3);
    char err[96] = "";
    CHECK_EQ(store_put("half.bin", 0, sizeof data, data, 32768, err, sizeof err), 0);
    CHECK_EQ(store_count(), 0);                                      /* not until the last chunk */
    store_init();                                                    /* power cut mid-upload */
    CHECK_EQ(store_count(), 0);
    CHECK_EQ(store_used(), 0);
}

static void out_of_step_chunks_are_refused(void)
{
    fresh();
    static uint8_t data[70000];
    fill(data, sizeof data, 4);
    char err[96] = "";
    CHECK_EQ(store_put("a.bin", 0, sizeof data, data, 32768, err, sizeof err), 0);
    CHECK_EQ(store_put("a.bin", 65536, sizeof data, data + 65536, 4464, err, sizeof err), -1);   /* skipped a chunk */
    CHECK(strstr(err, "out of step") != NULL);
    CHECK_EQ(store_put("a.bin", 32768, sizeof data, data + 32768, 32768, err, sizeof err), -1);  /* the upload was dropped */
    CHECK_EQ(upload("a.bin", data, sizeof data, err), 1);           /* starting again works */
    CHECK_EQ(store_count(), 1);
}

static void survives_a_restart(void)
{
    fresh();
    static uint8_t a[20000], b[5000];
    fill(a, sizeof a, 1); fill(b, sizeof b, 2);
    char err[96] = "";
    upload("one", a, sizeof a, err);
    upload("two", b, sizeof b, err);
    store_init();                                                    /* reboot */
    CHECK_EQ(store_count(), 2);
    const store_ent_t *e = store_find("two");
    CHECK(e && e->size == sizeof b && memcmp(store_data(e), b, sizeof b) == 0);
    e = store_find("one");
    CHECK(e && memcmp(store_data(e), a, sizeof a) == 0);
}

static void replacing_keeps_one_copy(void)
{
    fresh();
    static uint8_t a[40000], b[9000];
    fill(a, sizeof a, 5); fill(b, sizeof b, 6);
    char err[96] = "";
    upload("loop.adp", a, sizeof a, err);
    CHECK_EQ(upload("loop.adp", b, sizeof b, err), 1);
    CHECK_EQ(store_count(), 1);
    const store_ent_t *e = store_find("loop.adp");
    CHECK(e && e->size == sizeof b && memcmp(store_data(e), b, sizeof b) == 0);
    CHECK_EQ(store_used(), 12288);                                   /* the old copy's space is free */
}

static void delete_frees_space(void)
{
    fresh();
    static uint8_t a[10000];
    fill(a, sizeof a, 8);
    char err[96] = "";
    upload("x", a, sizeof a, err);
    CHECK(store_delete("x"));
    CHECK_EQ(store_count(), 0);
    CHECK_EQ(store_used(), 0);
    CHECK(!store_delete("x"));
    store_init();
    CHECK_EQ(store_count(), 0);                                      /* the delete was saved */
}

static void first_fit_reuses_a_gap(void)
{
    fresh();
    static uint8_t a[8192], b[8192], c[4096];
    fill(a, sizeof a, 9); fill(b, sizeof b, 10); fill(c, sizeof c, 11);
    char err[96] = "";
    upload("a", a, sizeof a, err);
    uint32_t a_start = store_find("a")->start;
    upload("b", b, sizeof b, err);
    store_delete("a");
    upload("c", c, sizeof c, err);
    CHECK_EQ(store_find("c")->start, a_start);
    CHECK(memcmp(store_data(store_find("b")), b, sizeof b) == 0);   /* its neighbour untouched */
}

static void torn_directory_write_falls_back(void)
{
    fresh();
    static uint8_t a[4096], b[4096];
    fill(a, sizeof a, 12); fill(b, sizeof b, 13);
    char err[96] = "";
    upload("first", a, sizeof a, err);                               /* directory copy 1 */
    upload("second", b, sizeof b, err);                              /* directory copy 2 (the newer) */
    /* A power cut while the newer copy was written: damage it. */
    uint32_t base = SLOTS_END;
    int newer = -1;
    uint32_t seq[2];
    for (int i = 0; i < 2; i++) memcpy(&seq[i], g_flash + base + (uint32_t)i * 4096 + 8, 4);
    newer = seq[1] > seq[0] ? 1 : 0;
    g_flash[base + (uint32_t)newer * 4096 + 40] ^= 0x5A;
    store_init();
    CHECK_EQ(store_count(), 1);                                      /* the state before "second" */
    CHECK(store_find("first") != NULL);
    CHECK(store_find("second") == NULL);
}

static void refuses_what_will_not_fit(void)
{
    fresh();
    char err[96] = "";
    uint8_t b[16] = { 0 };
    CHECK_EQ(store_put("huge", 0, store_capacity() + 4096, b, 16, err, sizeof err), -1);
    CHECK(strstr(err, "not enough room") != NULL);
    CHECK_EQ(store_put("", 0, 16, b, 16, err, sizeof err), -1);       /* names */
    CHECK_EQ(store_put("a/b", 0, 16, b, 16, err, sizeof err), -1);
    CHECK_EQ(store_put("0123456789012345678901234567890123456789", 0, 16, b, 16, err, sizeof err), -1);
    CHECK_EQ(store_put("empty", 0, 0, b, 0, err, sizeof err), -1);
}

static void at_most_24_files(void)
{
    fresh();
    char err[96] = "", name[16];
    uint8_t b[64] = { 1 };
    for (int i = 0; i < STORE_FILES; i++) { snprintf(name, sizeof name, "f%02d", i); CHECK_EQ(store_put(name, 0, 64, b, 64, err, sizeof err), 1); }
    CHECK_EQ(store_put("one-more", 0, 64, b, 64, err, sizeof err), -1);
    CHECK(strstr(err, "too many") != NULL);
    CHECK_EQ(store_put("f03", 0, 64, b, 64, err, sizeof err), 1);    /* replacing one is still fine */
}

static void never_writes_into_the_firmware_slots(void)
{
    fresh();
    /* Snapshot the slot area, run a busy session, check it's untouched. */
    for (uint32_t i = 0; i < SLOTS_END; i += 4096) g_flash[i] = 0x42;
    static uint8_t a[50000];
    fill(a, sizeof a, 14);
    char err[96] = "";
    upload("a", a, sizeof a, err); upload("b", a, 20000, err); store_delete("a"); upload("c", a, 30000, err);
    int touched = 0;
    for (uint32_t i = 0; i < SLOTS_END; i += 4096) if (g_flash[i] != 0x42) touched++;
    CHECK_EQ(touched, 0);
}

int main(void)
{
    RUN(finds_the_region_after_the_slots);
    RUN(no_partition_table_no_store);
    RUN(refuses_a_table_inside_the_running_image);
    RUN(upload_and_read_back);
    RUN(listed_only_when_complete);
    RUN(out_of_step_chunks_are_refused);
    RUN(survives_a_restart);
    RUN(replacing_keeps_one_copy);
    RUN(delete_frees_space);
    RUN(first_fit_reuses_a_gap);
    RUN(torn_directory_write_falls_back);
    RUN(refuses_what_will_not_fit);
    RUN(at_most_24_files);
    RUN(never_writes_into_the_firmware_slots);
    return test_summary("store");
}
