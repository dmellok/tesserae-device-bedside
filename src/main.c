/*
 * tesserae-device-bedside: firmware for the Bedside Dashboard.
 *
 * Pico Plus 2 W + Waveshare 10.3" IT8951 HAT (1872x1404, 16 greys). Pulls its
 * dashboard from a Tesserae server (src/tesserae.c, docs/tesserae.md) into
 * PSRAM and paints it; draws the menu and a status banner itself as overlays
 * with the panel's partial refresh; serves a bench web page over WiFi
 * (src/net.c, www/index.html) with a live copy of the panel and the tunables.
 *
 * Inputs: the I2C rotary encoder and the NeoKey 1x4 on the Qw/ST connector
 * (src/inputs.c), the Pico's BOOTSEL/BOOT button (flash-CS trick and GP45),
 * an optional push button on GP15, the USB serial console (see help()), and
 * the web UI's virtual controller, which drives exactly the same handlers.
 *
 * Every input is BINDABLE: each key's press and hold, the knob's two turn
 * directions, press and hold, and the BOOT button's short and long press each
 * run an action from the table in run_action() (menu navigation, refresh,
 * content, lights, or "button:<name>", a server button wake that the Tesserae
 * Controls map turns into a page / rotation step / HA action). Bindings and
 * the key labels drawn in the strip along the top edge live in the flash
 * config and are edited from the web UI (and, in Phase 3, retained MQTT).
 * Defaults give the proposal's grammar: knob turn = move, press = select,
 * hold = back; keys = prev / next / refresh / menu.
 *
 * Everything on the panel goes through one row generator, composite_row():
 * the content (dashboard frame or test card), then the key label strip, then
 * the banner, then the menu.
 * Overlays therefore need no frame buffer, partial updates slice any rectangle
 * out of it, and the web UI's preview is the same generator decimated.
 *
 * The application is four modules sharing app_internal.h:
 *   main.c      boot, the main loop, inputs and their bindings, config seeding,
 *               the sound glue (this file)
 *   screens.c   what is on the panel: content kinds, the boot and setup screens,
 *               the banner, and composite_row()
 *   paint.c     when and how the panel is refreshed: the painter's snapshot and
 *               pump, the clean-refresh policy, clears and full repaints
 *   commands.c  the serial and web command set, the state JSON and preview, the
 *               console log, field diagnostics
 * The device-drawn UI itself is src/ui_*.c behind include/local.h.
 */
#include "app_internal.h"

const char APP_BUILD[] = __DATE__ " " __TIME__;   /* only here: tools/flash.sh touches this file so the stamp is fresh */
bool have_psram;

/* Run-time tunables (web UI / serial). */
uint8_t  tune_move_mode     = MENU_MOVE_MODE;
uint32_t tune_long_press_ms = LONG_PRESS_MS;
bool     tune_banner        = true;

/* USB flashing. The host's 1200-baud reset is ours (platformio.ini turns the
 * SDK's off): say so on the panel, which keeps the picture through the flash,
 * then hand over to the USB bootloader. An alarm makes sure the reset happens
 * even if the main loop is stuck. */
static volatile bool s_usb_flash;
int64_t usb_boot_now(alarm_id_t id, void *u) { (void)id; (void)u; reset_usb_boot(0, 0); return 0; }
void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const *lc)
{
    (void)itf;
    if (lc->bit_rate == 1200 && !s_usb_flash) { s_usb_flash = true; add_alarm_in_ms(6000, usb_boot_now, NULL, true); }
}
void enter_usb_boot(void)
{
    printf("rebooting into the USB bootloader\n");
    snprintf(s_fw_version_line, sizeof s_fw_version_line, "Now running %s", FW_VERSION);
    show_content(CONTENT_FLASHING);
    uint32_t t0 = to_ms_since_boot(get_absolute_time());
    do { paint_pump(); sleep_ms(20); }
    while ((s_flush_pending || painter_busy()) && to_ms_since_boot(get_absolute_time()) - t0 < 5000);
    sleep_ms(100);
    reset_usb_boot(0, 0);
}

/* ---- inputs ----------------------------------------------------------------- */

/* Read the BOOTSEL button (pico-examples picoboard/button, BSD-3-Clause). */
__attribute__((unused)) static bool __no_inline_not_in_flash_func(bootsel_down)(void)
{
    const uint CS_PIN_INDEX = 1;
    uint32_t flags = save_and_disable_interrupts();
    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
    for (volatile int i = 0; i < 1000; ++i) {}
#if PICO_RP2040
    bool pressed = !(sio_hw->gpio_hi_in & (1u << 1));
#else
    bool pressed = !(sio_hw->gpio_hi_in & SIO_GPIO_HI_IN_QSPI_CSN_BITS);
#endif
    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
    restore_interrupts(flags);
    return pressed;
}

static bool button_down(void)
{
    return !gpio_get(BTN_PIN) || !gpio_get(BOOT_BTN_PIN);   /* both active low */
}

static press_t poll_press(bool (*down)(void))
{
    if (!down()) return PRESS_NONE;
    sleep_ms(20);
    if (!down()) return PRESS_NONE;
    uint32_t t0 = to_ms_since_boot(get_absolute_time());
    while (down()) sleep_ms(10);
    uint32_t held = to_ms_since_boot(get_absolute_time()) - t0;
    return held >= tune_long_press_ms ? PRESS_LONG : PRESS_SHORT;
}

/* ---- actions and bindings -------------------------------------------------------- */

static void backlight_step(int delta)
{
    int v = (int)inputs_backlight() + delta;
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    inputs_set_backlight((uint8_t)v);
    config_set_lights((uint8_t)v, inputs_night());
    config_save();
}

void run_action(const char *spec)
{
    if (!spec || !spec[0] || !strcmp(spec, "none")) return;
    printf("action: %s\n", spec);
    if (local_action(spec))                { local_flush(); return; }
    if (!strncmp(spec, "button:", 7))      { if (spec[7]) tesserae_button(spec + 7); }
    else if (!strcmp(spec, "refresh"))     tesserae_button("refresh");
    else if (!strcmp(spec, "fetch"))       tesserae_kick(true);
    else if (!strcmp(spec, "dashboard"))   show_content(CONTENT_DASHBOARD);
    else if (!strcmp(spec, "card"))        show_content(CONTENT_CARD);
    else if (!strcmp(spec, "clear"))       clear_screen(IT8951_MODE_INIT);
    else if (!strcmp(spec, "sleep"))       { painter_lock(); it8951_sleep(); painter_unlock(); }
    else if (!strcmp(spec, "backlight_up"))   backlight_step(+40);
    else if (!strcmp(spec, "backlight_down")) backlight_step(-40);
    else if (!strcmp(spec, "repair"))      { tesserae_forget(); banner_update(); }
    else printf("action: unknown \"%s\"\n", spec);
}

bool action_valid(const char *spec)
{
    if (!strncmp(spec, "button:", 7)) return spec[7] != '\0';
    for (int i = 0; i < local_action_count(); i++) if (!strcmp(spec, local_action_id(i))) return true;
    return false;
}

void run_binding(int input) { run_action(config_get()->bind[input]); }

__attribute__((unused)) static bool binding_set(int input) { const char *b = config_get()->bind[input]; return b[0] && strcmp(b, "none") != 0; }

/* BOOT / GP15 button. */
static void on_press(press_t p)
{
    if (p == PRESS_SHORT)     run_binding(CONFIG_BIND_BOOT_SHORT);
    else if (p == PRESS_LONG) run_binding(CONFIG_BIND_BOOT_LONG);
}

/* Keys: a key with no hold binding acts on the way down (snappy); one with a
 * hold binding waits to see which it was: hold fires at the threshold, press
 * fires on release if the hold did not. */
static bool s_key_deferred[4];

void key_run(int i, bool hold)
{
    if (local_key(i, hold)) { local_flush(); return; }          /* MENU, or the alarm ringing */
    run_binding((hold ? CONFIG_BIND_KEY1_HOLD : CONFIG_BIND_KEY1) + i);
    local_flush();
}

void on_key_down(int i)
{
    if (i < 0 || i > 3) return;
    if (local_key_has_hold(i)) { s_key_deferred[i] = true; return; }   /* tap or hold? wait and see */
    key_run(i, false);
}

static void on_key_hold(int i)
{
    if (i < 0 || i > 3 || !s_key_deferred[i]) return;
    s_key_deferred[i] = false;
    key_run(i, true);
}

void on_key_up(int i)
{
    if (i < 0 || i > 3 || !s_key_deferred[i]) return;
    s_key_deferred[i] = false;
    key_run(i, false);
}

void on_input(input_event_t e)
{
    s_last_input_ms = to_ms_since_boot(get_absolute_time());
    switch (e.kind) {
    case IN_TURN:
        /* the whole delta at once: the UI scales it by how fast it came */
        if (!local_turn(e.value)) run_binding(e.value > 0 ? CONFIG_BIND_KNOB_CW : CONFIG_BIND_KNOB_CCW);
        local_flush();
        break;
    case IN_PRESS_SHORT: if (local_press()) local_flush(); else run_binding(CONFIG_BIND_KNOB_PRESS); break;
    case IN_PRESS_LONG: {                     /* Back by default; any other binding replaces it */
        const char *b = config_get()->bind[CONFIG_BIND_KNOB_HOLD];
        if (!b[0] || !strcmp(b, "back")) { if (local_back_up()) local_flush(); }
        else { run_binding(CONFIG_BIND_KNOB_HOLD); local_flush(); }
        break; }
    case IN_KEY:         on_key_down(e.value); break;
    case IN_KEY_HOLD:    on_key_hold(e.value); break;
    case IN_KEY_RELEASE: on_key_up(e.value); break;
    default: break;
    }
}

int binding_index(const char *name)
{
    for (int i = 0; i < CONFIG_BINDINGS; i++) if (!strcmp(name, config_bind_name(i))) return i;
    return -1;
}

/* While WiFi joins: keep the boot log moving. */
static void boot_wait(void) { boot_service(); paint_pump(); }

/* ---- config seeding ------------------------------------------------------------ */

/* secrets.h is a first-boot seed and a development override: any value it
 * defines is applied (and re-saved if it changed) on every boot. The web UI
 * edits the stored copy; drop the define from secrets.h to stop overriding. */
static void config_seed(void)
{
    bool had = config_load();
    config_t before = *config_get();
#ifdef WIFI_SSID
    config_set_wifi(WIFI_SSID, WIFI_PASS);
#endif
#ifdef SERVER_URL
    if (strcmp(before.server_url, SERVER_URL) != 0) {
        config_set_server(SERVER_URL);
        config_set_device_token("");           /* a different server means a different token */
    }
#endif
#ifdef DEVICE_ID
    if (!before.device_id[0]) config_set_device_id(DEVICE_ID);   /* a server-adopted id wins */
#endif
#ifdef DEVICE_NAME
    config_set_device_name(DEVICE_NAME);
#endif
#ifdef DEVICE_KIND
    if (strcmp(before.kind, DEVICE_KIND) != 0) { config_set_kind(DEVICE_KIND); config_set_device_token(""); }
#endif
#ifdef PAIRING_CODE
    if (!before.device_token[0]) config_set_pairing_code(PAIRING_CODE);
#endif
    if (!had || memcmp(&before, config_get(), sizeof(config_t)) != 0)
        printf(config_save() ? "config: saved (seeded from secrets.h / defaults)\n" : "config: SAVE FAILED\n");
    else
        printf("config: loaded from flash\n");
    const config_t *c = config_get();
    printf("config: wifi='%s' server='%s' id='%s' token=%s etag=%s\n",
           c->wifi_ssid, c->server_url, config_device_id(), c->device_token[0] ? "yes" : "no",
           c->last_frame_etag[0] ? c->last_frame_etag : "-");
    if (c->mirror_h != 0xFF) it8951_set_mirror(c->mirror_h != 0);
    inputs_set_backlight(c->backlight);
    inputs_set_night(c->night != 0);
}

/* ---- main --------------------------------------------------------------------- */

/* Inputs, web commands and repaints. Called from the main loop and, through
 * the REST client's idle hook, while a network request is in flight, so the
 * knob and keys never wait on WiFi. Not re-entrant. */
static void ui_service(void);

/* Tesserae is about to download into `buf`: wait while core 1 still paints it. */
static void frame_buffer_wait(const uint8_t *buf)
{
    while (painter_busy() && P.frame == buf) { ui_service(); sleep_ms(2); }
}

/* Feed the sound generator the current sleep-sound settings. */
/* The Recording sound's file: the chosen one, or the first sound file stored. */
static const store_ent_t *recording_file(const char *name)
{
    if (name[0]) return store_find(name);
    for (int i = 0; i < store_count(); i++) {
        const store_ent_t *e = store_get(i);
        size_t l = strlen(e->name);
        if (l > 4 && !strcasecmp(e->name + l - 4, ".adp")) return e;
    }
    return NULL;
}

static void audio_update(void)
{
    bool changed = false;
    float fade = local_sleep_tick(&changed);
    local_alarm_tick(&changed);
    if (changed) local_flush();
    const local_state_t *ls = local_state();
    const store_ent_t *rec = ls->rain_kind == RS_RECORDING ? recording_file(ls->rec) : NULL;
    rs_params_t p = { .on = ls->rain_on, .kind = ls->rain_kind, .level = ls->rain_lvl, .volume = ls->vol, .fade = fade,
                      .patter = ls->patter, .wash = ls->wash, .tone = ls->tone, .wind = ls->wind, .surface = ls->surface,
                      .thunder = ls->thunder, .thunder_near = ls->thunder_near, .strike = ls->strike,
                      .chorus = ls->chorus, .birds = ls->birds, .bird_vol = ls->bird_vol,
                      .audition = ls->audition, .audition_bird = ls->audition_bird,
                      .rec = rec ? store_data(rec) : NULL, .rec_len = rec ? rec->size : 0 };
    audio_set(&p);

    /* The speaker: on while there's something to hear (a bird audition or a
     * thunder strike plays for a while after it's asked for). */
    static uint32_t last_aud, last_strike, extra_until;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (ls->audition != last_aud || ls->strike != last_strike) { last_aud = ls->audition; last_strike = ls->strike; extra_until = now + 15000; }
    audio_speaker_enable(ls->speaker);
    audio_speaker_audible((ls->rain_on && ls->vol > 0) || ls->chorus > 0 || (int32_t)(extra_until - now) > 0);
    audio_speaker_tick();

    /* The waveform temperature from the settings (0 = the firmware default). */
    {   int want = ls->wf_temp > 0 ? ls->wf_temp : 20;
        if (want != it8951_temp_forced()) { painter_lock(); it8951_set_temp(want); painter_unlock(); }
        if (it8951_reinit_pending()) { printf("paint: full repaint after the controller re-init\n"); local_set_dirty_full(); local_flush(); }
    }

    /* The PowerBoost, when battery backup is fitted. */
    if (ls->power_mon && power_tick()) local_power(power_on_battery(), power_percent(), power_low());
}

static void ui_service(void)
{
    static bool in;
    if (in) return;
    in = true;
    /* While the panel changes page, leave input unread (the seesaw chips hold
     * it) and drop it when the paint ends; otherwise act on it straight away. */
    if (!(painter_busy() && s_job_screen)) {
        for (int i = 0; i < 4; i++) {
            input_event_t e = inputs_poll();
            if (e.kind == IN_NONE) break;
            on_input(e);
        }
    }
    if (s_usb_flash) enter_usb_boot();
    drain_commands();
    audio_update();
    ha_poll();
    {   /* the live page vs the server's frames: follow the chosen dashboard */
        static int last_live = -1;
        int live = local_live_active() ? 1 : 0;
        bool booting = cur_content == CONTENT_BOOT && (int32_t)(to_ms_since_boot(get_absolute_time()) - s_boot_hold_until) < 0;
        if (cur_content == CONTENT_SETUP) {
            if (s_setup_until && (int32_t)(to_ms_since_boot(get_absolute_time()) - s_setup_until) >= 0) {
                printf("setup: nothing saved; restarting to try the WiFi again\n"); sleep_ms(200); watchdog_reboot(0, 0, 0);
            }
        }
        else if (booting) boot_service();
        else if (live != last_live || cur_content == CONTENT_BOOT) { last_live = live; show_content(live ? CONTENT_LIVE : (tesserae_frame() ? CONTENT_DASHBOARD : CONTENT_LIVE)); }
    }
    if (local_live_tick()) local_flush();
    if (!painter_busy() && local_settle_tick()) local_flush();   /* the knob stopped: menus back to full quality */
    if (!painter_busy() && !s_flush_pending && it8951_rails_live()) {   /* quiet for a while: the panel's rails off (they drift the image) */
        painter_lock(); bool off = it8951_idle_tick(); painter_unlock();
        if (off) printf("it8951: panel power off (idle)\n");
    }
    switch (local_take_request()) {               /* asked for from the panel's settings */
    case LREQ_REFRESH: printf("paint: full refresh (settings)\n"); local_set_dirty_full(); local_flush(); break;
    case LREQ_RESTART: printf("rebooting (settings)\n"); sleep_ms(200); watchdog_reboot(0, 0, 0); break;
    case LREQ_SETUP:   printf("rebooting into WiFi setup\n"); watchdog_hw->scratch[0] = SETUP_MAGIC; sleep_ms(200); watchdog_reboot(0, 0, 0); break;
    case LREQ_RESET:   printf("master reset (settings)\n"); config_erase_all(); sleep_ms(200); watchdog_reboot(0, 0, 0); break;
    default: break;
    }
    net_poll();
    paint_pump();
    in = false;
}

int main(void)
{
    stdio_init_all();
    stdio_set_driver_enabled(&s_log_drv, true);   /* the web console's copy of the log */
    painter_init();
    sleep_ms(1500);
    printf("\ntesserae-device-bedside %s (built %s): Pico Plus 2 W + Waveshare 10.3\" IT8951, Tesserae client\n", FW_VERSION, APP_BUILD);

    size_t psram = psram_init(PSRAM_CS_PIN_PLUS2);
    have_psram = (psram >= IT8951_FRAME_BYTES);
    if (have_psram) log_use_psram((char *)(PSRAM_XIP_BASE + 7u * 1024 * 1024 + 768u * 1024), 64u * 1024);   /* 7.75 MB */
    if (have_psram) {   /* 1 MB heap for HTTPS (mbedTLS) and big JSON (the GitHub release list) */
        psram_heap_init(PSRAM_XIP_BASE + 4u * 1024 * 1024 + 512u * 1024, 1024u * 1024);
        cJSON_Hooks hooks = { psram_heap_malloc, psram_heap_free };
        cJSON_InitHooks(&hooks);
    }
    printf("psram: %s (%u bytes)\n", have_psram ? "ready" : "MISSING: dashboard frames need it", (unsigned)psram);

    inputs_init();          /* before config_seed() so the stored backlight applies */
    power_init();
    audio_init_pins();      /* the amp shut down until there's sound */
    config_seed();

    gpio_init(BTN_PIN);      gpio_set_dir(BTN_PIN, GPIO_IN);      gpio_pull_up(BTN_PIN);
    gpio_init(BOOT_BTN_PIN); gpio_set_dir(BOOT_BTN_PIN, GPIO_IN); gpio_pull_up(BOOT_BTN_PIN);

    if (!it8951_init()) {
        printf("no controller; running the pin discovery (press d to repeat, i to retry init)\n");
        diag_pins();
        it8951_probe_matrix();
    } else {
        printf("paint: INIT clear (deghost)\n");
        it8951_clear(IT8951_MODE_INIT);
    }

    /* Test card first so the panel is not blank while we associate, and so the
     * orientation can be checked by eye (black square top-left). */
    /* The panel is up: commit an image that was just installed (it has ~16 s to
     * do so before the boot ROM goes back to the old one), and make sure only
     * the running image can boot. Before core 1 starts: the commit writes flash,
     * and core 1 runs from flash (a commit under it killed core 1). */
    if (have_psram) ota_init(PSRAM_XIP_BASE + 6u * 1024 * 1024 + 512u * 1024, 1024u * 1024);   /* 1 MB chunks */
    ota_set_version(FW_VERSION);
    ota_boot_check();
    store_init();
    if (have_psram) net_set_upload_buffer(PSRAM_XIP_BASE + 5u * 1024 * 1024 + 512u * 1024);   /* 5.5 MB, 64 KB */
    if (have_psram) audio_use_buffer((int16_t *)(PSRAM_XIP_BASE + 6u * 1024 * 1024), 4u * AUDIO_SR);   /* ~4 s */
    painter_start(paint_row, audio_core1_start);
    boot_collect();
    show_content(CONTENT_BOOT);
    net_set_wait_hook(boot_wait);

    const config_t *c = config_get();
    /* WiFi setup when there is no network saved, when asked for (Settings >
     * Device > Wi-Fi setup), or when the saved one can't be joined: then for 5
     * minutes, after which the panel restarts and tries it again (a router
     * outage must not strand it in setup). */
    bool asked = watchdog_hw->scratch[0] == SETUP_MAGIC;
    watchdog_hw->scratch[0] = 0;
    bool up = false;
    if (config_has_wifi() && !asked) up = net_init(c->wifi_ssid, c->wifi_pass);
    if (!up) {
        char ap[33], id[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
        pico_get_unique_board_id_string(id, sizeof id);
        snprintf(ap, sizeof ap, "Bedside-setup-%s", id + strlen(id) - 4);
        if (!config_has_wifi()) snprintf(s_setup_why, sizeof s_setup_why, "No WiFi set up yet.");
        else if (asked) snprintf(s_setup_why, sizeof s_setup_why, "Setup asked for from the menu. Restarts in 10 minutes if nothing is saved.");
        else snprintf(s_setup_why, sizeof s_setup_why, "Couldn't join \"%s\". Trying it again in 5 minutes.", c->wifi_ssid);
        if (config_has_wifi()) s_setup_until = to_ms_since_boot(get_absolute_time()) + (asked ? 600000u : 300000u);
        net_start_setup(ap);
        show_content(CONTENT_SETUP);
    }
    net_set_wait_hook(NULL);
    s_boot_hold_until = to_ms_since_boot(get_absolute_time()) + 4000;   /* time to read the log */
    /* Two frame buffers: the one on screen, and one downloads land in. */
    tesserae_init(have_psram ? PSRAM_XIP_BASE : NULL, have_psram ? PSRAM_XIP_BASE + 2u * 1024 * 1024 : NULL,
                  have_psram ? IT8951_FRAME_BYTES : 0, on_frame);
    (void)up;
    if (have_psram) tesserae_set_scratch(PSRAM_XIP_BASE + 7u * 1024 * 1024 + 512u * 1024, 256u * 1024);   /* frames.json */
    local_init();
    ha_init();
    live_init();
    rest_set_idle_hook(ui_service);
    tesserae_set_buffer_guard(frame_buffer_wait);
    local_set_dirty_full(); local_flush();      /* top bar with the key labels */
    banner_update();

    help();
    pairing_status();

    while (true) {
        /* Stall watch: any phase of the loop that holds the UI for long is
         * logged with its name and duration (see README "Diagnostics"). */
        uint32_t t0 = to_ms_since_boot(get_absolute_time()), tp = t0;
        #define PHASE(name) do { s_phase = name; s_loops++; uint32_t _n = to_ms_since_boot(get_absolute_time()); \
            if (_n - tp > 400) { printf("stall: %s took %lu ms\n", name, (unsigned long)(_n - tp)); } tp = _n; } while (0)
        press_t p = poll_press(button_down);
        if (p != PRESS_NONE) on_press(p);
        PHASE("buttons");

        int ch = getchar_timeout_us(0);
        if (ch != PICO_ERROR_TIMEOUT) handle_key(ch);

        ui_service();
        PHASE("ui");

        tess_state_t before = tesserae_state();
        bool had_err = tesserae_last_error()[0] != '\0';
        if (!ota_busy()) tesserae_poll();           /* an update has the network to itself */
        ota_tick();
        local_ota_service();
        PHASE("server");
        if (tesserae_state() != before || (tesserae_last_error()[0] != '\0') != had_err) banner_update();
        if (tesserae_local_time()[0]) local_set_time(tesserae_local_time());
        local_persist_tick();
        paint_pump();
        cleanup_if_idle();
        PHASE("paint");
        (void)t0;

        sleep_ms(10);
    }
}
