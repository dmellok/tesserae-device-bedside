/*
 * tesserae.h: the device's Tesserae client, driven from the main loop.
 *
 * This is an always-on, mains-powered panel, so the loop is the "always on"
 * variant of the protocol (docs/tesserae.md): no deep sleep, poll /frame on the
 * server's cadence, repaint only on a 200, heartbeat on each poll. Pairing is
 * the zero-touch discover flow by default (announce; the admin clicks Register
 * once in Settings -> Devices; the next announce hands back the token), or a
 * pre-minted pairing code when one is stored.
 *
 * The frame lands in PSRAM as the 1,314,144-byte packed 4bpp buffer the
 * server's esp32_gray_bin renderer emits, which is byte-for-byte the row format
 * it8951.c streams (high nibble = even column, 0x0 black .. 0xF white), so there
 * is no decode or dither on the device. The caller paints it through its own
 * row generator so overlays (menu, badges, key labels) composite on top.
 *
 * Requests are synchronous and block the main loop for up to a few seconds
 * (frame download ~1-3 s on a good link); the knob/keys are not serviced in
 * that window. Acceptable for the bench; see README "Known limitations".
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    TESS_OFF = 0,        /* no server URL configured */
    TESS_NO_WIFI,        /* waiting for the network */
    TESS_DISCOVERING,    /* announcing; waiting for the admin to click Register */
    TESS_PAIRING,        /* registering with a pairing code */
    TESS_READY,          /* token held; polling */
    TESS_ERROR,          /* last cycle failed to reach the server */
} tess_state_t;

/* Called by the client when a new frame has been downloaded and should be
 * painted (the frame pointer is tesserae_frame()). */
typedef void (*tess_paint_fn)(void);

/* frame_buf: PSRAM buffer of at least IT8951_FRAME_BYTES; NULL disables
 * frame fetches (the client still pairs and heartbeats). */
void tesserae_init(uint8_t *frame_buf, uint8_t *dl_buf, size_t frame_cap, tess_paint_fn on_frame);

/* Run one step if one is due (non-blocking when nothing is due). Call from the
 * main loop every few ms. */
void tesserae_poll(void);

/* Force the next poll now; `drop_etag` also forgets the ETag so the server
 * answers 200 with a full frame (the "refresh" action). */
void tesserae_kick(bool drop_etag);

/* Queue a physical key press to report on the next poll (and bring it
 * forward). name is one of the device's button_map names: conventionally
 * "left", "right", "refresh", or the key names set in Settings -> Devices. */
void tesserae_button(const char *name);

/* Forget the token (and pairing code) so the device re-pairs on the next poll. */
void tesserae_forget(void);

tess_state_t   tesserae_state(void);
const char    *tesserae_state_name(void);
const uint8_t *tesserae_frame(void);          /* NULL until the first frame */
/* Where the frame sits: the whole panel (0, 0, 1872, 1404), or the viewport it
 * was rendered for when the server honoured the reported viewport. */
void           tesserae_frame_geom(int *x, int *y, int *w, int *h);
/* The dashboard's viewport, reported on every heartbeat. A change asks for a
 * fresh frame. w 0 = the whole panel. */
void           tesserae_set_viewport(int x, int y, int w, int h);
const char    *tesserae_frame_etag(void);     /* digest of the frame on the glass */
int32_t        tesserae_poll_s(void);         /* current cadence */
uint32_t       tesserae_next_poll_in_s(void); /* seconds until the next poll */
const char    *tesserae_local_time(void);     /* server-resolved ISO 8601, "" until known */
int32_t        tesserae_tz_offset_s(void);   /* the server's UTC offset (local = UTC + this) */
const char    *tesserae_last_error(void);     /* "" when the last cycle was clean */
uint32_t       tesserae_frames_painted(void);

/* Called with the download buffer before each frame download; blocks while
 * something (the core 1 painter) is still reading it. */
void tesserae_set_buffer_guard(void (*fn)(const uint8_t *buf));
