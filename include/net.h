/*
 * net.h: WiFi (CYW43 on the Pico Plus 2 W) + a tiny raw-lwIP HTTP server for
 * the bench web UI. Credentials come from the flash config (seeded from
 * include/secrets.h on first boot).
 *
 * Threading: the PlatformIO picosdk builder links the "threadsafe background"
 * cyw43 arch, so lwIP callbacks run from an interrupt-driven context. They
 * therefore only parse, queue a command for the main loop (app_command) or
 * fill a small JSON buffer; the preview frame is rendered by net_poll() in the
 * main loop and handed to lwIP under cyw43_arch_lwip_begin/end.
 *
 * HTTP API (all GET, POST accepted too):
 *   /                 the UI page (www/index.html)
 *   /api/state        JSON snapshot of panel, menu, tunables, timings
 *   /api/cmd?k=v      queue a command (see app_command in main.c)
 *   /frame?s=N        preview: 4-byte header (u16 w, u16 h, little-endian) then
 *                     packed 4bpp rows (w/2 bytes each), the panel content
 *                     decimated by N (2..8)
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Join `ssid` (blocks up to ~30 s per attempt, two attempts) and start the
 * HTTP server. False with no credentials or when the join fails. */
bool        net_init(const char *ssid, const char *pass);
/* Station MAC as "aa:bb:cc:dd:ee:ff" (the device identity on the server). */
void        net_mac(char *out, size_t n);
/* Service deferred work; call often from the main loop. */
void        net_poll(void);
bool        net_connected(void);
const char *net_ip(void);
int         net_rssi(void);

/* ---- implemented by main.c ---------------------------------------------- */

/* Queue a command (copied; executed later in the main loop). */
void app_command(const char *name, const char *value);
/* Write the state JSON; returns its length. */
size_t app_state_json(char *buf, size_t cap);
/* Called repeatedly while net_init() waits for the WiFi join. */
void net_set_wait_hook(void (*fn)(void));
/* The log (the web console): text from byte `from`; *next is where to read next. */
size_t app_log_read(uint32_t from, char *dst, size_t cap, uint32_t *next);
/* Render the preview at 1/scale into the app's buffer and return it; scale 0
 * returns the existing buffer without re-rendering. */
const uint8_t *app_render_preview(int scale, size_t *len);
