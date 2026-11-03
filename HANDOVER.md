# Handover: from the bench driver to the bedside device firmware

*29 October 2026. Written for whoever works on `tesserae-device-bedside`: the
device firmware for the Bedside Dashboard proposal (Jorgo). This note records
what the bench project proved on real hardware, what was lifted from it, and
what the proposal still needs. It is deliberately specific about pins, timings
and gotchas so none of today's discovery has to be repeated.*

> **Update, 29 October 2026 (evening).** This repo now exists as a working
> tree: the bench driver was copied in whole, VCOM is set from the label
> (**-1.48 V**, `-DEPD_VCOM_MV=1480`), the Tesserae client (Phase 2's "pulls a
> first dashboard") and the knob/keys (Phase 2) are implemented, and the
> **first flash ran on the panel**: VCOM read back 1480 mV, INIT clear and test
> card painted (orientation not yet confirmed by eye), PSRAM 8 MB, rotary
> encoder (0x36) and NeoKey (0x30) both answered with seesaw id 0x55 (SAMD09),
> WiFi joined, discover → Register → token → polling all worked. What did
> not: the device was registered under bare `esp32_client`, which gave it an
> 800x480 Spectra 6 panel on the server and 192,000-byte colour frames; the
> firmware refused them ("frame size wrong"). Fix: the kind now defaults to
> `seeed_reterminal_e1003` (same glass, panel fixed by the server's catalog);
> after a reflash the server re-provisioned the device at 1872x1404 gray_16
> on the next discover. **First dashboard frame painted at 15:43**: a
> 1,314,144-byte `esp32_gray_bin` frame downloaded in ~1 s, streamed in
> 846 ms, GC16 in 532 ms, ETag held, subsequent polls 304. Test page
> "Bedside (first light)" (page `cfbdd5120985`, bound to `bedside`) carries a
> clock and a 16-step grey ramp for judging greys at -1.48 V. Kayden then
> confirmed by eye that it all works. **Later that evening:** the UI from the
> proposal's mock-up was built on both sides: server pages "Bedside"
> (`34e7775087ac`, live Open-Meteo weather for Carlton) and "Bedside ·
> Tomorrow" (`14a3610dc061`) in rotation `bedside_pages`, Albert Sans cached
> on the server; and `src/ui_*.c` on the device (top bar with date / chips /
> key labels, menu sheet, Dashboards picker, AC / Lights / Sound / Alarm /
> Weather / Status / Keys pages, volume bar, wake-up note, alarm screen) with
> the mock-up's knob and key grammar. Verified on the glass: key toggles
> repaint the 150 px top bar in ~0.24 s (A2), the menu sheet opens with a
> full GC16 (~1.4 s), the volume bar repaints in ~0.6 s. Local state is
> sample data and MQTT publishes are log lines until Phase 3. After Kayden's
> review: text moved from scaled Font24 to anti-aliased Albert Sans bitmaps
> (`tools/gen_font.py`, `src/fonts/`, ~500 KB in flash) so the key labels are
> one size (34 px) and the pages read like the mock-up; and inputs are
> coalesced in `src/paint.c` (a burst of detents or presses updates the state and
> paints once) so refreshes no longer bank up.
>
> **UI, final direction (29 Oct, night).** After several rejected concepts
> (radial menu, key tabs, live tiles), Kayden picked "Bedside pages":
> https://claude.ai/artifact/FfJVR3WbBC3776iGZm2pof . Knob = volume, press =
> list menu on the right, pages full width with labelled quick keys, orange =
> Menu / Back / Stop, and a dedicated status bar along the bottom with the
> volume. Phosphor Bold icons (bitmaps in `src/fonts/icon34.c`, `icon46.c`,
> codes in `include/icons.h`). Implemented in `src/ui_*.c`; the server pages
> now leave 150 px at the top and 130 px at the bottom for the device's bars.
> The 1200-baud reset stopped working on the board that evening (picotool's
> reset too); `tools/flash.sh` now asks the firmware for the bootloader over
> WiFi first (`/api/cmd?bootsel`), so only the first flash of that build
> needs BOOTSEL by hand.
>
> **Responsiveness and a cleaner UI (later that night).** Measured, then fixed:
> repaints were whole strips or the whole screen, with flashing GC16, a
> second volume repaint, and a dirty-rect union bug that turned a key toggle
> into a full-screen repaint. Now: a list of small dirty rectangles, all
> loaded into the IT8951 and refreshed once over their bounding box
> (`it8951_update_areas`), non-flashing GL16 (mode 3) for UI changes, a GC16
> clean-up after 40 partials when idle. Measured per interaction: volume or a
> toggle ~0.6 s, menu move ~0.6 s, page open ~1.3 s (the body has to be
> streamed); GL16's ~0.5 s refresh is the floor. Input never replays: turns
> made during a paint arrive as one summed delta, input made while the screen
> changed is discarded (`local_epoch` / `inputs_discard`), and key taps the
> NeoKey latched during a paint are honoured only if under 300 ms old. The
> REST client services the UI while it waits (`rest_set_idle_hook`) and
> frames download into a second PSRAM buffer, so the knob no longer freezes
> during a server poll. All on core 0; core 1 is kept for audio. The UI was
> stripped back: no key numbers, no hints or footers, page name in the top
> bar, a single volume bar, status icons only when unusual.
>
> **UI, final (29 Oct, late).** Replaced "Bedside pages" with the click-wheel
> design Kayden chose (https://claude.ai/artifact/C9LQZGki2gwZMCTczx94ZZ),
> keeping the physical 4x1 keys: ⏮ · ▶︎❙❙ · ⏭ · MENU (orange), knob = wheel and
> centre button. iPod-style split list with a large preview, value scrubber,
> choice list. Measured: list scroll ~1.0 s (one GL16 refresh over rows and
> preview), value change ~1.0 s, screen change ~1.5 s (GC16), volume ~0.6 s.
> The server's dashboard 'Sound / Lights / AC / Alarm' columns were removed;
> the device's status bar shows that state. See `README.md` for the run-through and `docs/tesserae.md` for
> the protocol as used. The rest of this note is the original bench handover,
> with the sections below amended where the picture changed.

## Where things are

| What | Where |
| --- | --- |
| Bench driver (this handover's source) | `~/Documents/Projects/Jorgo/ed103tc2-pico-driver`, public at https://github.com/dmellok/ed103tc2-pico-driver |
| Spectra 6 sibling, same board and adapter | https://github.com/dmellok/el133-pico-driver |
| Existing Pico firmware with the Tesserae server pull (el133's parent, Pico SDK, C) | https://github.com/dmellok/tesserae-device-pico-bin |
| `tesserae-device-firmware` (checked 29 Oct: it is the **ESP-IDF** unified firmware for ESP32 boards, so it does not replace pico-bin as the Pico base; its `seeed_reterminal_e1003` board is the same IT8951 / ED103TC2 glass and its REST client shaped ours) | https://github.com/dmellok/tesserae-device-firmware |
| Readable reference for the device protocol (Python daemon: MQTT subscribe, PNG frames) | https://github.com/dmellok/tesserae-device-pi-png |
| Client proposal, with plan, BOM and the 29 Oct progress note | https://gist.github.com/dmellok/a3edbc30c2d0f969744f1ebe6a66f69b |
| Interactive screen/keys mock-up | https://claude.ai/artifact/34JYWiCdWGYzborfu1rGqV |
| WiFi credentials used on the bench | `ed103tc2-pico-driver/include/wifi_secrets.h` (gitignored) |

## Hardware facts verified on the bench

Board: Pimoroni Pico Plus 2 W (RP2350B, 16 MB flash, 8 MB APS6404 PSRAM on
QMI CS1 / GP47). Panel: Waveshare 10.3" e-Paper HAT, IT8951 controller, glass
ED103TC2, 1872x1404, 16 greys. Adapter: Hard Stuff "Pico to Pi Hat".

**Adapter pin map** (found by the firmware's own pin discovery; the vendor
pinout PDF is an unrelated ESP32-style table and is wrong):

| HAT signal | Pi BCM | Pico GP | Note |
| --- | --- | --- | --- |
| SCK | 11 | GP10 | SPI1 SCK |
| MOSI | 10 | GP11 | SPI1 TX |
| MISO | 9 | **GP12** | SPI1 RX, not GP9 |
| CS | 8 | GP8 | |
| RST | 17 | GP17 | active low |
| HRDY | 24 | **GP1** | HIGH = ready. GP23/24/25/29 are the RM2 wireless pins, so the adapter reroutes BCM23-25 (BCM25 reportedly GP28) |

Everything else the el133 verified (BCM22/26/27/16/15 to GP22/26/27/16/15) is
1:1. So the Pi SPI0 lines land on the Pico's SPI1 hardware pins; the bench
driver uses PIO SPI anyway, which works on any GPIO.

**Controller:** GET_DEV_INFO returns 1872x1404, image buffer `0x00119F00`,
FW `SWv_0.1.`, LUT `M841_TFA5210` (the 10.3" waveform; A2 = mode 6, GC16 =
mode 2, INIT = mode 0). HRDY stays low for **~820 ms after reset** while the
IT8951 boots; Waveshare's 200 ms is not enough on its own.

**SPI speed:** 12.5 MHz (PIO divider 3) is clean. 18.75 MHz (divider 2)
returned corrupted device info that still looked plausible (2040x1406), so
the init insists on three identical, printable device-info reads before it
accepts a speed. Fractional PIO dividers jitter SCK below the IT8951's minimum
pulse width; use integer dividers.

**Timings at 12.5 MHz with DMA double-buffering:**

| operation | stream | refresh |
| --- | --- | --- |
| full frame, GC16 | ~845 ms | ~630 ms |
| full frame, INIT clear | ~845 ms | ~1.9 s |
| 912x688 menu box, GC16 | ~205 ms | ~590 ms |
| 912x688 menu box, A2 | ~205 ms | ~190 ms |
| on-device JPEG 1872x1404 decode + dither | 7.8 s | |

**Still unconfirmed by eye** (the panel was driven headless from the serial
console): the horizontal mirror default (`EPD_MIRROR_H=1`, Waveshare's "mode
1" for the 10.3"), and grey rendition. **VCOM is -1.48 V** from the FPC label
and is now the build default (`-DEPD_VCOM_MV=1480` in `platformio.ini`); the
controller had reported 0.00 V on the bench, so the first flash of this
firmware is the first time the panel runs at its rated VCOM. Judge greys
only after that.

**Buttons:** the Pico Plus 2 W's BOOT button is on **GP45** (active low) as
well as the flash-CS BOOTSEL trick; the bench driver reads both. The HAT has
no buttons.

**Bench flash loop without touching the board:** the pico-sdk USB stdio
accepts the 1200-baud reset. `pio run`, open the CDC port at 1200 baud, wait
for `/Volumes/RP2350`, `cp -X firmware.uf2` there, then read the log with
pyserial (DTR asserted). Scripted as `tools/flash.sh` in this repo. Once the
board was lost from USB mid-reset and needed a physical replug, so keep that
in mind for unattended runs; it happened again on 29 Oct on the first flash
attempt of this firmware (the 1200-baud open returned "Device not configured"
and the board never re-enumerated).

## What to lift from `ed103tc2-pico-driver`

The bench driver is AGPL-3.0-or-later with third-party pieces listed in its
`NOTICES.md` (picojpeg public domain, Font24 and the PIO SPI / BOOTSEL code
BSD-3, Waveshare's IT8951 sequence MIT, MicroPython's PSRAM init MIT).

- `src/epd_io.c`, `include/epd_io.h`: PIO SPI (modes 0 and 3, integer
  divider), CS/RST/HRDY, DMA write with a second channel draining RX so the
  state machine never stalls. Reusable as is.
- `src/it8951.c`, `include/it8951.h`: init with pin probing and speed
  fallbacks, VCOM, run-time mirror, `it8951_show_pattern()` (full frame from a
  row generator), `it8951_update_area()` (any rectangle, x and w widened to
  16 px, mirrored in panel space), sleep, enhance-driving. Reusable as is; the
  diagnostics (`it8951_probe_matrix`, `it8951_probe_blind`) can stay for field
  debugging.
- `src/menu.c`: per-row compositing of a menu box and a one-line banner using
  Font24 at 2x. The pattern to keep is the **row generator**: the panel is
  always fed from `composite_row(y)` = content, then overlays, so overlays need
  no frame buffer and partial updates just slice rows. The proposal's key
  labels, status badge ("on battery") and local pages fit the same shape.
- `src/img_decode.c`: picojpeg to luma in PSRAM, Floyd-Steinberg to 16 levels.
  A Tesserae-served frame can skip this if the server sends packed 4bpp.
- `src/psram.c`: QMI bring-up, verbatim from MicroPython.
- `src/net.c` + `www/index.html`: WiFi join and a raw-lwIP HTTP server. Worth
  keeping as the on-device tuning page even in the product firmware. Note the
  PlatformIO picosdk builder always links the **threadsafe-background** cyw43
  arch: lwIP callbacks run from an interrupt-driven context, so they only
  queue commands and fill small buffers; slow work happens in the main loop
  under `cyw43_arch_lwip_begin/end`. `include/lwipopts.h` is the project's
  (the builder's btstack copy is later on the include path).
- `tools/`: `gen_sample.py`, `png2frame.py`, `make_sample_jpg.py`,
  `fetch_sample_jpg.py`. Frame format: packed 4bpp, high nibble = even
  column, 936 bytes/row, 0x0 black .. 0xF white, logical left-to-right.

Resources the bench firmware already uses on the RP2350: PIO0 one state
machine, two DMA channels, GP47 for PSRAM, GP23/24/25/29 for the radio, about
155 KB of SRAM with lwIP (82 KB of that is the SRAM preview buffer, movable to
PSRAM).

## What the proposal needs next

Mapped to the gist's plan. Phase 1 is done; Phase 2's panel bring-up is done;
Phase 2's Tesserae pull is implemented (unverified on the panel); Phase 3's
overlay mechanism is proven.

1. **Tesserae pull (Phase 2).** Done in code, 29 Oct: `src/net_rest.c` (the
   pico-bin REST client, ported, with a streaming sink for the 1.3 MB frame)
   and `src/tesserae.c` (zero-touch pairing, always-on poll, ETag/304,
   heartbeat, button events). Decision taken: **server-side dithering**. The
   server's `esp32_gray_bin` renderer (built for the Seeed E1003, same glass)
   emits exactly the packed 4bpp row format `it8951.c` streams, so the device
   does no decode; `img_decode.c`/picojpeg were dropped. The device registers
   as `kind: seeed_reterminal_e1003` (runtime setting; bare `esp32_client`
   got an 800x480 Spectra 6 panel from the Register card on the bench, so
   never that) with `gamut: gray_16`; the bedside's own catalog entry is in
   `tesserae/hardware/` for the server. Remaining: flash and verify end to end,
   then move the synchronous requests off the main loop (core 1 or event
   driven) before the knob arrives, because a poll blocks inputs for 1-3 s.
   lwIP gotcha found on the way: the picosdk builder compiles lwIP core against
   the bundled btstack `lwipopts.h` unless the project include dir is
   prepended (`scripts/lwip_prepend.py`), which shows up as
   `dns_gethostbyname` failing to link.
2. **Knob, keys, gesture (Phase 2).** Knob and keys done in code, 29 Oct:
   `src/seesaw.c` (Adafruit seesaw register map on I2C0 GP4/GP5),
   `src/inputs.c` (encoder delta + button, NeoKey keys with debounce and
   press/hold, NeoPixel backlights with a red night palette, hot-plug
   re-probe), key-label strip along the top edge in `src/menu.c`, and a
   bindings table in `src/main.c`: every input (key press/hold, knob cw/ccw/
   press/hold, BOOT short/long) runs an action chosen from the web page's
   Mapping table and saved to flash; "button:<name>" sends a server button
   wake. Unverified on the boards (no hardware session yet); the first thing
   to check is the seesaw HW_ID read (0x87 expected for both ATtiny817
   boards) and the encoder's direction sign. **The APDS-9960 gesture sensor was
   dropped (1 Nov 2026: not wanted).** The web page's virtual controller drives the same bindings, so
   the mapping can be set up and tested before the keys are soldered.
3. **Audio (Phases 2 and 4).** MAX98357A over I2S from PIO (pico-extras has
   `audio_i2s`); check the GPIO splitter's free pins against the pin map above.
   Stored loop first, then the generated rain and dawn chorus.
4. **Power (Phase 2).** PowerBoost 1000C mains-sense and battery-level lines
   through the splitter; "on battery" badge is one more overlay in
   `composite_row`.
5. **MQTT and HA (Phase 3).** Retained topics for key labels/actions, volume,
   alarm offset, menu items; HA discovery; alarm publish. lwIP's MQTT client
   (`lwip/apps/mqtt.h`) is in the SDK tree; the builder only compiles
   `api`, `core`, `port` and `netif/ethernet.c`, so the `apps/` source must be
   added to the build.
6. **Local pages (Phase 3).** Alarm, AC, lights, sound, settings as more
   row-generator overlays; A2 for navigation, GC16 when leaving a page.

## Suggested first steps in this repo

1. ~~Start from a copy of `ed103tc2-pico-driver`~~ Done.
2. ~~Set `-DEPD_VCOM_MV` from the label~~ Done (1480). **Flash, and confirm
   orientation on the test card by eye (black square top-left) before anything
   else.** Then `V` on the serial console should read back -1.48 V.
3. ~~Replace the demo screens with the Tesserae client~~ Done in code. Verify:
   `cp include/secrets.h.example include/secrets.h`, fill in WiFi and the
   server URL, flash, Register the device in Tesserae, bind a 1872x1404
   dashboard, confirm it paints and that `304`s are cheap at a 10 s poll.
4. Only then add I2C inputs, audio and MQTT, one at a time, each verified on
   the bench web page. The knob maps to `menu_move`/`menu_select`/`menu_close`
   and the keys to `tesserae_button("<name>")` in `src/main.c`.

## 2026-10-29: knob freeze fix and mobile web app

- **Knob freeze.** One failed I2C read used to mark the encoder lost and park it for 5 s, so the knob and everything driven by it went dead. Seesaw reads now retry three times. A board counts as lost only after 30 failures in a row, and it is re-probed every second. A soak test went from frequent dropouts to none.
- **Stall logging.** The main loop prints `stall: <phase> took N ms` whenever one phase runs past 400 ms, to catch any other freeze source over serial.
- **Command queue.** Web commands go into an 8-slot ring, so a burst from the app no longer overwrites a pending one.
- **Web API.** `/api/cmd?set=key|value` drives the local UI state: vol, rain_on, rain_lvl, rain_kind, sleep, lights_on, bright, colour, ac_mode, setpoint, fan, night, keylights, dash, alarm `i,h,m,on,daymask,early` (bit 0 = Monday), alarm_add, alarm_del. `/api/state` carries the same state under `ui`.
- **Mobile app.** Served at `/`. It polls state every 1.5 s, updates optimistically, and ignores polled values for a field for a few seconds after you touch it. The source is `www/app.src.html`.

## 2026-10-29: panel driver on core 1

- **What moved.** `src/painter.c` runs the IT8951 on core 1: streaming pixels and waiting out each refresh. Core 0 keeps the knob, keys, network and UI state and stays responsive during paints. In a stress test, state requests during paints answered in under 100 ms.
- **Jobs.** One at a time. Core 0's `paint_pump()` in `src/paint.c` submits the next job only when core 1 is idle, so changes made during a paint are painted once, as their latest state. Ten volume changes in half a second produced three paints.
- **Snapshot.** Core 1 paints from a frozen copy of the UI draw lists (`local_snapshot()`, about 80 KB SRAM) and of the content, frame pointer and banner. Core 0 can rebuild freely. Each core has its own scratch line, because the web preview renders on core 0 at the same time.
- **Page changes.** While a page-change paint runs, input stays unread and is dropped at the end, as before. During small updates, input acts immediately.
- **Frame buffers.** Tesserae waits before downloading into the buffer core 1 is still painting (`tesserae_set_buffer_guard`).
- **Flash writes.** `config_save()` parks core 1 with `multicore_lockout_start_blocking()`. `flash_safe_execute()` does not do this under PlatformIO, because the build never sets `PICO_FLASH_SAFE_EXECUTE_PICO_SUPPORT_MULTICORE_LOCKOUT`. Erasing flash with core 1 running hung the painter. Don't write flash any other way.
- **BOOTSEL read removed.** The QSPI chip-select trick cuts flash off for both cores. The BOOT button is read on GP45 instead, which is the same button on the Pico Plus 2 W.
- **Direct panel calls** from core 0 (VCOM, init, sleep, diagnostics) take `painter_lock()`.
- **Audio, later.** Put it on core 1 in DMA interrupt context at a higher priority than the painter loop. The painter is thread-mode background work, and the IT8951 tolerates pauses in SPI. Keep the inter-core FIFO free, because the flash lockout uses it.

## 2026-10-29: sleep sounds, streamed to the web app

- **Synth.** `src/rainsynth.c` is procedural. Rain is a filtered-noise wash with gusts, plus individual drops: noise ticks ringing damped resonators, with occasional near, low plops. Ocean is brown noise through a filter that opens with each wave, with foam on the break. Noise is brown-to-pink by intensity. All changes glide over about a second, so nothing clicks. Levels at full volume sit between −24 and −14 dBFS.
- **Where it runs.** A repeating timer on its own hardware alarm, created on core 1, renders 10 ms blocks into a 0.74 s ring. It tracks a real-time sample clock, so it can't drift, and after a stall it catches up by at most 200 ms. It renders only while someone listens and costs about 14% of core 1. The interrupt pre-empts the painter.
- **Streaming.** `GET /audio` sends endless raw PCM: signed 16-bit little-endian, mono, 22.05 kHz, about 44 KB/s, in batches of at least 46 ms. The web app plays it through Web Audio with about 0.35 s of buffering. To support it, the lwIP heap went to 32 KB and the server to 6 connections.
- **Sleep timer.** Now implemented in `local_sleep_tick()`. It arms when sound turns on or the timer changes, fades over the last minute, then turns the sound off on the panel too. `ui.rain.sleep_left_s` in the state shows the time left.
- **When a speaker lands.** Feed an I2S DAC from the same ring by DMA. Move `on_timer` and `rs_render` to RAM with `__not_in_flash_func`. Defer `config_save()` while playing: the flash lockout pauses core 1 for the erase, and the ring only covers about 0.7 s.
- **Build.** `scripts/build_www.py` stamps the page's hash into `src/www.S`. SCons rebuilds by content, so before this, edits to the page did not reach the firmware.

## 2026-10-29: rain controls, thunder, config moved

- **Rain controls.** `rs_params_t` gains patter, wash, tone, wind, surface (open ground, leaves, tin roof, window), thunder (how often), thunder_near (far rumble to overhead crack) and strike (roll now). The web app shows them under "Shape the rain". The panel's Rain menu gets "Falling on" and "Thunder" (Off, Distant, Storm). Thunder is several overlapping low swells through a filter that opens on each peak. Near strikes add a crack and crackle and briefly lift the rain. Distant thunder sits about 6 dB over the rain, and a close strike peaks about 14 dB over it. `tools/render_rain.c` renders every scene.
- **Settings survive reboots.** Volume, sound, intensity and every shaping control persist in flash blob slot 1 (`config_blob_*`, magic "SND2"), saved 4 s after the last change.
- **Config moved from 2 MB to 15 MB.** The firmware image is about 2.6 MB, so the old sector sat inside the embedded test card. Every save corrupted a strip of the card, and every flash wiped the config. `config_load()` migrates a valid old copy once.
- **SomaFM was built, then removed as out of scope.** The device had chosen stations and polled now-playing, and the phone played the stream. If a radio comes back, SomaFM's stream servers refused the device's self-identified client with 403. The phone's own browser played fine.

## 2026-10-30: dawn chorus and the device alarm clock

- **Recordings.** Six species: Australian magpie, grey butcherbird, superb fairy-wren, eastern whipbird, red wattlebird and laughing kookaburra. The clips come from Wikimedia Commons, all CC BY-SA 4.0 or public domain, with credits in NOTICES.md. I picked each clip from spectrograms, and the cuts are in `assets/birds/clips.json`. The fairy-wren reels come from a mixed woodland soundscape, band-passed above 4 kHz. The bell miner was dropped: the only suitably licensed recordings are distant and hissy. xeno-canto's API now needs a key, and Freesound had nothing isolated for these species. Regenerating the clips needs the decoded source WAVs: `tools/make_bird_clips.py <dir>`.
- **Engine.** `src/birds.c` runs six voices that play clips straight from flash (mu-law), with ±4% pitch and a random distance that sets gain, a low-pass and the reverb send, plus a small Schroeder reverb. The conductor brings species in by intensity: magpie at 0, butcherbird 0.12, fairy-wren 0.25, whipbird 0.4, wattlebird 0.55, kookaburra 0.75. Gaps shrink as intensity rises. Birds mix after the master volume, under their own "alarm volume" (`bird_vol`), and the sleep sound fades out as the chorus builds. Core 1 runs at about 15% with the chorus playing.
- **Alarm clock on the device.** `local_alarm_tick()` uses the server's clock: the Date header plus `tz_offset_seconds`, falling back to the offset in `local_time`. In each alarm's wake window ("wake up early", else 10 min) the chorus climbs from 0.05 to 0.8 and the phase is PREWAKE, with lights on. At the alarm time it rings at full. Knob press stops, a key or turn snoozes 9 min, it gives up after 30 min, and a "Once" alarm switches itself off. Tested on the device: alarm set 2 min ahead, build-up, ring, snooze and stop all worked.
- **Controls.** Panel: Menu → Alarms → Dawn chorus, with a switch per bird (switching one on plays it), alarm volume and preview. Web app, Alarms tab: the same, with a play button per bird, a 90 s preview, and Stop and Snooze while ringing. API keys: `birds` (mask), `bird_vol`, `audition` (bird index), `chorus_preview` (seconds, 0 stops), `alarm_stop`, `alarm_snooze`. Settings persist in blob slot 1, now magic "SND3".
- **Until there is a speaker,** the chorus is only heard through Listen on a phone, because the audio engine renders only while someone listens. With a DAC, render always.
- **Flash** is now 4.4 MB with the clips, and a flash takes about 2 minutes.

## 2026-10-30: synthesised birds, Home Assistant over MQTT

- **Synthesised birds.** Four species were added in `src/birds.c` for variety: bell miner (pings), spotted pardalote ("sleep-may-be"), golden whistler (rising whistles and a whip crack, or "ee-CHONG") and crimson rosella ("too-ee-too", or "cussik"). Each call is built fresh from notes: a sine with a little 2nd and 3rd harmonic, a pitch contour, attack, release and optional vibrato. They join the same conductor, distance and reverb as the clips, at matched levels. Saved settings from before switch them on once (the `pad` flag in the SND3 blob).
- **Home Assistant (`src/ha.c`).**
  - **Connection.** lwIP's MQTT client, compiled from the SDK by `src/vendor/lwip_mqtt.c`, with buffers raised in `lwipopts.h`. Broker settings are in the web app's Device tab, saved in blob slot 2 with magic "HAC1", and can be seeded from secrets.h `MQTT_*`. The password is hidden in logs.
  - **Discovery.** On connect it sends 78 retained configs under `homeassistant/<component>/bedside_<mac6>/<key>/config`, one HA device. It re-announces when HA's birth message (`homeassistant/status online`) arrives.
  - **Topics.** State is the `/api/state` "ui" JSON on `bedside/state`, published on change at most every 250 ms. Diagnostics go to `bedside/diag` every 60 s. Availability is `bedside/status`, with a last will of "offline". Events (prewake, alarm_ringing, alarm_snoozed, alarm_stopped, thunder, chorus_preview ...) go to `bedside/event` for HA's event entity.
  - **Commands.** HA writes to `bedside/set/<key>`. `ha_translate()` maps each one onto the web API's command path, so both behave the same. Selects send option indexes through command templates. Alarm slots use text entities: time as `HH:MM`, and days as "Mon Wed Fri", "Weekdays", "Weekends", "Every day" or "Once". A slot's entities are unavailable while that alarm doesn't exist.
  - **Tested** against a local Mosquitto: all 78 configs are valid JSON, and every value, command and availability template renders under Jinja2 against the real state. Commands for volume, selects, the half-degree setpoint, bird switches, alarm time, days, window, add and delete, and thunder all worked, and the event arrived. The birth message re-announces. Not yet tested against a real Home Assistant instance.
  - **Lights and A/C** are the panel's own intent state. Map them to real HA entities with automations.

## 2026-10-30: live dashboard (D1), new status bar, more settings

- **Live dashboard.** Dashboard 0, "Live", is drawn by the panel (`build_live()` in `src/ui_live.c`) over a white page (`CONTENT_LIVE` in `src/screens.c`). Dashboards 1 and 2 are the server's rotation. The keys cycle Live, Bedside, Tomorrow. Server frames that arrive while Live is up are kept for later.
  - **Layout,** from mock-up D1: the clock in a new 214 px light face, the next alarm with a countdown ("on Monday" when more than a day away), and the dawn chorus start. Below that, the tonight timeline: now, when the sleep timer stops the sound, sleep, the chorus, sunrise (computed on the device, NOAA equations, location in the web app), then the alarm. Then seven soft tiles: outside with an hourly rain chart, bedroom with a 12-hour trend, washing, security, bins, next event and A/C. A tile turns black only when it needs you.
  - **Drawing.** New ops: `OP_RRECT` (rounded fill), `OP_LINE` (thick segment) and `OP_DASH` (dashed rule). New fonts: `clock214` and `ui72`.
  - **Refresh.** The header repaints each minute (GL16). The header and timeline repaint when the alarms, sleep timer or sound change. A tile repaints when its value changes.
  - **Values** come from `src/live.c`. Each tile's source is an HA entity id (via MQTT Statestream), a topic, or `bedside/live/<tile>`. `ha.c` subscribes to them all. See `docs/live-dashboard.md` for the HA configuration.
  - **Tested** against a local Mosquitto broker: all 14 values arrived, a tile turned black on "done" and on an unlocked door, and the page repainted. The bedroom trend needs 30-minute samples and hasn't been watched over hours yet.
  - **printf has no floats in this build,** so `live_fmt_dec()` formats decimals. Don't use `%f`.
- **Status bar** (every screen). Left: volume and the sound. Right, right-aligned: the sleep timer's stop time, alarm, A/C and lights when set, then the HA and Wi-Fi icons. Night, battery and offline appear only when true.
- **Settings page.**
  - **Rows:** night mode, auto night (from 21/22/23:00 until 07:00, switching only at the edges so a manual change holds), key lights, key colour, menu key colour, night colour (9-colour palette, `inputs_set_colours()`), knob light, clock (24 or 12 hour), snooze (5/9/10/15 min), refresh screen, status, restart, and reset everything (confirm screen).
  - **Saving:** blob slot 4, magic "SET1". The web app's Device tab has the same settings, and they're in HA discovery too (85 entities now).
  - **Master reset** (`config_erase_all()`) erases the config and blobs 1 to 8, then reboots. The device re-seeds Wi-Fi and server from secrets.h and re-registers. It is not tested on the bench panel, because it would wipe the user's alarms and pairing.

## 2026-10-30: full refresh once input settles

- Partial (GL16) updates fade blacks a little each time. Any partial update the user causes, within 3 s of a knob, key, web or HA input, marks the screen as touched. Once input has been quiet for 4 s (`SETTLE_MS` in main.c), the panel does one full GC16 refresh.
- Background updates don't count: the live page's minute clock and HA values keep the slower clean-up (40 partials and 20 s idle), so nothing flashes every minute at night.
- Verified on the device: a volume change gave one partial paint and then one full refresh about 4 s later, and nothing after that.
- Still worth trying for faded pages: the IT8951's "enhance driving" register (`/api/cmd?enhance`), which the firmware never sets at start-up.

## 2026-10-30: Mk II UI (flashed to the bench panel)

Design and rules: https://claude.ai/artifact/Gxywayio7myr4sh2UwxMxH. Compiles clean
(`pio run`, RAM 87.1%). Flashed 30 Oct: strip, confirmation box and grouped Settings checked from the web preview; physical key holds and the card layouts not yet tried by hand.

- **Home screen.** On a server dashboard (not Live) the top bar and status bar are
  gone: the dashboard plus one overlay (`build_overlay()` in `src/ui_overlay.c`). Strip
  176 px along the bottom (default), cards down the left (432 px), cards along
  the bottom (324 px), or off. Items are one line each (icon + value): clock,
  alarm, sound, room, connection; default clock, alarm, sound. "At night: Clock
  only" keeps the same size. A lost connection always shows. Live keeps its own
  layout.
- **Viewport.** `tesserae_set_viewport()` from `overlay_apply()`; sent on the
  heartbeat as `viewport`; frames of the viewport's size are accepted and placed
  by `dash_row()` in `src/screens.c` (docs/tesserae.md). The server side is not done.
- **Confirmations.** `toast()`: a box over everything for 1.5 s after a key
  action or a volume change on the home screen, drawn in the ring layer (now
  composited last), expired in `local_live_tick()`.
- **Keys.** Keys 1-3 run their press / hold bindings (`key_run()` in main.c);
  `local_key()` only handles MENU and the ringing alarm. Defaults: press rain /
  lights / AC, hold `dash.prev` (new action) / `dash.next` / `night`. Untouched
  old hold defaults (open.sound/lights/ac) migrate on load (`migrate_holds()`).
  Knob hold: Back by default (`local_back_up()`, nothing on the home screen);
  any other binding replaces it. The top bar's key icons follow the bindings.
- **Settings, grouped.** Home screen, Night, Keys and knob (Key 1-3 → Press /
  Hold, Knob hold, MENU key, brightness, colours, knob light), Time and clock,
  Device.
- **Night.** No more red: the keys keep their day colour at 12% ("Keys at night:
  Dim") or go dark ("Off"). `night_col` is kept in the blob and HA but unused.
- **Time.** "Set automatically" (default: Date headers + Tesserae's offset), a
  zone override with AU/NZ daylight-saving rules (`tz_offset_at()`), and Time /
  Date set by hand from the board's timer when automatic is off. The hand-set
  time is not kept across a reboot (no RTC).
- **Settings blob** slot 4 is now "SET2" (16 bytes); SET1 migrates.
- **Web app** (Device tab): a keymap with Key 1-3 press / hold and Knob hold,
  a Home screen card (overlay, what it shows, the viewport sent), Keys at night,
  time zone and automatic time. `set=` keys: `night_keys`, `overlay`, `show`
  (bit mask), `at_night`, `tz`, `auto_time`.
- **Not done:** HA entities for the new settings; the server rendering at the
  viewport.

## 2026-10-30: Screen updates setting (flashed)

Settings → Device → **Screen updates** (web app: Device → Panel; `set=refresh_mode|0..2`;
saved in the SET2 blob's `refresh_mode` byte): **Fast** (partial GL16/A2, then GC16 once
input settles: the old behaviour), **Clean** (every change painted with GC16, just its
areas), **Full** (every change repaints the whole panel with GC16). Applied in
`paint_pump()` in `src/paint.c`. Clean is the default (Kayden found it best; Full "didn't seem to work" on the glass, cause not yet known). Under Full, each change is ~1.5 s with
a flash (two paints for a key action: the change, then the confirmation clearing).

## 2026-10-30: faded full refreshes (driver fix, flashed)

Full-screen GC16 came out very faded (patches were fine; power and enhance-driving ruled out).
Compared with Waveshare's EPD_IT8951.c and the E1003 driver in tesserae-device-firmware
(`fw/src/panel/drivers/it8951_gray.c`); two differences, both now in `src/it8951.c`:
- **Forced waveform temperature, 14 C** (`apply_temp()`, CMD 0x0040, re-applied on every
  init). This board gives no temperature reading. Full GC16 went from 627 ms to 1015 ms: a
  longer, stronger waveform. `/api/cmd?temp=N` to try others (0 = controller default).
- **INIT before a full-screen GC16 once anything has been shown** (`s_ref_stale` in
  `paint()`): GC16 derives each pixel's drive from the controller's reference image, and a
  full GC16 after earlier refreshes under-drives (dmellok/tesserae#274). A full refresh now
  costs ~2.2 s more. `/api/cmd?cleanfull=0` turns it off for comparison.
- **STANDBY between updates: does not work on this board.** Sent after each refresh (as the
  E1003 driver does), the controller's HRDY stays low and the next command times out, so
  painting stops. Left in the code, off by default (`s_standby`; `/api/cmd?standby=1` to try).
- VCOM −1.55 V was tried live; no visible improvement reported; back at −1.48 V.
- Found the bench panel back on Screen updates = Fast after the reflashes (the Clean choice
  had not stuck); set to Clean again, saved, and it survives a reboot now.
- **ROOT CAUSE FOUND: the SPI clock.** At 12.5 MHz a full-screen solid black (`/api/cmd?pattern=black`)
  came out dark grey with lines: pixel data was being corrupted on the way in, which reads as
  lighter greys everywhere (the "faded" screens). At 4 MHz (`/api/cmd?spihz=4000000`, what the
  E1003 driver uses) solid black is solid. Default is now `-DEPD_SPI_HZ=4000000` in
  platformio.ini (full-screen load ~2.5 s instead of ~0.85 s). The VCOM / temperature sweeps
  above ran on corrupted data, so don't trust them; VCOM is back at the rated -1.48 V.
  Diagnostics kept: `pattern=black|ramp`, `temp=`, `cleanfull=`, `standby=` (broken).
- **SPI settled at 6.25 MHz** (platformio.ini). Test any clock with BOTH `pattern=black` and
  `pattern=ramp`: 7.5 and 9.4 MHz passed black but striped the greys.
- **Second pass.** A screen change or full GC16 still looked faded until something redrew it
  (a knob turn), so `paint_pump()` now follows those with a GL16 pass over the same areas
  with no reload (`PJ_REDISPLAY` / `it8951_redisplay_areas()`), ~0.55 s, no flash.
- Waveform temperature default is 24 C (judged best); VCOM back at the rated -1.48 V.
- Live (dashboard 0) still has the old top and status bars, by the Mk II rules; the panel boots on it.
- **Final: SPI 12.5 MHz with 12 mA / fast-slew pins** (`EPD_DRIVE_MA`, `EPD_FAST_SLEW` in
  platformio.ini; `/api/cmd?spidrive=12f` live). The corruption was soft edges from the
  default 4 mA / slow-slew pins, not the clock as such. Clean for black and the ramp.
- **Waveshare setup restored** for the rest: waveform temperature not forced, INIT only at
  start-up (`cleanfull` off), standby off; the GL16 second pass after screen changes stays.
- **Rain web stream**: the sample ring is ~4 s in PSRAM (+6 MB) instead of 0.74 s in SRAM, and
  the web player keeps a 1 s cushion; frame downloads no longer gap it.
- **Vertical lines on dithered dashboards** (31 Oct). Not the SPI clock (same at 12.5, 9.4 and
  7.5 MHz; the panel's own copy of the frame is clean). Two causes found: the GL16 second pass
  streaked dithered greys (now off; `/api/cmd?echo=gl16|gc16|off`, and it only ever applies to
  non-full screen changes), and drawing over the old screen without INIT left traces (INIT
  before full GC16 is back on by default, `cleanfull`). Faint lines may remain: reseat the
  panel's FPC at the HAT next.
- **Power supply was the last piece** (31 Oct): the remaining faint vertical lines went away on a
  good PSU. Some workarounds above (INIT before every full GC16) may no longer be needed; re-test
  with `/api/cmd?cleanfull=0` before keeping their cost in speed.

## 2026-11-01: OTA from GitHub, boot screen, WiFi setup, dashboard picker

- **OTA** (`src/ota.c`): A/B partitions + try-before-you-buy (Raspberry Pi's
  `pico_w/wifi/ota_update` sequence). Lessons, all fixed: the boot ROM needs a
  4 KB workarea for `rom_explicit_buy` (3264 gave -13); **the buy writes flash,
  so it must run before core 1 starts** (core 1 paints from flash; a buy under
  it killed core 1 and the next settings save hung waiting for it); settings are
  read through `XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE` (XIP_BASE is remapped onto
  the running slot); a trial (TBYB) image only starts from a flash-update reboot,
  so USB installs (`.uf2`) are normal builds and only the OTA `.bin` is TBYB.
  `picotool` v2.0.0 here; copying a UF2 onto the BOOTSEL drive fails silently
  once partitioned.
- **HTTPS**: mbedTLS 3.6.2 built by `scripts/build_mbedtls.py`
  (`include/bedside_mbedtls_config.h`: the name matters, a plain
  `mbedtls_config.h` loses to mbedTLS's own), lwIP altcp with the SDK's patched
  `altcp_tls_mbedtls.c`, a 1 MB PSRAM heap for mbedTLS and cJSON
  (`src/psram_heap.c`). Handshake ~1.5 s. Certificate dates are not checked.
- **PSRAM map**: frame 0, download 2 MB, preview 4 MB, heap 4.5-5.5 MB, audio
  6 MB, OTA chunk 6.5-7.5 MB, frames.json 7.5 MB, log 7.75 MB (64 KB).
- **Boot screen** (CONTENT_BOOT in `src/screens.c`): the official lockup
  (`assets/brand/`, `tools/gen_logo.py`) and the boot log; WiFi joins async.
- **WiFi setup** (net.c, vendored DHCP/DNS servers): see README.
- **Dashboard picker** (Tesserae 0.498 `frames.json`, `/frame?page=`).
- Diagnostics kept: `https=<url>`, `showsetup`, `wifisetup`, the `loop` phase in
  the state JSON.

## 2026-11-01: file storage, desktop web app

- Slots shrank to 5.5 MB (`tools/pt.json`; firmware is 3.6 MB since the
  1.3 MB test-card image went, the Test card is now a generated pattern). A
  panel on the old table keeps working with 1 MB of storage; `tools/partition.sh`
  moves it to the new one (settings survive).
- `src/store.c`: region from the partition table's end to the settings at
  15 MB. Ask the ROM one partition at a time (`PT_INFO_SINGLE_PARTITION`); the
  all-partitions layout read wrong and put the store inside slot A. Upload
  chunks land in PSRAM at 5.5 MB and are written from `net_poll()` (main
  context), never from the lwIP callback. One 4 KB SRAM staging sector is shared
  with the updater (`config_sector_buf()`); SRAM is at ~98%.
- USB flashing paints "Updating firmware" first: `tud_cdc_line_coding_cb` is
  ours (`PICO_STDIO_USB_ENABLE_RESET_VIA_BAUD_RATE=0`), with a 6 s alarm that
  resets anyway.
- OTA lessons: TCP_WND must exceed one 16 KB TLS record (altcp_tls opens the
  window only as records decrypt); GitHub's signed asset redirect is ~1 KB.
- Headless Chrome needs `--disable-features=AudioServiceOutOfProcess,AudioServiceSandbox`
  for `decodeAudioData` to return (for testing the converter).

## 2026-11-01: speaker, battery, wind, recordings

- `audio.c`: I2S is pico-extras' audio_i2s program built with the
  `pio_encode_*` helpers (no pioasm in this build), two DMA channels
  ping-ponging over 2 x 256 frames in SRAM, refilled in a DMA_IRQ_1 shared
  handler on core 1 from the ring, ~50 ms behind the writer (with a warm-up so
  it never plays stale ring data). Rendering now runs while the speaker plays
  as well as for web listeners. Verified on the bench without the amp: the
  buffers refill (`hw.i2s_fills` in the state JSON); not yet heard.
- `power.c` is untested against hardware (not wired yet). A gesture sensor
  (APDS-9960) driver was written then removed the same day: not wanted. The
  hardware settings blob keeps its bytes as reserved so saved settings load.
- `rainsynth.c`: RS_WIND and RS_RECORDING. The recording is decoded in place
  from flash (store.h format); verified by correlating the web stream with the
  uploaded file (0.994). `tools/render_rain.c` levels: wind L1/L3/L5 at about
  -26/-20/-17 dBFS RMS, rain L3 -19.
- printf here has no float support: format numbers as integers (mV).
- Menu lists move exactly one row per detent (acceleration only on values).

## 2026-11-01: SRAM and host tests

- SRAM 98.4 % -> 78 %: the 82 KB SRAM preview fallback is gone (no PSRAM =
  no preview) and the SRAM audio ring is 4096 samples (PSRAM replaces it at
  boot). Next-biggest if more is needed: the local UI draw lists (L_*/P_*,
  8 x 20 KB), read per row by the painter on core 1.
- `src/alarm_sched.c`: the alarm decisions pulled out of local.c, unchanged in
  behaviour; verified on the panel with a one-off alarm (prewake, chorus
  0.08 -> 0.74 over a 1 min window, rang on the minute).
- `tests/`: 31 tests, 202 checks. Each was checked by breaking the code it
  guards (mutation check), including the store free-space bug of 1 Nov.
  `store.c` builds for the host with `-DSTORE_HOST` against `tests/host/`.

## 2026-11-01: the fade, found

The IT8951 picks its waveform by temperature, and its sensor (an SE95 on the
HAT, per Waveshare's schematic) measures the board, not the glass. The board
warms with every refresh, so after a warm day it chose a hotter, shorter table
and under-drove 20 C glass: a uniform fade, worse the longer it ran, and
"reboots into a good or bad state" because the single force at init could be
missed. Now: temperature read properly (cmd 0x0040 arg 0 answers TWO words,
sensor then forced; we had read one), the force re-sent before every display
(as Seeed/FrameOS do for this glass), VCOM checked before each paint with a
re-init and full repaint if the controller lost its settings, and the forced
value a saved setting (Settings > Device > Waveform temp; web app Panel card;
`wf_temp` 0 = default 20). Bench: sensor 23 C with the room at 20.
Keep the forced value within ~5 C of the real glass temperature: too low is
over-drive (ghosting over time), not damage; wrong VCOM is what damages panels.
Also: Screen updates must be Clean, not Fast (Fast's GL16 partials accumulate
for ~40 min before cleanup). Research sources in the session transcript.

## 2026-11-01: navigation speed, and what the references say about the fade

Reference material read: IT8951 datasheet (V0.2.4.3), Waveshare IT8951 HAT
schematic and example code, E Ink's AF waveform mode declaration, the TPS65185
datasheet, and the E1003 / ESPHome / FrameOS / Seeed / FastEPD drivers for
this glass (extracts in the session scratchpad). Findings that changed code:
- **DPY_AREA is queued, not parallel.** N areas in one job cost N waveform
  runs (4 areas: 2.8 s). `it8951_update_areas` now loads each area and
  refreshes their bounding box once; a partial update only drives pixels
  whose value changed, so the picture is identical.
- **Rendering was full-width per row**: a 59 px gutter cost a full render of
  every row it spans (570 ms). `layer_row` now clips to the columns being
  streamed (`it8951_stream_window`).
- **Menu open merged the content and the strip** (5 px apart, merge gap 32)
  into a full-screen job, which took the INIT clear and a flash every time.
  `mark_screen` no longer marks the strip.
- Quick menu steps: DU on the changed rows plus the turn-count gutter as
  separate load areas (one refresh), 183 + 441 ms; a GC16 settle pass over
  list + preview 450 ms after the knob rests (dropped if a turn beats it).
  DU on this panel at 20 C forced is 441 ms (E Ink's typical: 260 at 25 C).
- SPI: 18.75 MHz (the next integer divider) fails the GET_DEV_INFO handshake;
  12.5 MHz is the ceiling. `spitest` (memory burst read-back, cmd 0x0012/13/15)
  reads real image memory but not the rows just loaded; the address unit or
  the FIFO protocol is still wrong, so it is a diagnostic in progress.
- Not applied, with reasons: Enhanced driving (reg 0x0038 = 0x0602) is for
  blur from a long FPC per Waveshare, not contrast; A2 would halve the step
  time but needs black/white source pixels and the rows carry anti-aliased
  greys; STANDBY between refreshes (what Waveshare's demo and the E1003 do)
  hangs HRDY on this board, and the driver comment's claim that SYS_RUN
  washes the image out over minutes was not reproduced. If a fade ever
  appears minutes after a paint with the temperature forced, try
  POWER_SEQ (cmd 0x0038 arg 0) after each refresh.
- Job trace: every submitted paint logs `paint: job <full|areas> mode M, N
  rect(s)`; `s_trace_marks` in `src/ui_core.c` logs every dirty rect.

## 2026-11-01, evening: USB loads were landing in the wrong slot

With the A/B table, `picotool load` picks a partition itself; the firmware
wipes the other slot at boot, so a load into the non-booting slot was erased
and the previous build kept running. Several of today's builds never ran
because of this. `tools/flash.sh` now asks the panel which slot boots
(`ota.slot` in /api/state) and loads with `-p <slot>`; it also refuses to
request BOOTSEL unless the panel is on this Mac's USB (otherwise it sits in
the bootloader until power-cycled). Every build is stamped (`build` in
/api/state and the boot line): check it after every flash.
Also: DU/A2 loads now carry black-and-white pixels so the controller's
"image on the glass" record is right; loading greys for a DU refresh left
later paints under-driving (faded) wherever the menus had been.

## 2026-11-01, late: the fade was the ribbon cable

`src/test_paint.c` (`pio run -e paintest`, load with `picotool load -x -p
<booting slot>`) drives the controller with Waveshare's reference sequence and
nothing else; commands over USB serial paint wedges/black/white/checker and
change one thing at a time (bpp, packed write, temperature, VCOM, mode,
enhanced driving, SPI clock). With it, the 16-step wedge was still faded and
streaky: not our paint path. Reseating the panel's FPC (ribbon) cable fixed
it: same wedge, same settings, black black and 16 clean steps. A marginal
FPC explains the whole history (came and went with handling, warmth and
time; every software change only ever helped "a bit"). After reseating with
power on, the controller needed a reset (`i`) before paints showed.
If it fades again: reseat the FPC first, then `tools/flash.sh` is not the
tool, the paint test is (load it, `r`, look).
Known: a 1 MHz full-frame load in the paint test hangs/crashes the board
(unresolved; 12.5 MHz is fine). The temperature forcing, VCOM check and DU
record fix stay: they are correct and cheap.

## 2026-11-02: the fade, measured and tuned

- `spitest` works now (the controller keeps one byte per pixel, so the burst
  read returns 4-bit loads expanded): 18,720 pixels, five passes, all exact
  at 12.5 MHz. The data path is clean; the fade is on the glass side.
- Caught in the act: a partial GC16 refresh of an area lightens it a little
  every time, even with identical pixels and even in the bare Waveshare
  sequence (`test_paint` `r` then `a` x4). The clock's minute tick did this to
  the strip every minute; menus and tiles likewise. A full INIT + GC16 resets
  it, which is why every "fix" looked good right after its repaint.
- VCOM sweep with that test (same pixels, four partials): -1.30 worse,
  -1.48 (the label) drifting, -1.66 better, -1.85, -2.00, -2.15 and -2.20
  better still with clean whites, -2.45 greys the whites. -2.20 darkened the greys
  over a morning's use, so -2.00 V is built in (-DEPD_VCOM_MV=2000). Drift
  lighter = go more negative; greys/whites darkening = go back.
- Firmware: strip changes are marked by diffing the strip layer before and
  after rebuild() (`mark_changed`), so the minute tick refreshes the digits
  only; every partial counts towards a full clean refresh (30 partials or 45
  minutes, idle 20 s, home screen); panel rails off after 4 s idle.
- Custom waveforms: possible in principle (the table lives in the HAT's
  flash, replaceable with the IT8951 USB tool) but E Ink's files are
  proprietary and per-lot; a wrong waveform can damage the panel. Not pursued.

## 2026-11-02, midday: making it predictable

"Sometimes fine, sometimes bad" is where in the partial-refresh cycle you
look: best right after a full INIT + GC16, a little worse with each partial.
Now deterministic: a full clean refresh 15 s after the last interaction if
any partial happened (all modes; SETTLE_MS), and every N minutes while idle
(Settings > Device > Clean refresh every; web Panel card; default 10,
`clean_min`), plus after 30 partials. The panel rails stay on again: the
first refresh after powering them up looked weak, which made the fade look
bimodal (`epdpower=1` re-enables idle power-off to test). Screen-updates mode
is now cosmetic: GL16 is never used, and the settle refresh runs in every mode.

## 2026-11-02, afternoon: modular firmware

`src/local.c` (2,800 lines) and `src/main.c` (1,500) are now eleven modules, one
per concern, with the public API unchanged (`include/local.h`). Behaviour is
identical: the split was mechanical (cut along the section comments, `static`
dropped only from what crosses a file, declared in one internal header each),
then compiled and flashed. Flash grew 1 KB (lost cross-module inlining).

- **The UI** (`src/ui_*.c`, sharing `include/ui_internal.h`): `ui_core.c` state,
  option tables, dirty regions, `rebuild()`, the repaint regions, init and the
  ticks; `ui_draw.c` the draw list and row rendering; `ui_menu.c` the
  click-wheel menus (tree, row model, top bar, navigation, every input);
  `ui_live.c` the Live dashboard, status bar, date and clock; `ui_overlay.c`
  the home-screen overlay, toasts, the update card; `ui_alarm.c` the alarm
  clock, dawn chorus and sleep timer; `ui_api.c` the web app's direct control,
  ha.c views, settings persistence, power and the state JSON.
- **The application** (sharing `include/app_internal.h`): `main.c` boot, the
  main loop, inputs and bindings, config seeding, the sound glue; `screens.c`
  content kinds, boot and setup screens, the banner, `composite_row()`;
  `paint.c` the painter's snapshot and pump, the clean-refresh policy, clears
  and full repaints; `commands.c` the serial and web command set, state JSON
  and preview, the console log, field diagnostics.
- Each header lists its shared variables and functions under the module that
  defines them. Something used by one module only stays `static` in it. When
  a function moves between modules, move its declaration to the new group.
- The build stamp (`APP_BUILD`, main.c) is the one thing `tools/flash.sh`
  touches to refresh; the state JSON reads it from there.

## 2026-11-02, late afternoon: release 0.3.0, and why the GitHub check sometimes failed

- **v0.3.0 released** (tag pushed, `release.yml` built it, notes written by hand:
  `gh release edit v0.3.0 --notes-file`). `--generate-notes` gives only a
  compare link on this repo (no PRs), so write the notes.
- **"ota: release list unreadable (… error at -1)"**, intermittent since the
  first OTA work, was GitHub answering `Transfer-Encoding: chunked` now and
  then: the body began `707E\r\n[{…` and cJSON parsed "707" as a number
  (`error at -1` = the parse *succeeded* but was not an array). The REST client
  never de-chunked. Fixed with `src/http_chunked.c` (a byte-at-a-time decoder,
  host-tested in `tests/test_misc.c`), used by `net_rest.c` for sink bodies as
  they stream and for small bodies in place. The failure path now dumps the
  first 60 bytes of the buffer, so the next "unreadable" explains itself.
- **The build stamp was stale** (`build` in `/api/state` showed the previous
  build after a flash). PlatformIO decides what to recompile by file content,
  not mtime, so `touch src/main.c` did nothing; `tools/flash.sh` now also
  removes `main.o`. ALWAYS check `build` after a flash.

## 2026-11-02, evening: the speaker is in; the click, the tuning page, the desktop app

Kayden wired the amp and speaker (works), and reported a sharp click now and
then while changing sound settings, and that the bird calls can be very sharp.

- **The click was the settings save.** Every sound/hardware/panel setting is
  written to flash 3-4 s after the knob stops; the write parks core 1 (the
  multicore lockout) for the 50-60 ms sector erase + program, and the I2S DMA
  had only two 11.6 ms buffers, so the amp ran dry: a hard cut, then a hard
  restart. Now `I2S_FRAMES` is 2048 (two 93 ms buffers in SRAM, DMA keeps
  playing through the write), the reader sits a buffer plus 20 ms behind the
  writer, and playback fades in over 12 ms whenever it (re)starts and fades
  out before any gap it can't cover. `ui.spk.dropouts` in `/api/state` counts
  gaps; verified: saves during playback leave it unchanged. SRAM +14 KB (89.9%).
- **Speaker tuning** (`src/spk_dsp.c`, host-tested): per sample on core 1 in
  the DMA fill, low cut (60-160 Hz), bass shelf at 200 Hz (±8 dB), treble shelf
  at 3 kHz (-12..+6 dB), a "soften" low-pass (8..3 kHz) for sharp highs, level
  trim (-12..+6 dB) with a glide, and a peak limiter (gentle -6 dB / firm
  -12 dB). Settings in the hardware blob's spare bytes (defaults read as zero;
  older blobs load unchanged). Panel: Settings > Device > Speaker tuning;
  web: the Speaker card / page; keys `spk_gain|bass|treble|soft|hpf|limit`.
  A test sound (`test` 1-4: pink, 1 kHz, 100 Hz, sweep) plays for 20 s at the
  current volume whether or not the sleep sound is on.
- **Bird softness** (`bird_soft`, default 35, Dawn chorus > Softness): two
  one-pole low-passes on the bird bus alone, 9 kHz (natural) down to 1.6 kHz;
  the rain is untouched. The test checks the whistler's top end drops to under
  35% while most of the call remains.
- **The desktop app** (`www/desktop.src.html` → `/desktop`): a dedicated
  shell (sidebar pages, status strip, wide grids, the panel preview beside the
  room controls, the speaker's response curve drawn from the same biquad
  formulas, a full-height console, keys 1-7 switch pages). The controls are
  not duplicated: `scripts/build_www.py` lifts each card marked
  `<!--part:name-->` out of `app.src.html` into the `<!--@name-->` slots, and
  shares the script and stylesheet. The phone page sends wide pointer screens
  to `/desktop` unless `?m` (or its "Phone layout" link) was used.
- `net_rest.c` freed cJSON's output with libc `free()`: with the PSRAM heap in
  place that is the wrong allocator. Now `cJSON_free()`.

## 2026-11-03, morning: fade again overnight, and why the strip was being repainted whole

Kayden: "some more fade in the morning" (on the Bedside server dashboard with
the strip, VCOM -1.90 V since the afternoon before). The log explained the
mechanism: 920 paints in 12 h. The strip's clock was one text op and the
items after it were laid out from the clock's measured width; the digits are
not fixed-width, so when the minute changed the alarm and sound items shifted
a few pixels, the diff marked all three, and the refresh (one DPY_AREA over
the bounding box) was a GC16 partial over ~1100 x 124 px every minute, all
night, with a full clean refresh every 10 minutes.

- **Clock digits in fixed slots** (`clock_slots()` in ui_overlay.c, used by the
  strip, the cards and the Live page): one op per character, every digit as
  wide as the widest, so a minute tick marks one digit cell and nothing moves.
  The Live page's "in 13 h 30 min" rounds to 5 min (15 past 3 h), so that text
  changes rarely too.
- **Clock updates** (Settings > Home screen; web Display page; `clock_du`):
  the strip's partials painted two-level (DU) at night (default), always, or
  never. DU paints black and white only (no anti-aliasing on the digits) with
  no flash, and does not run the GC16 partial waveform that fades this glass.
  The card layouts keep GC16 (their grey cards would threshold to white).
- **Waveform temperature**: the board sensor runs warm, never cold, so when it
  reads below the 20 C default the glass is at least that cold and its longer
  waveform is used (`main.c`); the sensor is read again before every clean
  refresh and logged with the forced value and VCOM, so the next overnight
  report has numbers.
- VCOM -1.95 V (platformio.ini): -1.90 let the blacks drift, -2.00 darkened
  greys; the middle.

## 2026-11-03: internet radio

A **Radio** sleep sound (kind 5): `src/radio.c` streams an Icecast /
SHOUTcast MP3 station over plain TCP or TLS (its own altcp pcb, alongside the
REST client's), parses the ICY headers and interleaved metadata (`src/icy.c`,
host-tested), keeps 256 KB of compressed audio in PSRAM, decodes it on core 0
with minimp3 (`src/vendor/minimp3.h`, public domain, `MINIMP3_ONLY_MP3`) into
8 s of mono 22.05 kHz PCM (`src/pcm_resample.c`: channels averaged, a box
filter down from 44.1 / 48 kHz), and the sound engine on core 1 pulls that as
the radio bed (`rs_params_t.radio_pull`), so the tone stage, sleep timer and
dawn chorus apply as for rain. PSRAM: PCM ring at 5.6 MB, stream + decoder at
6.19 MB. Playback starts with 2 s in hand; a dead stream reconnects with
backoff (2, 5, 30 s); a 403 is retried once without a User-Agent (SomaFM
refuses generic browser-like identities but accepts the device's own).

Things learned on the bench:
- `radio_tick()` runs from `ui_service()`, so it keeps decoding during a
  dashboard download. The main loop only turns ~10 times a second, so the
  decode budget per pass is adaptive (40 ms with under 1 s decoded, 16 ms
  under 6 s, 4 ms after): at a flat 8 ms the decoder fell behind real time
  and the sound stuttered.
- minimp3 returns `frame_bytes == 0` when the window ends mid-frame; that
  means "more bytes", not "junk". Discarding the window there cost 62 KB and
  a re-sync per connection.
- The "no data for 20 s" timeout must compare signed: a packet can land
  (updating the last-data stamp) after the tick took its clock, and the
  unsigned difference wraps to huge.
- Verified: Drone Zone 128 kbps, 1.9 MB in 90 s, 3,872 frames, 836 bad bytes
  (the initial sync), 0 underruns, titles arriving; core 1 at 17%.

Presets: flash blob slot 6 (`RAD1`), seeded with five SomaFM channels, Radio
Paradise and FIP; `radio=<i>|<name>|<url>` sets one (web app, Sound card >
Stations). The chosen preset (`station`) lives in the hardware blob's spare
byte. Panel: Sound > Station (shown when the kind is Radio). HA's Sleep sound
select gained Radio. Not done: wake-to-radio, HLS/AAC stations, a station
list fetched from SomaFM's API.

## 2026-11-03, afternoon: the Radio section, and lists that scroll

- **Menu > Radio**: Playing on/off (switches the sound to Radio), *My
  stations* (the seven presets from the web app), then every SomaFM channel
  as a row; the preview shows the channel's description; the one playing
  says "Playing"; pressing a row plays it. The root row shows what is on.
- **SomaFM's list** (`src/soma.c`, host-tested): a snapshot compiled in
  (`src/soma_snapshot.c`, regenerate with `tools/soma_snapshot.py`), replaced
  by the live `api.somafm.com/channels.json` 45 s after boot and daily (the
  fetch is the REST client + cJSON, ~3 s, from the PSRAM heap; the table lives
  in the radio's PSRAM work area). Every channel streams at
  `http://ice1.somafm.com/<id>-128-mp3` (all 51 answered 200). `/api/soma`
  gives the list to the web app, whose Station picker has a SomaFM group.
  State: `station` -1 means a SomaFM channel, `soma` its id (hardware blob
  spare bytes: `rsv1[0]` = station + 2, `rsv1[1..16]` the id).
- **Lists longer than 16 rows scroll** instead of shrinking their rows: a
  window of `LIST_FIT` (9) full-height rows that moves only when the
  selection would leave it (`list_top()`, the `top` kept per stack entry).
  A step inside the window repaints two rows (DU, as before); a step that
  moves the window repaints the list (`mark_list()`). Shorter lists are
  unchanged. `s_rows` holds `ROWS_MAX` (72) rows.

## 2026-11-03, evening: the fade isolated again

- The bare `paintest` firmware reproduced both a washed-out INIT + GC16 wedge
  and progressively lighter identical partials, so the application was not the
  baseline cause. Rated VCOM, slower SPI and Waveshare's 8-bit word-by-word
  path each helped a little, but none was sufficient alone. Replacing the
  cables and moving from Mac USB to an external supply also each helped.
- **The combination produced a clean wedge:** replacement cables, external
  power, rated **-1.48 V**, and SPI **4 MHz requested / 3.75 MHz actual**.
  Those are now the build defaults. Do not connect PowerBoost and Mac USB
  together without the Schottky diode in the wiring guide.
- Nine ordinary GC16 partials still faded the Live page at that clean baseline;
  the 15-second settle refresh restored it. The Live minute clock now honours
  **Clock updates** too, using DU when two-level updates are active. The default
  is **Two-level always** for both the Live clock and the strip. Grey cards and
  other partials keep GC16 and the full clean-refresh policy. Flashed and
  verified on the glass: the next minute painted the same 143 x 266 column in
  DU (426 ms) and the column stayed clean.
- A second ED103TC2, labelled **-1.45 V**, then passed the same clean-ramp and
  clean-DU-minute checks. The build default is now **1450 mV for that installed
  second panel**; the first panel must be built with 1480 mV instead.
- `tools/flash.sh paintest` was never a valid command: the script treats its
  argument as log seconds and builds the normal image. Build with
  `pio run -e paintest` and load `.pio/build/paintest/firmware.uf2` into the
  booting slot with `picotool load -x -p <slot>`.

## 2026-11-03, evening: the new Live bento

- The replacement Live dashboard is an asymmetric six-tile bento in
  `src/ui_live.c`: Clock, dark Tonight, tall Outside with rain forecast,
  Bedroom (including humidity and A/C), Home (security and washing), and a wide
  Reminders tile (bins and next event). The 214 px clock fits its tile; body
  labels use larger black text rather than small grey type.
- **Demo data** fills realistic values without changing MQTT configuration.
  It is in panel Settings > Home screen and the web app's Home screen card,
  persists in the hardware settings blob (`rsv0` bit 5), and has API key
  `live_demo`. The currently flashed panel has it enabled.
- Home turns black for an open door or finished washing. Real values still use
  the same MQTT sources documented in `docs/live-dashboard.md`.
- Verified with the populated bento: the minute updated the clock and demo
  sound-time in DU (two areas, 140 x 266 and 451 x 50) and every tile stayed
  clean. All host tests and the firmware build passed.

## 2026-11-03, night: review of the bento, release 0.5.0

- Kimi's Live bento and panel-swap defaults reviewed before the tag: both
  firmware environments and the host tests build clean, and `pio run`
  regenerates the same web bundle (`src/www.S` sha1s), so the pages came from
  `app.src.html`. Two fixes, found by measuring the fonts on the host
  (`draw_font_w` over `src/fonts/*.c`): "No alarm" in F_XL is 398 px wide and
  its hint started 290 px in, so they overlapped; the hint now follows the
  value. The wake window used the raw `early`, so an alarm left at 0 put the
  chorus at the alarm time; it uses the 10 min default like
  `alarm_window_s()`. An unknown next event reads "--" like the other cards.
- **v0.5.0 tagged**: `release.yml` builds it, notes written by hand
  (`gh release edit v0.5.0 --notes-file`). It carries the radio, the bento,
  Demo data, the Live clock in DU and the second panel's defaults (VCOM 1450,
  SPI 3.75 MHz). Build the first panel with `-DEPD_VCOM_MV=1480`.
