/*
 * config.h: persistent device configuration in a reserved flash sector.
 *
 * One fixed struct, loaded into RAM at boot and written back on change. Holds
 * what must survive a power cut: WiFi credentials, the Tesserae server URL, the
 * device id and its bearer token, the pairing code (until used), the ETag of the
 * frame on the glass, and the monotonic button-event counter.
 *
 * Flash-write safety: config_save() erases and programs a sector with
 * interrupts off for a few ms. lwIP's background context is interrupt driven,
 * so a save while a request is in flight can drop packets; call it between
 * requests (the Tesserae client does this after each cycle).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Bindable inputs, in the order the web UI lists them. */
enum {
    CONFIG_BIND_KEY1 = 0, CONFIG_BIND_KEY2, CONFIG_BIND_KEY3, CONFIG_BIND_KEY4,        /* press */
    CONFIG_BIND_KEY1_HOLD, CONFIG_BIND_KEY2_HOLD, CONFIG_BIND_KEY3_HOLD, CONFIG_BIND_KEY4_HOLD,
    CONFIG_BIND_KNOB_CW, CONFIG_BIND_KNOB_CCW, CONFIG_BIND_KNOB_PRESS, CONFIG_BIND_KNOB_HOLD,
    CONFIG_BIND_BOOT_SHORT, CONFIG_BIND_BOOT_LONG,
    CONFIG_BINDINGS
};
#define CONFIG_BIND_LEN 24   /* "button:<name up to 16>" */

typedef struct {
    char     wifi_ssid[33];
    char     wifi_pass[65];
    char     server_url[160];        /* e.g. http://192.168.1.50:8765 */
    char     device_id[33];          /* announced id; replaced by the server's */
    char     device_name[41];        /* suggested display name (optional) */
    char     kind[40];               /* device kind announced to the server; see docs/tesserae.md */
    char     device_token[256];      /* bearer token, set after claim/register */
    char     pairing_code[16];       /* set pre-register, cleared on success */
    char     last_frame_etag[80];    /* digest of the frame on the glass */
    int32_t  poll_s;                 /* last next_poll_s / awake_poll_s honoured */
    uint32_t button_event_id;        /* monotonic, persists across power cycles */
    uint8_t  mirror_h;               /* 0/1: run-time override of EPD_MIRROR_H; 0xFF = default */
    uint8_t  backlight;              /* key backlight level 0..255 */
    uint8_t  night;                  /* 1 = red night palette */
    uint8_t  keybar;                 /* 1 = draw the key label strip */
    /* Input bindings: an action spec per input (see CONFIG_BIND_* and the
     * action table in main.c), and the on-screen label per key. Until the MQTT
     * config topics exist (Phase 3) these are edited from the web UI. */
    char     bind[CONFIG_BINDINGS][CONFIG_BIND_LEN];
    char     key_label[4][16];
    /* Alarms (the device rings even when Home Assistant is away). */
    uint8_t  n_alarms;
    struct { uint8_t h, m, on, days, early; } alarms[4];   /* days: bit 0 = Monday */
} config_t;

/* Load from flash. True if a valid saved config was found; false if blank or
 * corrupt (the RAM copy is then zeroed with defaults). */
bool config_load(void);

/* Write the RAM copy back to flash. True on a verified program. */
bool config_save(void);

const config_t *config_get(void);

/* Mutators update the RAM copy only; call config_save() to persist. NULL leaves
 * a string field unchanged. */
void config_set_wifi(const char *ssid, const char *pass);
void config_set_server(const char *url);
void config_set_device_id(const char *id);
void config_set_device_name(const char *name);
void config_set_kind(const char *kind);
const char *config_kind(void);   /* stored kind, else the build default */
void config_set_device_token(const char *token);
void config_set_pairing_code(const char *code);
void config_set_frame_etag(const char *etag);
void config_set_poll_s(int32_t s);
void config_set_button_event_id(uint32_t id);
void config_set_mirror(uint8_t m);
void config_set_bind(int input, const char *spec);
/* Erase one 4 KB flash sector at physical offset `off` and program `len` bytes
 * (<= 4096, from SRAM) into it, with core 1 parked. */
void config_flash_sector(uint32_t off, const uint8_t *data, size_t len);
void config_set_key_label(int key, const char *label);
const char *config_bind_name(int input);   /* "key1", "key1_hold", "knob_cw", ... */
void config_set_lights(uint8_t backlight, uint8_t night);
void config_set_keybar(uint8_t on);
config_t *config_mut(void);   /* direct access for compound fields (alarms) */

bool config_has_wifi(void);
bool config_has_server(void);

/* The device id to use: the stored one, else "bedside-<board id suffix>". */
const char *config_device_id(void);

/* Small independent records, one flash sector each after the config (slot 1..8).
 * load fails (and leaves data alone) unless magic, size and checksum match. */
bool config_blob_load(int slot, uint32_t magic, void *data, size_t len);
bool config_blob_save(int slot, uint32_t magic, const void *data, size_t len);
/* Master reset: erase the config and all blobs (reboot afterwards). */
void config_erase_all(void);
