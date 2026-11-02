# tesserae-device-bedside

Firmware for the **Bedside Dashboard** (client project, Jorgo): a Pimoroni
**Pico Plus 2 W** (RP2350B, 16 MB flash, 8 MB PSRAM) driving a Waveshare
**10.3" e-Paper HAT** (IT8951 controller, ED103TC2 glass, 1872x1404, 16 greys)
through the Hard Stuff "Pico to Pi HAT" adapter, pulling its dashboard from a
self-hosted **[Tesserae](https://github.com/dmellok/tesserae)** server.

Status, 29 October 2026 (evening): **first light, and the UI from the
mock-up.** The panel paints at -1.48 V, PSRAM, the encoder and the NeoKey all
answer, WiFi joins, the device pairs with the server, and the dashboards
built after the proposal's interactive mock-up are on the glass (1.3 MB
fetched in ~1 s, painted in ~1.4 s). The device draws the mock-up's top bar,
menu sheet, local pages and alarm screen itself; keys toggle state with a
0.25 s partial update and the knob sets the volume.

The panel bring-up from
[ed103tc2-pico-driver](https://github.com/dmellok/ed103tc2-pico-driver) is
carried over whole (pin map, 12.5 MHz PIO SPI, DMA streaming, GC16/A2 partial
updates, menu overlay, bench web UI), VCOM is set to the panel's label value
(**-1.48 V**), the Tesserae client is in (zero-touch pairing, ETag-gated frame
polling, 1.3 MB frames straight into PSRAM, heartbeat, button events), and the
knob and the four keys are driven over I2C with every input remappable from
the web page. Audio, battery sensing and MQTT follow (see
[HANDOVER.md](HANDOVER.md) for the plan and the bench facts).

## How it works

```
 Tesserae server ──(REST, http)──► Pico Plus 2 W ──(PIO SPI 12.5 MHz)──► IT8951 ──► 10.3" glass
   renders the dashboard,            PSRAM frame buffer 1,314,144 B
   dithers to 16 greys,              + menu / banner overlays (row generator)
   packs 4 bpp (esp32_gray_bin)      + bench web UI on port 80
```

- **Thin client.** The server renders and dithers; the device downloads the
  packed 4 bpp frame and streams it to the controller. No decode on the Pico.
- **Always on.** Mains powered (PowerBoost load-sharing), so the firmware uses
  the protocol's always-on variant: poll `/frame` every `awake_poll_s`, repaint
  only on a `200`, never deep sleep. A `304` costs one small request.
- **Overlays without a frame buffer.** Everything on the glass comes from one
  row generator: dashboard (or test card), then the status banner, then the
  menu. Partial updates slice rectangles out of it (A2 ≈ 0.2 s per menu move).
- **Pairing without typing.** The device announces itself; the admin clicks
  Register once in Settings → Devices; the token arrives on the next announce
  and is kept in flash. A re-flash re-pairs by MAC with no click.

The protocol as used here is written up in [docs/tesserae.md](docs/tesserae.md).

## Build and flash

Needs [PlatformIO](https://platformio.org/) (the `maxgerhardt` RP2350 platform
fork and the Pico SDK are fetched on first build).

```sh
cp include/secrets.h.example include/secrets.h      # WiFi, server URL, device id
pio run                                              # .pio/build/pimoroni_pico_plus_2w/firmware.uf2
tools/flash.sh [log seconds]                         # hands-free: 1200-baud reset, copy UF2, tail the log
pio device monitor                                   # 115200, DTR asserted
```

`tools/flash.sh` builds, resets the board into BOOTSEL over the USB CDC port,
copies the UF2 to `/Volumes/RP2350` and tails the serial log. If the board
drops off USB during the reset (it happens; the handover saw it too) it needs
a physical replug: hold BOOTSEL while plugging in and the script's wait for
the volume picks it up.

`secrets.h` is a **first-boot seed**: its values are written into a flash
config sector and can be changed later from the web UI without a rebuild
(the server URL, device id, display name, pairing code, WiFi for the next
boot). Everything else is a build flag; the list is at the top of
[platformio.ini](platformio.ini). The important one is already set:

```
-DEPD_VCOM_MV=1480     ; "-1.48V" on this panel's FPC label
```

## First run

1. Boot. The panel does an INIT clear and shows the test card. **Check by
   eye that the black square is top-left**; if it is top-right, tick *Mirror*
   in the web UI (saved to flash) or build with `-DEPD_MIRROR_H=0`.
2. The banner at the bottom-left shows the web UI address and what the device
   is waiting for (`register bedside in Tesserae`, `bind a dashboard`, ...).
3. In Tesserae, Settings → Devices → **Discovered** → Register. The device
   announces itself as the server's Seeed reTerminal E1003 kind (same glass,
   same controller, same 16-grey frame; see docs/tesserae.md for why and for
   the bedside catalog entry). On the card set *Stay awake* on with a 10 to
   15 s awake poll, bind a 1872x1404 dashboard, Send. Within one poll the
   dashboard replaces the test card and the banner disappears. If the banner
   says *bad frame from server*, the device was registered under a kind or
   panel that does not match: delete it in Settings → Devices and let it
   re-pair.
4. Open `http://<device ip>/` for the room controls, the Tesserae state
   (paired, ETag, poll cadence, server local time), the tunables, and a
   Console tab with the live serial log and the serial commands.

## The UI

The click-wheel design, after the iPod (mock-up linked in HANDOVER.md). Text is
Albert Sans and icons are Phosphor Bold, both 4-bit bitmaps generated by
`tools/gen_font.py`. Every control means one thing on every screen:

| Control | Does |
| --- | --- |
| Knob turn | scroll a list, or move a value; on the dashboard, the volume |
| Knob press | choose (the wheel's centre button) |
| Knob hold | back, the same as MENU |
| Key 1 ⏮ / key 3 ⏭ | previous / next dashboard, from anywhere |
| Key 2 ▶︎❙❙ | rain play / pause; hold for the bedroom lights |
| Key 4 MENU (orange) | back one level; hold to go straight home |
| Alarm ringing | knob press stops it, anything else snoozes |

Screens: the server dashboard between a top bar (title, key icons) and a status
bar (volume, and on the dashboard what is playing, lights, A/C, alarm); a split
list (rows left, the highlighted row large on the right) for Menu, Rain,
Lights, A/C, Alarm and Settings; a value screen with a scrubber; a choice list
with a check on the current option. Screen changes use the full-quality GC16
refresh; moves within a screen use GL16, one refresh each.

## Controls

Hardware on the Qw/ST connector (I2C0, GP4 SDA / GP5 SCL), polled every 15 ms,
hot-pluggable:

| Part | Address | What |
| --- | --- | --- |
| Adafruit I2C QT Rotary Encoder (#5880) | 0x36 | detented knob, push button (seesaw pin 24), one NeoPixel ring light |
| Adafruit NeoKey 1x4 QT (#4980) | 0x30 | four MX key sockets (seesaw pins 4..7), four NeoPixel backlights |

**The dashboard keys are remappable.** Keys 1 to 3 on the dashboard (press
and hold) and the BOOT button run one action each, chosen on the web page
under *Mapping* or on the device's Keys page, and saved to flash. The knob and
the orange key are fixed by the interface. The actions: the toggles and page openers above, menu /
select / back, volume, refresh, night lights, backlight, re-pair, the panel
housekeeping, or *Server button name*, a button wake the server's
**Controls** map turns into a page, a rotation step or a Home Assistant
action. A key with no hold action fires on the way down; one with a hold
action fires on release unless the hold fired first (600 ms). Backlights:
warm white with one orange key by day, dim red at night.

**Web page** (`http://<device ip>/`): Room (sound, lights, A/C, the dashboard
picker), Alarms, **Console** (the device's log, the last 16 KB, polled from
`GET /api/log?from=<pos>`; typed text runs through the same handler as the USB
serial port via `/api/cmd?serial=<text>`, and `enter=1` is a knob press), and
Device (keymap, home screen, Tesserae pairing and status, panel tuning, WiFi).
The old Remote tab (glass preview and virtual controller) is gone;
`GET /frame?s=N` still serves the preview for tools.

| Other input | Action |
| --- | --- |
| Serial | `m` menu, `j`/`k` turn the knob, Enter press, `q`/Esc hold, `1`-`4` press key, `f` fetch now, `t` test card, `g` dashboard, `c`/`w` clear, `v1480` set VCOM, `V` read VCOM, `i` re-init, `z` sleep, `e` enhance driving, `d` pin discovery, `p` pairing status, `?` help |
| Web `/api/cmd?k=v` | `knob=cw\|ccw\|press\|hold`, `key=1-4`, `keyhold=1-4`, `page=<menu\|dash\|ac\|lights\|sound\|alarm\|weather\|status\|keys\|home>`, `scenario=<prewake\|ring\|stop\|power\|mains>`, `action=<spec>`, `bind=<input>\|<spec>`, `label=<1-4>\|<text>`, `backlight=0-255`, `night=0/1`, plus `server=`, `device=`, `name=`, `kind=`, `pair=`, `forget`, `fetch`, `button=<name>`, `wifi=ssid\|pass`, `reboot` and the panel commands |

## Layout

| Path | What |
| --- | --- |
| `src/epd_io.c`, `include/epd_io.h` | PIO SPI (integer divider), CS/RST/HRDY, DMA write with RX drain; adapter pin map |
| `src/it8951.c`, `include/it8951.h` | IT8951 init with pin probing and speed fallbacks, VCOM, mirror, full/partial paints from a row generator, sleep |
| `src/psram.c` | QMI PSRAM bring-up (frame buffer at `PSRAM_XIP_BASE`, preview at +4 MB) |
| `src/net_rest.c`, `include/net_rest.h` | Tesserae v1 REST client on raw lwIP TCP: discover/register/frame/status, streaming download, Date-header clock |
| `src/http_chunked.c`, `include/http_chunked.h` | HTTP/1.1 chunked-transfer decoder used by `net_rest.c` (GitHub answers chunked now and then), host-tested |
| `src/tesserae.c`, `include/tesserae.h` | pairing state machine and the always-on poll/paint/heartbeat cycle; button event queue |
| `src/config.c`, `include/config.h` | flash config sector (WiFi, server, id, token, pairing code, ETag, button counter, mirror, bindings, key labels, backlight) |
| `src/rainsynth.c`, `include/rainsynth.h` | Procedural sleep sounds (rain, ocean, noise) with thunder, surfaces, wind and tone controls, plain C. `tools/render_rain.c` renders it on a Mac to audition and level-check it. |
| `src/birds.c`, `include/birds.h`, `assets/birds/` | Dawn chorus from real recordings of six Australian birds plus four synthesised ones, arranged live (clip, timing, pitch, distance, reverb). `tools/make_bird_clips.py` cuts the clips from the Commons sources; `scripts/build_birds.py` packs them as 8-bit mu-law (1.7 MB) into flash at build time. Credits in NOTICES.md. |
| `src/ha.c`, `include/ha.h` | Home Assistant over MQTT with discovery: 78 entities (sleep sounds, rain and thunder, dawn chorus and each bird, four alarm slots, lights and A/C, night mode, key lights, dashboard, buttons, diagnostics, an Events entity). Broker set in the web app's Device tab. Uses lwIP's MQTT client via `src/vendor/lwip_mqtt.c`. |
| `src/live.c`, `include/live.h`, `docs/live-dashboard.md` | The Live dashboard's values: one MQTT source per tile (an HA entity via Statestream, a topic, or `bedside/live/<tile>`), the bedroom trend, sunrise. The page itself is `build_live()` in `src/local.c`. |
| `src/audio.c`, `include/audio.h`, `src/spk_dsp.c` | Runs the synth from a timer interrupt on core 1 into a sample ring; the speaker (MAX98357A over PIO I2S, two 93 ms DMA buffers so a flash write never cuts the sound) plays it through the tone stage in `spk_dsp.c` (low cut, bass and treble shelves, a soften low-pass, level trim, peak limiter; Settings > Device > Speaker tuning, or the web app's Speaker page). `GET /audio` streams the ring as raw s16le mono PCM at 22.05 kHz for the web app's Listen button. |
| `src/painter.c`, `include/painter.h` | Panel driver on core 1: one paint job at a time from a snapshot, so core 0 stays responsive. Flash writes park it via multicore lockout (see HANDOVER). |
| `src/net.c`, `include/net.h` | WiFi join + raw-lwIP HTTP server (`/`, `/api/state`, `/api/cmd`, `/frame?s=N`) |
| `www/app.src.html`, `www/desktop.src.html`, `www/icons.js` | The web app, twice: the phone layout (Room, Alarms, Console, Device) and a desktop layout (sidebar pages: Overview with the panel preview, Sound, Speaker with the response curve, Alarms, Display, Device, Console) served at `/desktop`, which wide screens are sent to. The controls exist once, in `app.src.html` (marked `<!--part:name-->`); `desktop.src.html` places them (`<!--@name-->`) and shares the script and stylesheet. `scripts/build_www.py` assembles both and inlines the Phosphor icons before every build; edit the `.src` files, never `index.html` / `desktop.html`. |
| `src/seesaw.c`, `include/seesaw.h` | Adafruit seesaw register access on I2C0 (GPIO, encoder, NeoPixel) |
| `src/inputs.c`, `include/inputs.h` | encoder + NeoKey polling, debounce, press/hold events, backlights and night palette, hot-plug |
| `src/ui_*.c`, `include/local.h`, `include/ui_internal.h` | the device-drawn UI, one module per concern: `ui_core.c` (state, option tables, dirty regions, rebuild, repaint regions, ticks), `ui_draw.c` (the draw list and row rendering), `ui_menu.c` (the click-wheel menus: tree, row model, top bar, navigation, every input), `ui_live.c` (the Live dashboard, status bar, date, clock), `ui_overlay.c` (the home-screen overlay, toasts, the update card), `ui_alarm.c` (alarm clock, dawn chorus, sleep timer), `ui_api.c` (web-app direct control, HA views, settings persistence, power, state JSON). `local.h` is the public API; `ui_internal.h` is what the modules share |
| `src/draw.c`, `src/fonts/*.c`, `include/bfont.h` | per-row drawing primitives and the anti-aliased Albert Sans bitmap fonts (30/34/46/96 px text, 180/270 px numerals) generated by `tools/gen_font.py` |
| `src/menu.c`, `src/vendor/font24.c` | the bottom-left status banner (Font24) |
| `src/patterns.c`, `src/sample_card.bin` | test patterns and the embedded test card |
| `src/main.c` | bindings, commands, content/overlay composition, state JSON, main loop |
| `tools/` | `flash.sh` (build + hands-free flash + log; asks the device for the bootloader over WiFi first), `gen_font.py` (TTF → 4-bit bitmap font), `gen_sample.py` (test card), `png2frame.py` (PNG → packed 4 bpp frame) |
| `tesserae/hardware/waveshare_103_it8951_pico.json` | hardware catalog entry for the server (optional; see docs/tesserae.md) |
| `docs/tesserae.md` | the protocol digest |

## Hardware facts (verified on the bench)

Adapter pin map (the vendor PDF is wrong; found by the firmware's pin discovery):
SCK GP10, MOSI GP11, **MISO GP12**, CS GP8, RST GP17, **HRDY GP1**. HRDY stays
low ~820 ms after reset. 12.5 MHz SPI is clean; 18.75 MHz corrupts reads.
Full-frame GC16: ~845 ms stream + ~630 ms refresh. A2 partial of the menu
box: ~0.2 s + ~0.2 s. Controller reports LUT `M841_TFA5210`, image buffer
`0x00119F00`. Details and timings in [HANDOVER.md](HANDOVER.md).

## Known limitations

- Requests are synchronous: a poll blocks the main loop for up to a few
  seconds (frame download ≈ 1 to 3 s). The knob and keys are not serviced in
  that window (the encoder's delta register still accumulates detents, so
  turns are not lost, only delayed). Move the client to core 1 or make it
  event driven.
- The APDS-9960 gesture sensor was dropped from the build (1 Nov 2026).
- Weather on the device's own Weather page is sample data.
- Nothing is published over MQTT yet; the alarm does not actually fire.
- Plain `http://` only. The server must be reachable by IP or a name the
  LAN's DNS resolves; `.local` names do not.
- `power_plugged` is reported `true` unconditionally and no battery voltage
  is sent until the PowerBoost sense lines are wired.
- No `overlay`/patch or deck-cache capabilities are advertised; every change
  is a full GC16 repaint. Fine at the bedside cadence; revisit with the
  server-drawn key labels.

## Licence

AGPL-3.0-or-later. Third-party components and ported code are listed in
[NOTICES.md](NOTICES.md).

## Releases

`.github/workflows/build.yml` builds every push and pull request (the image is a
workflow artifact). To publish a release, push a version tag:

```sh
git tag v0.2.0 && git push origin v0.2.0
```

`release.yml` then builds with that version (the panel reports it as its
firmware version) and attaches `bedside-<version>.uf2` (USB install),
`bedside-<version>.bin` with its `.sha256`, and `manifest.json` (version, file,
size, sha256) for the over-the-air update check. CI builds never include
`include/secrets.h`, so an update can't overwrite the panel's stored WiFi or
server settings.

## Over-the-air updates

The panel installs releases from this repo's GitHub releases, over HTTPS
(mbedTLS, certificates checked against `src/tls_roots.c`):

- **Settings > Device > Software** on the panel, or **Device > Software** in the
  web app: the installed version, *Check GitHub*, the latest 10 releases (any
  can be installed, older ones too), and *Auto-update overnight* (03:10, the
  newest stable release if it is newer, never within 90 minutes of an alarm).
- How: the flash holds two firmware slots (A/B, `tools/pt.json`). An update
  downloads into the spare slot in 1 MB ranges, is checked against the
  release's `manifest.json` SHA-256 by reading the slot back, and the panel
  restarts into it *on trial*. If the new firmware doesn't start properly, the
  boot ROM goes back to the old one. Once it commits, the old slot is cleared.
- **One-time setup** for a panel flashed before this: `tools/partition.sh`
  (writes the partition table over USB; settings survive). After that
  `tools/flash.sh` loads with picotool.

## WiFi setup

With no WiFi saved, when the saved network can't be joined (then for 5
minutes, after which it tries again), or from **Settings > Device > Wi-Fi
setup**, the panel opens its own network, **Bedside-setup-xxxx**, and shows how
to connect. A phone that joins it gets the setup page by itself (or open
`http://192.168.4.1`): pick the WiFi, enter the password and the Tesserae
server, and the panel restarts onto your network.

## Picking a dashboard

With Tesserae 0.498 or later, **Menu > Dashboard** (and the Room tab in the web
app) lists *Live* plus every dashboard bound to this panel (`frames.json`,
fetched when the menu opens). Picking one shows it now without moving the
server's rotation; the next poll carries on with the schedule.

## Files on the panel

**Device > Files** in the web app keeps files in the flash after the two
firmware slots (about 4 MB, `src/store.c`), so updates never touch them and
OTA downloads stay small. Add one with the button or by dropping it on the
card. Sounds (MP3, WAV, M4A, ...) are converted in the browser by default to
22 kHz mono IMA ADPCM (`.adp`, about 0.66 MB a minute; format in
`include/store.h`), levelled, with the last 2 s blended into the start so the
file loops without a seam. The play button decodes it back in the browser.

API: `GET /api/files`; `GET /api/file?name=`; `POST /api/file?name=&off=&total=`
with up to 64 KB of body per chunk, in order, 4 KB aligned;
`POST /api/file?name=&delete=1`.

## Speaker and battery backup

Wired as in `docs/wiring-guide.html`; each has a switch in **Settings > Device**
on the panel and in the web app's **Hardware** card, and in Home Assistant.

- **Speaker** (MAX98357A): I2S from PIO on GP18 (BCLK), GP19 (LRCLK), GP21
  (data); GP16 holds the amp in shutdown whenever nothing is playing (plus 3 s
  so fades finish). The web stream keeps working alongside it.
- **Battery backup** (PowerBoost 1000C): battery on GP26, USB-C input on GP27,
  LBO on GP28, each through a 100 k / 100 k divider. Off by default: unwired
  ADC pins float. On: the strip shows the battery when mains drops, toasts on
  the change, and HA gets a Battery sensor and a Mains power binary sensor.

Sleep sounds gained **Wind** and **Recording** (loops a sound file from Device >
Files; thunder and the sleep timer work over it as over rain).

## Tests

`make -C tests` builds and runs the host tests (plain C, no Pico SDK): the
alarm decisions (`src/alarm_sched.c`), the file store on a simulated flash
chip, recording playback and level limits in the sound engine, SHA-256 and
release version ordering. GitHub runs them on every push before the firmware
build. On this Mac the Makefile picks the MacOSX26 SDK (the 27 one won't link
host code).

## Menus: quick moves and turn counts

- **Quick menu moves** (Settings > Keys and knob; web app Controls card; on by
  default): a knob step repaints only the rows that changed, in the panel's
  two-level mode (about 0.6 s). Once the knob rests for half a second the list
  and the preview repaint in full quality. Off: every step is a full-quality
  paint (about 1.5 s).
- **Turn counts** (same places; on by default): beside each row, the knob
  turns to reach it from the highlighted one; + is clockwise.
- Opening or closing a menu repaints the content area in one pass without the
  INIT clear (no flash), about 1.8 s.
