/*
 * app_internal.h: what main.c, screens.c, paint.c and commands.c share
 */
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/stdio.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "tusb.h"                 /* cdc_line_coding_t: the 1200-baud reset */
#include "it8951.h"
#include "epd_io.h"
#include "ota.h"
#include "psram_heap.h"
#include "cJSON.h"
#include "boot_logo.h"
#include "pico/unique_id.h"
#include "hardware/structs/watchdog.h"
#include "patterns.h"
#include "store.h"
#include "patterns.h"
#include "psram.h"
#include "menu.h"
#include "net.h"
#include "config.h"
#include "tesserae.h"
#include "inputs.h"
#include "local.h"
#include "net_rest.h"
#include "draw.h"
#include "painter.h"
#include "audio.h"
#include "power.h"
#include "ha.h"
#include "live.h"
#include "radio.h"
#include "pico/stdio/driver.h"

/* ---- macros ---- */

/* boot */
#define SETUP_MAGIC 0x5E7A5E7Au          /* watchdog scratch 0: boot into WiFi setup */
#ifndef FW_VERSION
#define FW_VERSION "0.1.0"
#endif
#ifndef BTN_PIN
#define BTN_PIN 15                  /* optional external button to GND; idle high */
#endif
#ifndef BOOT_BTN_PIN
#define BOOT_BTN_PIN 45             /* Pico Plus 2 W wires its BOOT button to GP45 too */
#endif
#ifndef LONG_PRESS_MS
#define LONG_PRESS_MS 600
#endif
#ifndef MENU_MOVE_MODE
#define MENU_MOVE_MODE IT8951_MODE_A2   /* fast B/W update when the highlight moves */
#endif
#if defined(__has_include)
#  if __has_include("secrets.h")
#    include "secrets.h"
#  endif
#endif

/* the boot screen */
#define BOOT_LOGO_X   104                 /* top left, the log aligned under it */
#define BOOT_LOGO_Y   84
#define BOOT_SUB_Y    (BOOT_LOGO_Y + 200 + 30)
#define BOOT_RULE_Y   (BOOT_SUB_Y + 66)
#define BOOT_LOG_X    112
#define BOOT_LOG_Y    (BOOT_RULE_Y + 34)
#define BOOT_LINE_H   30
#define BOOT_LINES    ((IT8951_H - 50 - BOOT_LOG_Y) / BOOT_LINE_H)
#define BOOT_COLS     ((IT8951_W - 2 * BOOT_LOG_X) / GLYPH_W)

/* the painter */
#define SETTLE_MS 15000u                 /* a full clean refresh this long after the last interaction */

/* commands and the console */
#define DIAG_PINS 30
#define CMDQ 8
#define PREVIEW_PSRAM_OFF  (4u * 1024 * 1024)
#define LOG_RING s_log_cap

/* ---- types ---- */
typedef enum { CONTENT_DASHBOARD, CONTENT_CARD, CONTENT_WHITE, CONTENT_LIVE, CONTENT_BOOT, CONTENT_SETUP, CONTENT_FLASHING } content_t;   /* BOOT: logo + the boot log; SETUP: WiFi setup steps */   /* LIVE: white, the live page is drawn over it */
typedef struct { char line[BOOT_LINES][BOOT_COLS + 1]; char sub[96]; } boot_text_t;
typedef struct { int y; const bfont_t *f; uint8_t g; const char *t; } setup_line_t;
typedef struct {
    content_t      content;
    const uint8_t *frame;
    bool           banner;
    char           text[96];
    boot_text_t    boot;
} paint_snap_t;
typedef enum { PRESS_NONE, PRESS_SHORT, PRESS_LONG } press_t;

/* ---- shared variables (defined in the module named) ---- */
/* main.c */
extern const char APP_BUILD[];      /* the build stamp: compiled into main.c only, so tools/flash.sh's touch refreshes it */
extern bool have_psram;
extern uint8_t tune_move_mode;
extern uint32_t tune_long_press_ms;
extern bool tune_banner;

/* screens.c */
extern volatile const char *s_phase;
extern volatile uint32_t s_loops;
extern content_t cur_content;
extern boot_text_t s_boot;
extern uint32_t s_boot_seen;
extern uint32_t s_boot_last_ms;
extern uint32_t s_boot_hold_until;
extern volatile uint32_t s_log_w;
extern char s_setup_why[96];
extern uint32_t s_setup_until;
extern char s_fw_version_line[48];
extern char banner_text[96];

/* paint.c */
extern paint_snap_t P;
extern bool s_flush_pending;
extern uint32_t s_last_input_ms;
extern bool s_job_screen;
extern uint8_t s_echo_mode;

/* commands.c */
extern uint32_t s_log_cap;
extern stdio_driver_t s_log_drv;

/* ---- shared functions (defined in the module named) ---- */
/* main.c */
bool action_valid(const char *spec);
int binding_index(const char *name);
int64_t usb_boot_now(alarm_id_t id, void *u);
void boot_service(void);
void enter_usb_boot(void);
void key_run(int i, bool hold);
void on_input(input_event_t e);
void on_key_down(int i);
void on_key_up(int i);
void paint_pump(void);
void run_action(const char *spec);
void run_binding(int input);

/* screens.c */
bool boot_collect(void);
const char *content_name(void);
void banner_compose(void);
void banner_update(void);
void black_row(int y, uint8_t out[IT8951_ROW_BYTES], void *ctx);
void boot_row(int y, uint8_t out[IT8951_ROW_BYTES], const boot_text_t *t);
void composite_row(int y, uint8_t out[IT8951_ROW_BYTES], void *ctx);
void dash_row(int y, uint8_t out[IT8951_ROW_BYTES], const uint8_t *frame);
void ramp_row(int y, uint8_t out[IT8951_ROW_BYTES], void *ctx);
void setup_row(int y, uint8_t out[IT8951_ROW_BYTES]);

/* paint.c */
void cleanup_if_idle(void);
void clear_screen(uint8_t mode);
void local_flush(void);
void on_frame(void);
void paint_row(int y, uint8_t out[IT8951_ROW_BYTES], void *ctx);
void repaint(void);
void show_content(content_t c);

/* commands.c */
size_t app_log_tail(char *dst, size_t cap);
void console_input(const char *text);
void diag_pins(void);
void drain_commands(void);
void handle_key(int ch);
void help(void);
void log_use_psram(char *buf, uint32_t cap);
void pairing_status(void);
