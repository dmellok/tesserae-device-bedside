/*
 * sim.h: what the simulator's own pieces (sim/src) share.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    int         port;           /* the web server: the phone app, /api, and the simulator page /sim */
    const char *flash_path;     /* the 16 MB flash image: settings, pairing, stored files */
    bool        instant;        /* refreshes take no time (default: the panel's real timings) */
    bool        no_audio;       /* no sound through the Mac's speakers */
    bool        no_knob, no_keys;   /* as if a key board were unplugged */
} sim_opts_t;
extern sim_opts_t sim_opt;

/* ---- the clock: real time plus jumps forward (the "time warp") ---- */
uint64_t sim_real_us(void);          /* since this boot, without the warp */
void     sim_warp(int64_t seconds);  /* jump forward: timers fire, alarms ring, as if the time had passed */
int64_t  sim_warp_total_s(void);     /* all jumps, kept across a simulated reboot */
void     sim_seed_epoch(void);       /* the wall clock from the Mac's until a server's Date header sets it */
int32_t  sim_host_tz_offset_s(void);

/* ---- reboot: the process starts itself again; the flash file stays ---- */
void sim_reboot(const char *why) __attribute__((noreturn));
void sim_flash_open(const char *path);
void sim_flash_sync(void);
bool sim_is_main_thread(void);

/* ---- the panel (sim_panel.c) ---- */
uint32_t       sim_glass_version(void);   /* bumps on every change of the glass */
const uint8_t *sim_glass(void);           /* 1872 x 1404, 4 bpp, even x in the high nibble */
void           sim_glass_lock(bool lock);
size_t         sim_panel_json(char *b, size_t cap);

/* ---- the keys and the knob (sim_keys.c) ---- */
void   sim_key(int i, bool down);
void   sim_knob_turn(int detents);
void   sim_knob_button(bool down);
void   sim_boot_button(bool down);
size_t sim_keys_json(char *b, size_t cap);

/* ---- the network (sim_lwip.c, sim_web.c) ---- */
const char *sim_lan_ip(void);
void        sim_web_poll(void);

/* ---- the menus (wrap_ui_menu.c) ---- */
size_t sim_menu_json(char *b, size_t cap);

/* ---- sound (sim_audio.c) ---- */
size_t sim_audio_json(char *b, size_t cap);
