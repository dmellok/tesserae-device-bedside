/*
 * config.c: flash-backed device configuration. See config.h.
 *
 * Layout: magic + version + config_t + crc32 in one sector at CONFIG_FLASH_OFFSET,
 * read through the memory-mapped XIP window and written with
 * flash_range_erase/program. It lives 15 MB into the 16 MB part, well clear of
 * the firmware image. (It used to sit at 2 MB, which the image has grown past:
 * every save there overwrote a strip of the embedded test card, and every
 * flash overwrote the config. config_load() migrates a valid copy from there.)
 * Further sectors after it hold small independent blobs (config_blob_*).
 * Ported from tesserae-device-pico-bin.
 */
#include "config.h"

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "hardware/flash.h"
#include "pico/multicore.h"
#include "hardware/sync.h"
#include "hardware/regs/addressmap.h"   /* XIP_BASE */
/* Flash as stored, not as mapped: when the firmware runs from partition B the
 * boot ROM translates XIP_BASE onto that partition, so the config at 15 MB is
 * read through the untranslated (uncached) window instead. */
#define FLASH_RAW XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE

#define CONFIG_FLASH_OFFSET   (15u * 1024 * 1024)
#define CONFIG_OLD_OFFSET     (2u * 1024 * 1024)
#define CONFIG_MAGIC          0x42454453u           /* "BEDS" */
#define CONFIG_VERSION        7u                    /* bump to wipe stale config */
#define DEFAULT_POLL_S        15

typedef struct {
    uint32_t magic;
    uint32_t version;
    config_t cfg;
    uint32_t crc;            /* crc32 over magic..cfg */
} stored_t;
#define CONFIG_PAGE_BYTES (((sizeof(stored_t) + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE) * FLASH_PAGE_SIZE)

static config_t s_cfg;

static uint32_t crc32(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

static void defaults(void)
{
    memset(&s_cfg, 0, sizeof s_cfg);
    s_cfg.poll_s = DEFAULT_POLL_S;
    s_cfg.mirror_h = 0xFF;
    s_cfg.backlight = 40;
    s_cfg.keybar = 1;
    s_cfg.n_alarms = 1;
    s_cfg.alarms[0].h = 6; s_cfg.alarms[0].m = 30; s_cfg.alarms[0].on = 1; s_cfg.alarms[0].days = 0x1F; s_cfg.alarms[0].early = 15;
    static const char *const defaults_bind[CONFIG_BINDINGS] = {
        "rain.toggle", "lights.toggle", "ac.toggle", "menu",           /* key press (4 is fixed: menu / back) */
        "dash.prev", "dash.next", "night", "none",                     /* key hold (4 is fixed: home) */
        "vol.up", "vol.down", "menu", "back",                          /* knob: fixed by the UI */
        "menu", "back",                                                /* BOOT      */
    };
    for (int i = 0; i < CONFIG_BINDINGS; i++) snprintf(s_cfg.bind[i], CONFIG_BIND_LEN, "%s", defaults_bind[i]);
    /* key labels default to empty: derived from the action and the live state */
}

static bool valid_at(uint32_t off)
{
    const stored_t *f = (const stored_t *)(FLASH_RAW + off);
    return f->magic == CONFIG_MAGIC && f->version == CONFIG_VERSION && f->crc == crc32(f, offsetof(stored_t, crc));
}

static void set_str(char *dst, size_t cap, const char *src);

/* The key holds used to open the Sound / Lights / AC pages; Mk II holds change
 * dashboard and night mode. Only untouched old defaults move. */
static bool migrate_holds(void)
{
    static const char *const OLD[3] = { "open.sound", "open.lights", "open.ac" }, *const NEW[3] = { "dash.prev", "dash.next", "night" };
    for (int i = 0; i < 3; i++) if (strcmp(s_cfg.bind[CONFIG_BIND_KEY1_HOLD + i], OLD[i])) return false;
    for (int i = 0; i < 3; i++) set_str(s_cfg.bind[CONFIG_BIND_KEY1_HOLD + i], CONFIG_BIND_LEN, NEW[i]);
    printf("config: key holds now change dashboard and night mode\n");
    return true;
}

bool config_load(void)
{
    if (valid_at(CONFIG_FLASH_OFFSET)) {
        s_cfg = ((const stored_t *)(FLASH_RAW + CONFIG_FLASH_OFFSET))->cfg;
        if (migrate_holds()) config_save();
        return true;
    }
    if (valid_at(CONFIG_OLD_OFFSET)) {
        s_cfg = ((const stored_t *)(FLASH_RAW + CONFIG_OLD_OFFSET))->cfg;
        printf("config: moving from the 2 MB sector (inside the firmware image) to 15 MB\n");
        config_save();
        return true;
    }
    defaults();
    return false;
}

/* Erase one sector and program len bytes (a multiple of the page size). */
typedef struct { uint32_t off; const uint8_t *data; size_t len; } flash_job_t;
volatile int g_flash_step;              /* diagnostics: how far the last write got */
static void write_sector(const flash_job_t *j)
{
    g_flash_step = 3;
    flash_range_erase(j->off, FLASH_SECTOR_SIZE);
    g_flash_step = 4;
    flash_range_program(j->off, j->data, j->len);
    g_flash_step = 5;
}

/* Core 1 paints from flash-resident code, so park it in RAM (the SDK's
 * multicore lockout) for the write. Called explicitly: flash_safe_execute()
 * only does this when PICO_FLASH_SAFE_EXECUTE_PICO_SUPPORT_MULTICORE_LOCKOUT
 * is defined, which the SDK's CMake sets and the PlatformIO build does not;
 * without it core 1 ran on through the erase and hung. Before core 1 starts
 * (boot), interrupts off on this core is enough. */
static void flash_write(uint32_t off, const uint8_t *data, size_t len)
{
    flash_job_t j = { off, data, len };
    bool park = multicore_lockout_victim_is_initialized(1);
    g_flash_step = 1;
    if (park) multicore_lockout_start_blocking();
    g_flash_step = 2;
    uint32_t ints = save_and_disable_interrupts();
    write_sector(&j);
    restore_interrupts(ints);
    g_flash_step = 6;
    if (park) multicore_lockout_end_blocking();
    g_flash_step = 7;
}

/* One 4 KB sector (erase, then program up to 4 KB from SRAM), core 1 parked:
 * the firmware updater writes the spare partition with this. */
void config_flash_sector(uint32_t off, const uint8_t *data, size_t len) { flash_write(off, data, len); }

/* A sector of SRAM for staging those writes (flash can't program from PSRAM);
 * shared by the updater and the file store, which never run at once. */
static uint8_t s_stage[FLASH_SECTOR_SIZE] __attribute__((aligned(4)));
uint8_t *config_sector_buf(void) { return s_stage; }

/* Master reset: erase the config and every blob sector. The caller reboots;
 * config_seed() then starts again from include/secrets.h. */
void config_erase_all(void)
{
    static uint8_t blank[FLASH_PAGE_SIZE];
    memset(blank, 0xFF, sizeof blank);
    for (int i = 0; i <= 8; i++) flash_write(CONFIG_FLASH_OFFSET + (uint32_t)i * FLASH_SECTOR_SIZE, blank, sizeof blank);
    printf("config: everything erased\n");
}

/* ---- blobs: one sector each after the config ------------------------------------ */
typedef struct { uint32_t magic, len, crc; } blob_hdr_t;
#define BLOB_OFF(slot) (CONFIG_FLASH_OFFSET + (uint32_t)(slot) * FLASH_SECTOR_SIZE)

bool config_blob_load(int slot, uint32_t magic, void *data, size_t len)
{
    if (slot < 1 || slot > 8) return false;
    const blob_hdr_t *h = (const blob_hdr_t *)(FLASH_RAW + BLOB_OFF(slot));
    if (h->magic != magic || h->len != len || h->crc != crc32(h + 1, len)) return false;
    memcpy(data, h + 1, len);
    return true;
}

bool config_blob_save(int slot, uint32_t magic, const void *data, size_t len)
{
    static uint8_t page[1024];
    if (slot < 1 || slot > 8 || len + sizeof(blob_hdr_t) > sizeof page) return false;
    memset(page, 0xFF, sizeof page);
    blob_hdr_t h = { magic, (uint32_t)len, crc32(data, len) };
    memcpy(page, &h, sizeof h);
    memcpy(page + sizeof h, data, len);
    size_t n = ((sizeof h + len + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE) * FLASH_PAGE_SIZE;
    flash_write(BLOB_OFF(slot), page, n);
    blob_hdr_t chk; memcpy(&chk, (const void *)(FLASH_RAW + BLOB_OFF(slot)), sizeof chk);
    return chk.crc == h.crc;
}

bool config_save(void)
{
    stored_t st;
    memset(&st, 0, sizeof st);
    st.magic = CONFIG_MAGIC;
    st.version = CONFIG_VERSION;
    st.cfg = s_cfg;
    st.crc = crc32(&st, offsetof(stored_t, crc));

    static uint8_t page[CONFIG_PAGE_BYTES];
    memset(page, 0xFF, sizeof page);
    memcpy(page, &st, sizeof st);

    flash_write(CONFIG_FLASH_OFFSET, page, CONFIG_PAGE_BYTES);

    const stored_t *f = (const stored_t *)(FLASH_RAW + CONFIG_FLASH_OFFSET);
    return (f->magic == CONFIG_MAGIC && f->crc == crc32(f, offsetof(stored_t, crc)));
}

const config_t *config_get(void) { return &s_cfg; }
config_t *config_mut(void) { return &s_cfg; }

static void set_str(char *dst, size_t cap, const char *src)
{
    if (src == NULL) return;
    snprintf(dst, cap, "%s", src);
}

void config_set_wifi(const char *ssid, const char *pass)
{
    set_str(s_cfg.wifi_ssid, sizeof s_cfg.wifi_ssid, ssid);
    set_str(s_cfg.wifi_pass, sizeof s_cfg.wifi_pass, pass);
}
void config_set_server(const char *url)         { set_str(s_cfg.server_url, sizeof s_cfg.server_url, url); }
void config_set_device_id(const char *id)       { set_str(s_cfg.device_id, sizeof s_cfg.device_id, id); }
void config_set_device_name(const char *name)   { set_str(s_cfg.device_name, sizeof s_cfg.device_name, name); }
void config_set_kind(const char *kind)          { set_str(s_cfg.kind, sizeof s_cfg.kind, kind); }

#ifndef TESSERAE_KIND
/* The server's own 10.3" IT8951 / ED103TC2 board. Its hardware-catalog entry
 * fixes the panel at 1872x1404 gray_16 on the esp32_gray_bin renderer, which is
 * exactly this glass; a bare esp32_client kind lets the Register card default
 * to 800x480 Spectra 6 (seen on the bench, 29 Oct). Switch to the bedside entry
 * in tesserae/hardware/ once it is installed on the server. */
#define TESSERAE_KIND "seeed_reterminal_e1003"
#endif
const char *config_kind(void) { return s_cfg.kind[0] ? s_cfg.kind : TESSERAE_KIND; }
void config_set_device_token(const char *token) { set_str(s_cfg.device_token, sizeof s_cfg.device_token, token); }
void config_set_pairing_code(const char *code)  { set_str(s_cfg.pairing_code, sizeof s_cfg.pairing_code, code); }
void config_set_frame_etag(const char *etag)    { set_str(s_cfg.last_frame_etag, sizeof s_cfg.last_frame_etag, etag); }
void config_set_poll_s(int32_t s)               { s_cfg.poll_s = s; }
void config_set_button_event_id(uint32_t id)    { s_cfg.button_event_id = id; }
void config_set_mirror(uint8_t m)               { s_cfg.mirror_h = m; }
void config_set_lights(uint8_t backlight, uint8_t night) { s_cfg.backlight = backlight; s_cfg.night = night; }
void config_set_keybar(uint8_t on)              { s_cfg.keybar = on; }
void config_set_bind(int input, const char *spec)
{
    if (input < 0 || input >= CONFIG_BINDINGS) return;
    set_str(s_cfg.bind[input], CONFIG_BIND_LEN, spec);
}

void config_set_key_label(int key, const char *label)
{
    if (key < 0 || key > 3) return;
    set_str(s_cfg.key_label[key], sizeof s_cfg.key_label[key], label);
}

const char *config_bind_name(int input)
{
    static const char *const names[CONFIG_BINDINGS] = {
        "key1", "key2", "key3", "key4", "key1_hold", "key2_hold", "key3_hold", "key4_hold",
        "knob_cw", "knob_ccw", "knob_press", "knob_hold", "boot_short", "boot_long",
    };
    return (input >= 0 && input < CONFIG_BINDINGS) ? names[input] : "";
}

bool config_has_wifi(void)   { return s_cfg.wifi_ssid[0]  != '\0'; }
bool config_has_server(void) { return s_cfg.server_url[0] != '\0'; }

const char *config_device_id(void)
{
    if (s_cfg.device_id[0] != '\0') return s_cfg.device_id;

    /* "bedside-xxxx" from the last two bytes of the RP2350 unique id: matches
     * ^[a-z][a-z0-9_-]{1,31}$ and is stable across reflashes. */
    static char s_devid[16];
    if (s_devid[0] == '\0') {
        pico_unique_board_id_t bid;
        pico_get_unique_board_id(&bid);
        snprintf(s_devid, sizeof s_devid, "bedside-%02x%02x",
                 bid.id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES - 2],
                 bid.id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES - 1]);
    }
    return s_devid;
}
