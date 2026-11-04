/*
 * commands.c: the serial and web command set, the state JSON and the preview, the console log, and field diagnostics
 */
#include "app_internal.h"

/* ---- pin discovery (field debugging; see README) ------------------------------- */

static void diag_sample(uint32_t *out)
{
    *out = 0;
    for (uint p = 0; p < DIAG_PINS; p++) if (gpio_get(p)) *out |= 1u << p;
}

void diag_pins(void)
{
    uint32_t dn = 0, up = 0;
    printf("diag: scanning GP0..GP%d (pull-down, then pull-up)\n", DIAG_PINS - 1);
    epd_io_deinit();
    for (uint p = 0; p < DIAG_PINS; p++) {
        if (p == EPD_PIN_RST) continue;
        gpio_init(p);
        gpio_pull_down(p);
    }
    sleep_ms(5);
    diag_sample(&dn);
    for (uint p = 0; p < DIAG_PINS; p++) if (p != EPD_PIN_RST) gpio_pull_up(p);
    sleep_ms(5);
    diag_sample(&up);

    printf("diag: driven HIGH:");
    for (uint p = 0; p < DIAG_PINS; p++) if (p != EPD_PIN_RST && (dn & up & (1u << p))) printf(" GP%u", p);
    printf("\ndiag: driven LOW :");
    for (uint p = 0; p < DIAG_PINS; p++) if (p != EPD_PIN_RST && !((dn | up) & (1u << p))) printf(" GP%u", p);
    printf("\ndiag: floating   :");
    for (uint p = 0; p < DIAG_PINS; p++) if (p != EPD_PIN_RST && !(dn & (1u << p)) && (up & (1u << p))) printf(" GP%u", p);
    printf("\n");

    for (uint p = 0; p < DIAG_PINS; p++) if (p != EPD_PIN_RST) gpio_pull_down(p);
    gpio_init(EPD_PIN_RST); gpio_put(EPD_PIN_RST, 1); gpio_set_dir(EPD_PIN_RST, GPIO_OUT);
    sleep_ms(50);
    uint32_t before, during, a5, a50, a500, a2000;
    diag_sample(&before);
    gpio_put(EPD_PIN_RST, 0); sleep_ms(10); diag_sample(&during);
    gpio_put(EPD_PIN_RST, 1);
    sleep_ms(5);    diag_sample(&a5);
    sleep_ms(45);   diag_sample(&a50);
    sleep_ms(450);  diag_sample(&a500);
    sleep_ms(1500); diag_sample(&a2000);
    uint32_t changed = (before ^ during) | (before ^ a5) | (before ^ a50) | (before ^ a500) | (before ^ a2000);
    changed &= ~(1u << EPD_PIN_RST);
    printf("diag: RST pulse on GP%d -> pins that moved (before/during/+5ms/+50ms/+500ms/+2s):", EPD_PIN_RST);
    if (!changed) printf(" none");
    for (uint p = 0; p < DIAG_PINS; p++) {
        if (!(changed & (1u << p))) continue;
        printf(" GP%u=%u%u%u%u%u%u", p, (unsigned)((before >> p) & 1), (unsigned)((during >> p) & 1), (unsigned)((a5 >> p) & 1),
               (unsigned)((a50 >> p) & 1), (unsigned)((a500 >> p) & 1), (unsigned)((a2000 >> p) & 1));
    }
    printf("\ndiag: a pin that is low during reset and rises within ~2 s afterwards is HRDY\n");
    for (uint p = 0; p < DIAG_PINS; p++) if (p != EPD_PIN_RST) gpio_disable_pulls(p);
    gpio_pull_up(BTN_PIN);
}

/* ---- commands (serial + web share this) --------------------------------------- */

static void exec_command(const char *name, const char *value)
{
    int v = atoi(value);
    bool save = false;
    if      (!strcmp(name, "fetch"))     tesserae_kick(true);
    else if (!strcmp(name, "ha")) {          /* a command from Home Assistant (ha.c) */
        char n2[24], v2[200];
        if (ha_translate(value, n2, sizeof n2, v2, sizeof v2)) exec_command(n2, v2);
        return;
    }
    else if (!strcmp(name, "mqtt"))      { ha_configure(value); return; }
    else if (!strcmp(name, "live_src")) {    /* "<slot key>|<entity id or topic>" */
        const char *bar = strchr(value, '|');
        if (bar) for (int i = 0; i < LV_COUNT; i++) if (!strncmp(value, live_key(i), (size_t)(bar - value)) && live_key(i)[bar - value] == 0) live_set_source(i, bar + 1);
        return;
    }
    else if (!strcmp(name, "live_base")) { live_set_ss_base(value); return; }
    else if (!strcmp(name, "live_loc"))  {  /* "lat,lon" */
        char *e; double la = strtod(value, &e);
        if (*e == ',') { double lo = strtod(e + 1, NULL); live_set_location((float)la, (float)lo); }
        return;
    }
    else if (!strcmp(name, "button"))    tesserae_button(value);
    /* Virtual controller (web UI): the same bindings as the hardware. */
    else if (!strcmp(name, "knob")) {
        if      (!strcmp(value, "cw"))    on_input((input_event_t){ IN_TURN, +1 });
        else if (!strcmp(value, "ccw"))   on_input((input_event_t){ IN_TURN, -1 });
        else if (!strcmp(value, "press")) on_input((input_event_t){ IN_PRESS_SHORT, 0 });
        else if (!strcmp(value, "hold"))  on_input((input_event_t){ IN_PRESS_LONG, 0 });
    }
    else if (!strcmp(name, "key"))       { if (v >= 1 && v <= 4) key_run(v - 1, false); }   /* the same as the hardware */
    else if (!strcmp(name, "keyhold"))   { if (v >= 1 && v <= 4) key_run(v - 1, true); }
    else if (!strcmp(name, "page")) {
        screen_t sc = SCR_COUNT;
        for (int i = 0; i < SCR_COUNT; i++) if (!strcmp(value, local_screen_name((screen_t)i))) sc = (screen_t)i;
        if (sc != SCR_COUNT) { local_open(sc); local_flush(); }
    }
    else if (!strcmp(name, "scenario"))  { if (local_scenario(value)) local_flush(); }
    else if (!strcmp(name, "set")) {
        char key[24]; const char *bar = strchr(value, '|');
        if (bar && (size_t)(bar - value) < sizeof key) {
            snprintf(key, sizeof key, "%.*s", (int)(bar - value), value);
            if (local_set(key, bar + 1)) local_flush(); else printf("set: rejected %s\n", value);
        }
    }
    else if (!strcmp(name, "somafm"))    radio_soma_refresh();
    /* A radio preset: radio=<0-6>|<name>|<url> ("" clears the slot). */
    else if (!strcmp(name, "radio")) {
        const char *b1 = strchr(value, '|'), *b2 = b1 ? strchr(b1 + 1, '|') : NULL;
        char nm[RADIO_NAME_MAX];
        if (b1 && b2 && (size_t)(b2 - b1 - 1) < sizeof nm) {
            snprintf(nm, sizeof nm, "%.*s", (int)(b2 - b1 - 1), b1 + 1);
            printf(radio_preset_set(atoi(value), nm, b2 + 1) ? "radio: preset %d saved\n" : "radio: preset %d rejected\n", atoi(value));
        }
    }
    else if (!strcmp(name, "boot"))      run_binding(!strcmp(value, "long") ? CONFIG_BIND_BOOT_LONG : CONFIG_BIND_BOOT_SHORT);
    else if (!strcmp(name, "action"))    run_action(value);
    /* Remapping: bind=<input>|<action spec>, label=<1-4>|<text>. */
    else if (!strcmp(name, "bind")) {
        const char *bar = strchr(value, '|');
        char in[16];
        if (bar) {
            snprintf(in, sizeof in, "%.*s", (int)(bar - value) < 15 ? (int)(bar - value) : 15, value);
            int idx = binding_index(in);
            if (idx < 0) printf("bind: unknown input \"%s\"\n", in);
            else if (!action_valid(bar + 1)) printf("bind: unknown action \"%s\"\n", bar + 1);
            else { config_set_bind(idx, bar + 1); save = true; printf("bind: %s -> %s\n", in, bar + 1); }
        }
    }
    else if (!strcmp(name, "label")) {
        const char *bar = strchr(value, '|');
        int i = atoi(value) - 1;
        if (bar && i >= 0 && i < 4) { char lb[16]; snprintf(lb, sizeof lb, "%.13s", bar + 1); config_set_key_label(i, lb); save = true; local_set_dirty_full(); local_flush(); }
    }
    else if (!strcmp(name, "backlight")) { if (v < 0) v = 0; if (v > 255) v = 255; inputs_set_backlight((uint8_t)v); config_set_lights((uint8_t)v, inputs_night()); save = true; }
    else if (!strcmp(name, "night"))     { inputs_set_night(v != 0); config_set_lights(inputs_backlight(), v != 0); save = true; }
    else if (!strcmp(name, "dashboard")) show_content(CONTENT_DASHBOARD);
    else if (!strcmp(name, "card"))      show_content(CONTENT_CARD);
    else if (!strcmp(name, "show"))      show_content(v == 1 ? CONTENT_CARD : CONTENT_DASHBOARD);
    else if (!strcmp(name, "menu")) {
        if      (!strcmp(value, "open"))   { local_open(SCR_MENU); }
        else if (!strcmp(value, "close"))  { local_action("back"); }
        else if (!strcmp(value, "up"))     { local_turn(-1); }
        else if (!strcmp(value, "down"))   { local_turn(+1); }
        else if (!strcmp(value, "select")) { local_press(); }
        local_flush();
    }
    else if (!strcmp(name, "clear"))     clear_screen(IT8951_MODE_INIT);
    else if (!strcmp(name, "white"))     clear_screen(IT8951_MODE_GC16);
    else if (!strcmp(name, "sleep"))     { painter_lock(); it8951_sleep(); painter_unlock(); }
    else if (!strcmp(name, "spitest"))   { painter_lock(); it8951_spi_test(v > 0 ? v : 8); painter_unlock(); local_set_dirty_full(); local_flush(); }
    else if (!strcmp(name, "init"))      { painter_lock(); it8951_init(); painter_unlock(); local_set_dirty_full(); local_flush(); }
    else if (!strcmp(name, "enhance"))   { painter_lock(); it8951_enhance_driving(); painter_unlock(); }
    else if (!strcmp(name, "pattern"))   {     /* diagnostics: INIT, then a full GC16 of solid black or a 16-step ramp */
        bool ramp = !strcmp(value, "ramp");
        painter_lock();
        it8951_clear(IT8951_MODE_INIT);
        it8951_show_pattern(ramp ? ramp_row : black_row, NULL, IT8951_MODE_GC16);
        painter_unlock();
        printf("pattern: %s shown (GC16); 'dashboard' to go back\n", ramp ? "16-step ramp" : "solid black");
    }
    else if (!strcmp(name, "temp"))      { painter_lock(); it8951_set_temp(v); painter_unlock(); }     /* 0 = sensed */
    else if (!strcmp(name, "epdpower"))  { painter_lock(); it8951_set_rails_off(v != 0); painter_unlock(); }   /* epdpower=1: rails off between refreshes (default); 0: leave them on */
    else if (!strcmp(name, "standby"))   { painter_lock(); it8951_set_standby(v != 0); painter_unlock(); }
    else if (!strcmp(name, "cleanfull")) { it8951_set_clean_full(v != 0); printf("it8951: INIT before full GC16 %s\n", v ? "on" : "off"); }
    else if (!strcmp(name, "vcom"))      { if (v >= 500 && v <= 5000) { painter_lock(); it8951_set_vcom((uint16_t)v); painter_unlock(); } else printf("vcom: expected 500..5000 mV\n"); }
    else if (!strcmp(name, "mirror"))    { { painter_lock(); it8951_set_mirror(v != 0); painter_unlock(); } config_set_mirror(v != 0); save = true; printf("tune: mirror %d\n", v != 0); repaint(); }
    else if (!strcmp(name, "movemode"))  { tune_move_mode = (v == IT8951_MODE_GC16) ? IT8951_MODE_GC16 : IT8951_MODE_A2; }
    else if (!strcmp(name, "echo"))      { s_echo_mode = !strcmp(value, "off") ? 0xFF : !strcmp(value, "gc16") ? IT8951_MODE_GC16 : IT8951_MODE_GL16;
                                           printf("paint: second pass %s\n", value); }
    else if (!strcmp(name, "ota"))       {     /* <url>|<size>|<sha256>[|<version>]: install that image */
        char u[1024]; snprintf(u, sizeof u, "%s", value);
        char *f[4] = { u, NULL, NULL, NULL };
        for (int i = 1; i < 4 && f[i - 1]; i++) { char *bar = strchr(f[i - 1], '|'); if (bar) { *bar = 0; f[i] = bar + 1; } }
        if (f[1] && f[2]) ota_start(f[0], (uint32_t)strtoul(f[1], NULL, 10), f[2], f[3]);
        else printf("ota: expected <url>|<size>|<sha256>[|<version>]\n");
    }
    else if (!strcmp(name, "picks"))     tesserae_picks_fetch();          /* refresh the dashboard list */
    else if (!strcmp(name, "otacheck"))  ota_releases_fetch();
    else if (!strcmp(name, "otainstall")) { if (ota_release(v)) local_ota_prepare(v); else printf("ota: no such release\n"); }   /* the panel says so first, then installs */
    else if (!strcmp(name, "https"))     {     /* diagnostics: GET a URL (http or https), print the status and the start */
        static char u[1024]; snprintf(u, sizeof u, "%s", value);
        uint8_t *buf = PSRAM_XIP_BASE + 6u * 1024 * 1024 + 512u * 1024; size_t got = 0; uint32_t total = 0;
        uint32_t t0 = to_ms_since_boot(get_absolute_time());
        bool ok = rest_download_range(u, sizeof u, 0, 65536, buf, &got, &total, 30000);
        printf("https: %s, %u bytes (of %lu) in %lu ms, heap free %u\n", ok ? "ok" : "FAILED", (unsigned)got, (unsigned long)total,
               (unsigned long)(to_ms_since_boot(get_absolute_time()) - t0), (unsigned)psram_heap_free_bytes());
        if (got) { char head[121]; size_t n = got < 120 ? got : 120; memcpy(head, buf, n); head[n] = 0; for (size_t i = 0; i < n; i++) if (head[i] < 32 || head[i] > 126) head[i] = '.'; printf("https: %s\n", head); }
    }
    else if (!strcmp(name, "wifisetup")) { printf("rebooting into WiFi setup\n"); watchdog_hw->scratch[0] = SETUP_MAGIC; sleep_ms(200); watchdog_reboot(0, 0, 0); }
    else if (!strcmp(name, "showsetup")) { snprintf(s_setup_why, sizeof s_setup_why, "Preview of the setup screen."); show_content(CONTENT_SETUP); }   /* diagnostics */
    else if (!strcmp(name, "serial"))    console_input(value);
    else if (!strcmp(name, "enter"))     handle_key('\r');   /* the console's Enter: a knob press */
    else if (!strcmp(name, "spidrive"))  {     /* <mA>[f]: 2/4/8/12, 'f' = fast slew; re-inits the panel link */
        epd_io_set_drive(v, strchr(value, 'f') != NULL);
        painter_lock(); it8951_init(); painter_unlock(); local_set_dirty_full(); local_flush();
    }
    else if (!strcmp(name, "spihz"))     { if (v >= 1000000 && v <= 24000000) { painter_lock(); it8951_set_spi_hz((uint32_t)v); it8951_init(); painter_unlock(); local_set_dirty_full(); local_flush(); } }
    else if (!strcmp(name, "longpress")) { if (v >= 150 && v <= 5000) tune_long_press_ms = (uint32_t)v; }
    else if (!strcmp(name, "banner"))    { tune_banner = (v != 0); banner_update(); }
    else if (!strcmp(name, "diag"))      { painter_lock(); diag_pins(); it8951_probe_matrix(); it8951_init(); painter_unlock(); }
    /* Tesserae provisioning. Saved immediately; take effect on the next poll. */
    else if (!strcmp(name, "server"))    { config_set_server(value); save = true; tesserae_forget(); }
    else if (!strcmp(name, "device"))    { config_set_device_id(value); save = true; tesserae_forget(); }
    else if (!strcmp(name, "name"))      { config_set_device_name(value); save = true; }
    else if (!strcmp(name, "kind"))      { config_set_kind(value); save = true; tesserae_forget(); }
    else if (!strcmp(name, "pair"))      { config_set_pairing_code(value); config_set_device_token(""); save = true; tesserae_kick(true); }
    else if (!strcmp(name, "forget"))    tesserae_forget();
    else if (!strcmp(name, "wifi")) {
        /* "ssid|password": applied at the next boot (the radio is not re-joined live). */
        char ssid[33] = "", pass[65] = "";
        const char *bar = strchr(value, '|');
        if (bar) { int n = (int)(bar - value); if (n > 32) n = 32; snprintf(ssid, sizeof ssid, "%.*s", n, value); snprintf(pass, sizeof pass, "%.64s", bar + 1); }
        else       snprintf(ssid, sizeof ssid, "%.32s", value);
        config_set_wifi(ssid, pass); save = true;
        printf("config: wifi credentials stored; reboot to join '%s'\n", ssid);
    }
    else if (!strcmp(name, "factory_reset")) {  /* web app: "factory_reset=yes" */
        if (!strcmp(value, "yes")) { printf("master reset (web)\n"); config_erase_all(); sleep_ms(200); watchdog_reboot(0, 0, 0); }
    }
    else if (!strcmp(name, "reboot"))    { printf("rebooting\n"); sleep_ms(100); watchdog_reboot(0, 0, 0); }
    else if (!strcmp(name, "bootsel"))   { add_alarm_in_ms(USB_BOOT_FAILSAFE_MS, usb_boot_now, NULL, true); enter_usb_boot(); }
    else printf("cmd: unknown \"%s\"\n", name);
    if (save) printf(config_save() ? "config: saved\n" : "config: SAVE FAILED\n");
}

/* A small queue filled from the lwIP context, drained by the main loop. A
 * phone can send several changes in quick succession; none is lost unless more
 * than CMDQ arrive between two passes of the loop. */
static struct { char name[24]; char value[200]; } s_cmdq[CMDQ];
static volatile uint8_t s_cmd_head, s_cmd_tail;

void app_command(const char *name, const char *value)
{
    uint8_t next = (uint8_t)((s_cmd_head + 1) % CMDQ);
    if (next == s_cmd_tail) { printf("web: command queue full, dropped %s\n", name); return; }
    snprintf(s_cmdq[s_cmd_head].name, sizeof s_cmdq[0].name, "%s", name);
    snprintf(s_cmdq[s_cmd_head].value, sizeof s_cmdq[0].value, "%s", value ? value : "");
    __dmb();
    s_cmd_head = next;
}

void drain_commands(void)
{
    while (s_cmd_tail != s_cmd_head) {
        char name[24], value[200];
        memcpy(name, s_cmdq[s_cmd_tail].name, sizeof name);
        memcpy(value, s_cmdq[s_cmd_tail].value, sizeof value);
        __dmb();
        s_cmd_tail = (uint8_t)((s_cmd_tail + 1) % CMDQ);
        printf("web: %s=%s\n", name, (strcmp(name, "wifi") && strcmp(name, "mqtt")) ? value : "(hidden)");
        s_last_input_ms = to_ms_since_boot(get_absolute_time());
        exec_command(name, value);
    }
}

/* ---- web UI glue --------------------------------------------------------------- */

static void json_escape(char *dst, size_t cap, const char *src)
{
    size_t n = 0;
    for (; *src && n + 2 < cap; src++) {
        if (*src == '"' || *src == '\\') { dst[n++] = '\\'; dst[n++] = *src; }
        else if ((unsigned char)*src < 0x20) dst[n++] = ' ';
        else dst[n++] = *src;
    }
    dst[n] = '\0';
}

size_t app_state_json(char *buf, size_t cap)
{
    const it8951_info_t *in = it8951_info();
    const config_t *c = config_get();
    char esc[200], mac[18];
    net_mac(mac, sizeof mac);
    size_t n = 0;
#define ADD(...) do { int k = snprintf(buf + n, cap > n ? cap - n : 0, __VA_ARGS__); if (k > 0) n += (size_t)k; } while (0)
    ADD("{\"fw\":\"%s\",\"panel\":{\"w\":%u,\"h\":%u,\"lut\":\"%s\",\"fw\":\"%s\",\"addr\":\"0x%08lx\"},",
        FW_VERSION, in->w, in->h, in->lut, in->fw, (unsigned long)in->img_addr);
    ADD("\"vcom_mv\":%ld,\"mirror\":%d,\"move_mode\":%u,\"spi_hz\":%lu,\"long_press_ms\":%lu,\"banner\":%d,",
        (long)it8951_cached_vcom(), it8951_mirror() ? 1 : 0, tune_move_mode,
        (unsigned long)it8951_spi_hz(), (unsigned long)tune_long_press_ms, tune_banner ? 1 : 0);
    ADD("\"ip\":\"%s\",\"mac\":\"%s\",\"rssi\":%d,\"psram\":%d,\"busy\":%d,\"paints\":%lu,\"last_stream_ms\":%lu,\"last_refresh_ms\":%lu,\"uptime_s\":%lu,",
        net_ip(), mac, net_rssi(), have_psram ? 1 : 0, painter_busy() ? 1 : 0, (unsigned long)it8951_paint_count(),
        (unsigned long)it8951_last_stream_ms(), (unsigned long)it8951_last_refresh_ms(),
        (unsigned long)(to_ms_since_boot(get_absolute_time()) / 1000));
    const local_state_t *ls = local_state();
    ADD("\"content\":\"%s\",\"screen\":\"%s\",", content_name(), local_screen_name(local_screen()));
    ADD("\"local\":{\"alarm\":\"%02d:%02d\",\"alarm_on\":%d,\"phase\":%d,\"rain_on\":%d,\"rain_lvl\":%d,\"lights_on\":%d,\"bright\":%d,"
        "\"ac_mode\":%d,\"sp\":%d.%d,\"vol\":%d,\"night\":%d,\"on_battery\":%d,\"dash_page\":%d},",
        ls->alarm_h, ls->alarm_m, ls->alarm_on, (int)ls->phase, ls->rain_on, ls->rain_lvl, ls->lights_on, ls->bright,
        ls->ac_mode, ls->sp_x2 / 2, (ls->sp_x2 % 2) * 5, ls->vol, ls->night, ls->on_battery, ls->dash_page);
    ADD("\"live\":"); n += live_json(buf + n, cap > n ? cap - n : 0); ADD(",");
    { extern volatile int g_flash_step;
      ADD("\"loop\":{\"phase\":\"%s\",\"n\":%lu,\"flash_step\":%d},", (const char *)s_phase, (unsigned long)s_loops, g_flash_step); }
    ADD("\"build\":\"%s\",", APP_BUILD);
    ADD("\"ota\":{\"state\":\"%s\",\"pct\":%d,\"err\":\"%s\",\"to\":\"%s\",\"slot\":%d,\"ab\":%d,\"checked_s\":%ld,\"rel\":[",
        ota_state_name(), ota_percent(), ota_error(), ota_target_version(), ota_boot_partition(), ota_partitioned() ? 1 : 0,
        ota_checked_ms() ? (long)((to_ms_since_boot(get_absolute_time()) - ota_checked_ms()) / 1000) : -1L);
    for (int i = 0; i < ota_release_count(); i++) { const ota_release_t *e = ota_release(i);
        ADD("%s{\"v\":\"%s\",\"date\":\"%s\",\"pre\":%d,\"ok\":%d}", i ? "," : "", e->ver, e->date, e->pre, e->has_manifest); }
    ADD("]},");
    ADD("\"mqtt\":"); n += ha_status_json(buf + n, cap > n ? cap - n : 0); ADD(",");
    ADD("\"audio\":{\"sr\":%d,\"listeners\":%d,\"cpu_pm\":%d},", AUDIO_SR, audio_listeners(), audio_cpu_permille());
    ADD("\"ui\":"); n += local_state_json(buf + n, cap > n ? cap - n : 0); ADD(",");
    ADD("\"inputs\":{\"encoder\":%d,\"keys\":%d,\"position\":%ld,\"backlight\":%u,\"night\":%d,\"labels\":[",
        inputs_have_encoder() ? 1 : 0, inputs_have_keys() ? 1 : 0, (long)inputs_encoder_position(),
        inputs_backlight(), inputs_night() ? 1 : 0);
    for (int i = 0; i < 4; i++) { json_escape(esc, sizeof esc, local_key_label(i)); ADD("%s\"%s\"", i ? "," : "", esc); }
    ADD("],\"bindings\":{");
    for (int i = 0; i < CONFIG_BINDINGS; i++) { json_escape(esc, sizeof esc, c->bind[i]); ADD("%s\"%s\":\"%s\"", i ? "," : "", config_bind_name(i), esc); }
    ADD("},\"actions\":[");
    for (int i = 0; i < local_action_count(); i++) ADD("%s{\"id\":\"%s\",\"label\":\"%s\"}", i ? "," : "", local_action_id(i), local_action_name(i));
    ADD("]},\"tesserae\":{");
    json_escape(esc, sizeof esc, c->server_url);      ADD("\"server\":\"%s\",", esc);
    json_escape(esc, sizeof esc, config_device_id()); ADD("\"device_id\":\"%s\",", esc);
    json_escape(esc, sizeof esc, c->device_name);     ADD("\"name\":\"%s\",", esc);
    json_escape(esc, sizeof esc, config_kind());      ADD("\"kind\":\"%s\",", esc);
    ADD("\"paired\":%d,\"has_code\":%d,\"state\":\"%s\",", c->device_token[0] ? 1 : 0, c->pairing_code[0] ? 1 : 0, tesserae_state_name());
    json_escape(esc, sizeof esc, tesserae_last_error()); ADD("\"error\":\"%s\",", esc);
    ADD("\"etag\":\"%s\",\"frames\":%lu,\"poll_s\":%ld,\"next_in_s\":%lu,\"local_time\":\"%s\",\"has_frame\":%d}}",
        tesserae_frame_etag(), (unsigned long)tesserae_frames_painted(), (long)tesserae_poll_s(),
        (unsigned long)tesserae_next_poll_in_s(), tesserae_local_time(), tesserae_frame() ? 1 : 0);
#undef ADD
    return n < cap ? n : cap - 1;
}

/* Preview: the composite generator decimated by `scale`, in PSRAM past the
 * frame buffers. Without PSRAM there is no preview (nor dashboards). */
static size_t   preview_len;
static uint8_t *preview_buf;

const uint8_t *app_render_preview(int scale, size_t *len)
{
    if (scale == 0) { *len = preview_len; return preview_len ? preview_buf : NULL; }
    if (!have_psram) { *len = 0; return NULL; }
    if (scale < 2) scale = 2;
    if (scale > 8) scale = 8;
    preview_buf = PSRAM_XIP_BASE + PREVIEW_PSRAM_OFF;

    int w = (IT8951_W / scale) & ~1, h = IT8951_H / scale;
    uint8_t *dst = preview_buf;
    dst[0] = (uint8_t)w; dst[1] = (uint8_t)(w >> 8); dst[2] = (uint8_t)h; dst[3] = (uint8_t)(h >> 8);
    dst += 4;
    static uint8_t row[IT8951_ROW_BYTES];
    for (int j = 0; j < h; j++) {
        composite_row(j * scale, row, NULL);
        uint8_t *o = dst + (size_t)j * (size_t)(w / 2);
        for (int i = 0; i < w; i += 2) {
            int xa = i * scale, xb = (i + 1) * scale;
            uint8_t ga = (uint8_t)((row[xa >> 1] >> ((xa & 1) ? 0 : 4)) & 0x0F);
            uint8_t gb = (uint8_t)((row[xb >> 1] >> ((xb & 1) ? 0 : 4)) & 0x0F);
            o[i >> 1] = (uint8_t)((ga << 4) | gb);
        }
    }
    preview_len = 4 + (size_t)(w / 2) * (size_t)h;
    *len = preview_len;
    return preview_buf;
}

/* ---- serial ------------------------------------------------------------------- */

void help(void)
{
    printf("keys: m menu | j/k or arrows turn knob | Enter/space press knob | Esc/q hold knob\n"
           "      f fetch frame now | l/r send key left/right | t test card | g dashboard\n"
           "      c clear (INIT) | w white (GC16) | v<mV>Enter set VCOM | V read VCOM\n"
           "      1-4 press key | i re-init | z sleep | e enhance driving | d pin discovery+sweep | p pairing status | ? help\n"
           "button (BOOT/GP45 or GP%d): short = menu/move, long (>%lu ms) = select/refresh\n",
           BTN_PIN, (unsigned long)tune_long_press_ms);
    if (net_connected()) printf("web UI: http://%s/\n", net_ip());
}

void pairing_status(void)
{
    const config_t *c = config_get();
    char mac[18]; net_mac(mac, sizeof mac);
    printf("tesserae: server=%s id=%s kind=%s mac=%s state=\"%s\" token=%s etag=%s poll=%lds frames=%lu%s%s\n",
           c->server_url[0] ? c->server_url : "(none)", config_device_id(), config_kind(), mac, tesserae_state_name(),
           c->device_token[0] ? "yes" : "no", c->last_frame_etag[0] ? c->last_frame_etag : "-",
           (long)tesserae_poll_s(), (unsigned long)tesserae_frames_painted(),
           tesserae_last_error()[0] ? " error=" : "", tesserae_last_error());
}

/* ---- console: everything printed, kept for the web app's Console tab ---- */
/* A small SRAM ring until PSRAM is up, then 64 KB in PSRAM (log_use_psram). */
static char              s_log_sram[2048];
static char             *s_log = s_log_sram;
uint32_t          s_log_cap = sizeof s_log_sram;
static void log_out(const char *buf, int len)
{
    uint32_t w = s_log_w;
    for (int i = 0; i < len; i++) s_log[(w + (uint32_t)i) % LOG_RING] = buf[i];
    s_log_w = w + (uint32_t)len;
}
stdio_driver_t s_log_drv = { .out_chars = log_out };

void log_use_psram(char *buf, uint32_t cap)
{
    uint32_t w = s_log_w, keep = w < s_log_cap ? w : s_log_cap;
    for (uint32_t i = 0; i < keep; i++) buf[(w - keep + i) % cap] = s_log[(w - keep + i) % s_log_cap];
    s_log = buf; s_log_cap = cap;           /* positions stay valid: only the ring grew */
}

/* Log text from position `from` (bytes ever written); returns bytes copied and
 * where the next read starts. A reader too far behind skips to the oldest kept. */
size_t app_log_read(uint32_t from, char *dst, size_t cap, uint32_t *next)
{
    uint32_t w = s_log_w;
    if (from > w || w - from > LOG_RING) from = w > LOG_RING ? w - LOG_RING : 0;
    size_t n = w - from; if (n > cap) n = cap;
    for (size_t i = 0; i < n; i++) dst[i] = s_log[(from + (uint32_t)i) % LOG_RING];
    *next = from + (uint32_t)n;
    return n;
}

/* The newest `cap` bytes of the log (for the boot screen). */
size_t app_log_tail(char *dst, size_t cap)
{
    uint32_t w = s_log_w, from = w > cap ? w - (uint32_t)cap : 0, next;
    return app_log_read(from, dst, cap, &next);
}

/* Typed into the web console: the same handler as the USB serial port. */
void console_input(const char *text)
{
    printf("> %s\n", text);
    for (const char *p = text; *p; p++) handle_key((unsigned char)*p);
    handle_key(0);                          /* ends a v<mV> entry; otherwise ignored */
}

void handle_key(int ch)
{
    static bool collecting_vcom;
    static int  vcom_digits;
    static unsigned vcom_val;

    if (collecting_vcom) {
        if (ch >= '0' && ch <= '9') { vcom_val = vcom_val * 10 + (unsigned)(ch - '0'); vcom_digits++; putchar(ch); return; }
        collecting_vcom = false;
        putchar('\n');
        if (vcom_digits > 0 && vcom_val >= 500 && vcom_val <= 5000) { painter_lock(); it8951_set_vcom((uint16_t)vcom_val); painter_unlock(); }
        else printf("vcom: expected v<500..5000> (millivolts, e.g. v1480)\n");
        return;
    }
    if (ch == 0x1B) {
        int c2 = getchar_timeout_us(5000);
        if (c2 == '[') {
            int c3 = getchar_timeout_us(5000);
            if (c3 == 'A') on_input((input_event_t){ IN_TURN, -1 });
            else if (c3 == 'B') on_input((input_event_t){ IN_TURN, +1 });
            return;
        }
        on_input((input_event_t){ IN_PRESS_LONG, 0 });
        return;
    }
    switch (ch) {
    case 'm': local_action("menu"); local_flush(); return;
    case 'j': case '\t': on_input((input_event_t){ IN_TURN, +1 }); return;
    case 'k':            on_input((input_event_t){ IN_TURN, -1 }); return;
    case 'q':            on_input((input_event_t){ IN_PRESS_LONG, 0 }); return;
    case '\r': case '\n': case ' ':
        on_input((input_event_t){ IN_PRESS_SHORT, 0 }); return;
    case 'f': tesserae_kick(true); return;
    case 'l': tesserae_button("left"); return;
    case 'r': tesserae_button("right"); return;
    case 't': show_content(CONTENT_CARD); return;
    case 'g': show_content(CONTENT_DASHBOARD); return;
    case 'c': clear_screen(IT8951_MODE_INIT); return;
    case 'w': clear_screen(IT8951_MODE_GC16); return;
    case 'v': collecting_vcom = true; vcom_digits = 0; vcom_val = 0; printf("vcom: v"); return;
    case 'V': { painter_lock(); uint16_t v = it8951_get_vcom(); painter_unlock(); printf("vcom: -%u.%02uV (%u mV)\n", v / 1000, (v % 1000) / 10, v); return; }
    case 'i': { painter_lock(); it8951_init(); painter_unlock(); } return;
    case 'z': { painter_lock(); it8951_sleep(); painter_unlock(); } return;
    case 'e': { painter_lock(); it8951_enhance_driving(); painter_unlock(); } return;
    case 'd': { painter_lock(); diag_pins(); it8951_probe_matrix(); it8951_init(); painter_unlock(); } return;
    case 'p': pairing_status(); return;
    case '?': case 'h': help(); return;
    case '1': case '2': case '3': case '4': on_key_down(ch - '1'); on_key_up(ch - '1'); return;
    default: return;
    }
}
