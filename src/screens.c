/*
 * screens.c: what is on the panel: the content kinds, the boot and setup screens, the banner, and the row generator that composites them
 */
#include "app_internal.h"

/* ---- content ------------------------------------------------------------------ */

volatile const char *s_phase = "boot";   /* last main-loop phase passed (state JSON) */
volatile uint32_t    s_loops;
content_t cur_content = CONTENT_CARD;

static void frame_row(int y, uint8_t out[IT8951_ROW_BYTES], void *ctx)
{
    memcpy(out, (const uint8_t *)ctx + (size_t)y * IT8951_ROW_BYTES, IT8951_ROW_BYTES);
}

/* A dashboard frame is the whole panel, or just the viewport the overlay
 * leaves (tesserae_frame_geom); white around a viewport frame. */
void dash_row(int y, uint8_t out[IT8951_ROW_BYTES], const uint8_t *frame)
{
    int gx, gy, gw, gh;
    tesserae_frame_geom(&gx, &gy, &gw, &gh);
    if (gw >= IT8951_W && gh >= IT8951_H) { frame_row(y, out, (void *)frame); return; }
    memset(out, 0xFF, IT8951_ROW_BYTES);
    if (y < gy || y >= gy + gh) return;
    memcpy(out + gx / 2, frame + (size_t)(y - gy) * (size_t)(gw / 2), (size_t)(gw / 2));
}

/* Diagnostic patterns: solid black, and 16 grey steps left (black) to right (white). */
void black_row(int y, uint8_t out[IT8951_ROW_BYTES], void *ctx) { (void)y; (void)ctx; memset(out, 0x00, IT8951_ROW_BYTES); }
void ramp_row(int y, uint8_t out[IT8951_ROW_BYTES], void *ctx)
{
    (void)y; (void)ctx;
    for (int x = 0; x < IT8951_W; x += 2) {
        uint8_t a = (uint8_t)(x * 16 / IT8951_W), b = (uint8_t)((x + 1) * 16 / IT8951_W);
        out[x / 2] = (uint8_t)((a << 4) | b);
    }
}

/* ---- the boot screen: the Tesserae logo, the firmware, and the log as it
 * happens (Font24, the newest line in black), until the first dashboard. ---- */
boot_text_t s_boot;                 /* built on core 0 */
uint32_t    s_boot_seen, s_boot_last_ms, s_boot_hold_until;
static uint8_t     s_boot_line[2][IT8951_W];

volatile uint32_t s_log_w;          /* the console log's write position (below) */
/* Lines left off the screen: the panel's own paint timings (each repaint of
 * the log would otherwise add a line, and repaint it again). */
static bool boot_skip(const char *l, size_t n)
{
    static const char *const SKIP[] = { "streamed in", "area(s) in parallel", "second pass over" };
    for (size_t k = 0; k < sizeof SKIP / sizeof SKIP[0]; k++) {
        size_t m = strlen(SKIP[k]);
        for (size_t i = 0; i + m <= n; i++) if (!memcmp(l + i, SKIP[k], m)) return true;
    }
    return false;
}

/* The last BOOT_LINES non-empty lines of the log. True when they changed. */
bool boot_collect(void)
{
    static char tail[3072];
    static boot_text_t prev;
    prev = s_boot;
    size_t n = app_log_tail(tail, sizeof tail);
    int k = BOOT_LINES;
    memset(s_boot.line, 0, sizeof s_boot.line);
    size_t end = n;
    while (k > 0 && end > 0) {
        size_t e = end; while (e > 0 && (tail[e - 1] == '\n' || tail[e - 1] == '\r')) e--;
        if (e == 0) break;
        size_t b = e; while (b > 0 && tail[b - 1] != '\n') b--;
        if (boot_skip(tail + b, e - b)) { end = b; continue; }
        size_t len = e - b; if (len > BOOT_COLS) len = BOOT_COLS;
        k--;
        for (size_t i = 0; i < len; i++) { char c = tail[b + i]; s_boot.line[k][i] = (c >= 32 && c < 127) ? c : ' '; }
        end = b;
    }
    if (k > 0) {                            /* fewer lines than rows: top-align */
        memmove(s_boot.line[0], s_boot.line[k], sizeof s_boot.line[0] * (size_t)(BOOT_LINES - k));
        memset(s_boot.line[BOOT_LINES - k], 0, sizeof s_boot.line[0] * (size_t)k);
    }
    int slot = ota_boot_partition();
    snprintf(s_boot.sub, sizeof s_boot.sub, "Bedside  \x80  firmware %s%s%c", FW_VERSION, slot >= 0 ? "  \x80  slot " : "", slot >= 0 ? 'A' + slot : ' ');
    return memcmp(&prev, &s_boot, sizeof prev) != 0;
}

void boot_row(int y, uint8_t out[IT8951_ROW_BYTES], const boot_text_t *t)
{
    memset(out, 0xFF, IT8951_ROW_BYTES);
    if (y >= BOOT_LOGO_Y && y < BOOT_LOGO_Y + boot_logo_h) {
        int x0 = BOOT_LOGO_X & ~1;
        memcpy(out + x0 / 2, boot_logo + (size_t)(y - BOOT_LOGO_Y) * (boot_logo_w / 2), boot_logo_w / 2);
        return;
    }
    uint8_t *line = s_boot_line[get_core_num()];
    if (y >= BOOT_SUB_Y && y < BOOT_SUB_Y + ui34.line_h) {
        memset(line, 0xF, IT8951_W);
        draw_font_row(&ui34, line, IT8951_W, BOOT_LOG_X, t->sub, y - BOOT_SUB_Y, G_DARK);
        draw_pack(out, 0, line, IT8951_W);
        return;
    }
    if (y >= BOOT_RULE_Y && y < BOOT_RULE_Y + 2) {
        memset(line, 0xF, IT8951_W); memset(line + BOOT_LOG_X, G_LIGHT, IT8951_W - 2 * BOOT_LOG_X);
        draw_pack(out, 0, line, IT8951_W);
        return;
    }
    if (y >= BOOT_LOG_Y && y < BOOT_LOG_Y + BOOT_LINES * BOOT_LINE_H) {
        int i = (y - BOOT_LOG_Y) / BOOT_LINE_H, g = (y - BOOT_LOG_Y) % BOOT_LINE_H;
        if (g >= GLYPH_H || !t->line[i][0]) return;
        bool newest = (i == BOOT_LINES - 1 || !t->line[i + 1][0]);
        memset(line, 0xF, IT8951_W);
        draw_text_row(line, IT8951_W, BOOT_LOG_X, t->line[i], g, 1, newest ? G_BLACK : G_DARK);
        draw_pack(out, 0, line, IT8951_W);
    }
}

/* ---- setup mode screen: the logo and how to connect ------------------------- */
char     s_setup_why[96];
uint32_t s_setup_until;          /* restart to try the saved WiFi again (0: stay) */
char s_fw_version_line[48];
void setup_row(int y, uint8_t out[IT8951_ROW_BYTES])
{
    memset(out, 0xFF, IT8951_ROW_BYTES);
    if (y >= BOOT_LOGO_Y && y < BOOT_LOGO_Y + boot_logo_h) {
        memcpy(out + (BOOT_LOGO_X & ~1) / 2, boot_logo + (size_t)(y - BOOT_LOGO_Y) * (boot_logo_w / 2), boot_logo_w / 2);
        return;
    }
    static char url[40];
    snprintf(url, sizeof url, "or go to http://192.168.4.1");
    const setup_line_t F[] = {          /* CONTENT_FLASHING: the panel keeps this up through a USB flash */
        { 380,  &ui72, G_BLACK, "Updating firmware" },
        { 520,  &ui46, G_DARK,  "New firmware is loading over USB." },
        { 590,  &ui46, G_DARK,  "Keep the panel plugged in. It restarts by itself." },
        { 0,    &ui34, G_DARK,  "" }, { 0, &ui34, G_DARK, "" }, { 0, &ui34, G_DARK, "" },
        { 1240, &ui34, G_DARK,  s_fw_version_line },
    };
    const setup_line_t S[] = {
        { 380,  &ui72, G_BLACK, "Set up this panel" },
        { 520,  &ui46, G_DARK,  "1   On your phone, join the WiFi network" },
        { 590,  &ui96, G_BLACK, net_setup_ssid() },
        { 760,  &ui46, G_DARK,  "2   The setup page opens by itself" },
        { 822,  &ui34, G_DARK,  url },
        { 930,  &ui46, G_DARK,  "3   Choose your WiFi and your Tesserae server" },
        { 1240, &ui34, G_DARK,  s_setup_why },
    };
    const setup_line_t *L = cur_content == CONTENT_FLASHING ? F : S;
    uint8_t *line = s_boot_line[get_core_num()];
    for (size_t i = 0; i < sizeof S / sizeof S[0]; i++) {
        if (y < L[i].y || y >= L[i].y + L[i].f->line_h || !L[i].t[0]) continue;
        memset(line, 0xF, IT8951_W);
        int x = BOOT_LOG_X + (L[i].f == &ui34 && i == 4 ? 62 : 0);
        draw_font_row(L[i].f, line, IT8951_W, x, L[i].t, y - L[i].y, L[i].g);
        draw_pack(out, 0, line, IT8951_W);
        return;
    }
    if (y >= 1200 && y < 1202) { memset(line, 0xF, IT8951_W); memset(line + BOOT_LOG_X, G_LIGHT, IT8951_W - 2 * BOOT_LOG_X); draw_pack(out, 0, line, IT8951_W); }
}

const char *app_setup_ssid(void)   { return config_get()->wifi_ssid; }
const char *app_setup_server(void) { return config_get()->server_url; }
const char *app_setup_name(void)   { return config_get()->device_name; }

/* Repaint the log when new lines have arrived (fast 2-level update). */
void boot_service(void)
{
    if (cur_content != CONTENT_BOOT || s_log_w == s_boot_seen) return;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - s_boot_last_ms < 400 || painter_busy()) return;
    s_boot_seen = s_log_w; s_boot_last_ms = now;
    if (!boot_collect()) return;              /* only paint timings arrived */
    local_mark_area(0, BOOT_LOG_Y, IT8951_W, BOOT_LINES * BOOT_LINE_H, IT8951_MODE_DU);
    local_flush();
}

static void content_row(int y, uint8_t out[IT8951_ROW_BYTES])
{
    if (cur_content == CONTENT_BOOT) { boot_row(y, out, &s_boot); return; }
    if (cur_content == CONTENT_SETUP || cur_content == CONTENT_FLASHING) { setup_row(y, out); return; }
    const uint8_t *frame = tesserae_frame();
    if (cur_content == CONTENT_DASHBOARD && frame) dash_row(y, out, frame);
    else if (cur_content == CONTENT_WHITE || cur_content == CONTENT_LIVE) memset(out, 0xFF, IT8951_ROW_BYTES);
    else                                           IT8951_PATTERNS[3].fn(y, out, NULL);   /* the test card: grey blocks */
}

const char *content_name(void)
{
    if (cur_content == CONTENT_BOOT) return "boot screen";
    if (cur_content == CONTENT_SETUP) return "WiFi setup";
    if (cur_content == CONTENT_FLASHING) return "updating firmware";
    if (cur_content == CONTENT_LIVE) return "live";
    if (cur_content == CONTENT_DASHBOARD) return tesserae_frame() ? "dashboard" : "dashboard (none yet)";
    return cur_content == CONTENT_CARD ? "test card" : "white";
}

/* ---- overlays: local UI + banner ------------------------------------------------ */

char banner_text[96];

void composite_row(int y, uint8_t out[IT8951_ROW_BYTES], void *ctx)
{
    (void)ctx;
    content_row(y, out);
    if (cur_content == CONTENT_BOOT || cur_content == CONTENT_SETUP || cur_content == CONTENT_FLASHING) return;   /* these screens stand alone */
    local_overlay_row(y, out);
    if (tune_banner && banner_text[0]) banner_overlay_row(banner_text, y, out);
}

void banner_compose(void)
{
    const char *ip = net_connected() ? net_ip() : NULL;
    tess_state_t st = tesserae_state();
    if (!ip)                              snprintf(banner_text, sizeof banner_text, "no wifi: see include/secrets.h");
    else if (st == TESS_OFF)              snprintf(banner_text, sizeof banner_text, "http://%s/  set server URL", ip);
    else if (st == TESS_DISCOVERING)      snprintf(banner_text, sizeof banner_text, "http://%s/  register %s in Tesserae", ip, config_device_id());
    else if (st == TESS_ERROR)            snprintf(banner_text, sizeof banner_text, "http://%s/  server unreachable", ip);
    else if (st == TESS_READY && !tesserae_frame() && tesserae_last_error()[0])
                                          snprintf(banner_text, sizeof banner_text, "http://%s/  bad frame from server: check kind/panel", ip);
    else if (st == TESS_READY && !tesserae_frame())
                                          snprintf(banner_text, sizeof banner_text, "http://%s/  bind a dashboard to %s", ip, config_device_id());
    else if (cur_content == CONTENT_LIVE || (tesserae_frame() && cur_content == CONTENT_DASHBOARD))
                                          banner_text[0] = '\0';
    else                                  snprintf(banner_text, sizeof banner_text, "http://%s/", ip);
}

void banner_update(void)
{
    char before[sizeof banner_text];
    memcpy(before, banner_text, sizeof before);
    banner_compose();
    if (!strcmp(before, banner_text)) return;
    int x, y, w, h, ow = 0;
    if (before[0]) banner_rect(before, &x, &y, &ow, &h);
    if (banner_text[0]) banner_rect(banner_text, &x, &y, &w, &h); else { banner_rect("x", &x, &y, &w, &h); w = 0; }
    if (ow > w) w = ow;
    if (w == 0) return;
    local_mark_area(x, y, w, h, IT8951_MODE_GC16);
    local_flush();
}
