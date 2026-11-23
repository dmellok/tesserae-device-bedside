# Bedside simulator

The panel's firmware, built for macOS and run in a browser. Use it to try a
change to the menus, pages, setup or alarms in seconds without flashing, and
to reproduce what a user sees from their settings without the hardware.

```sh
make -C sim run            # build, start, then open http://localhost:8080/sim
```

`make -C sim` only builds (`sim/build/bedside-sim`). It needs the Xcode
command line tools and Python 3, and nothing else. PlatformIO isn't needed.

## What runs

This is the real firmware, not a model of it. The application files in `src/`
compile unchanged: `main.c`, the whole UI (`ui_*.c`), painting (`paint.c`,
`screens.c`, `painter.c` with core 1 as a thread), inputs (`inputs.c`),
settings and files in flash (`config.c`, `store.c`), the Tesserae client
(`tesserae.c`, `net_rest.c`), Home Assistant (`ha.c`), the radio, and the
sound engine. Only the hardware underneath is replaced, in `sim/src`:

| On the device | In the simulator |
|---|---|
| IT8951 controller and glass (`it8951.c`) | `sim_panel.c`: the image memory and the glass. Refreshes take the device's measured times. GC16 and INIT flash, and DU and A2 show two levels until a full-quality pass. A full GC16 does an INIT first when the controller's comparison image is out of date. |
| NeoKey 1x4 and rotary encoder (`seesaw.c`) | `sim_keys.c`: the seesaw boards, with latched key flags and the LED colours the firmware sends |
| WiFi and the web server (`net.c`) | `sim_web.c`: the same routes on a Mac socket, plus `/sim` |
| lwIP, mbedTLS, cyw43 | `sim_lwip.c`: DNS, TCP and an MQTT 3.1.1 client on Mac sockets |
| I2S amp (`audio.c`) | `sim_audio.c`: the same render loop into the same ring, played through the Mac's output |
| Flash | `sim/build/flash.bin`, 16 MB, kept between runs |
| PSRAM | an 8 MB array (`override/psram.h`) |
| Pico SDK | `include/sim_sdk.h` and one-line stand-ins for each SDK header |
| OTA, battery monitor | stubs (`sim_stubs.c`) |

`net_rest.c` and `tesserae.c` are compiled inside two small wrappers
(`wrap_*.c`). They start the wall clock and time zone from the Mac's when no
server has answered yet. On the device both come from the server.

## The simulator page

`/sim` shows the glass as the panel would. The four keys sit above it and
glow with their LEDs, and the knob is beside them. The page also has:

- **Keys:** <kbd>1</kbd>–<kbd>4</kbd> are the four keys (hold one to hold it).
  <kbd>←</kbd>/<kbd>→</kbd> or scrolling on the knob turns it, <kbd>Enter</kbd>
  presses it, <kbd>Esc</kbd> holds it (Back), and <kbd>b</kbd> is BOOT.
- **Clock:** jumps forward by +1 min to +8 h, or to a time such as 06:20.
  Timers fire and alarms ring as if that time had passed.
- **Refresh outlines:** each refresh is outlined in its waveform's colour for
  a moment, which shows what repainted and how.
- **Log:** the firmware's console, the same one as `/api/log`.
- **Phone app:** the real web app, served from `www/` and read fresh on every
  request. After a change to `www/app.src.html`, run `make -C sim www` and
  reload.

The phone app is also at `http://<this Mac's IP>:8080/` on the LAN, as on the
device. In the terminal running the simulator, single keys go to the
firmware's serial console (<kbd>?</kbd> lists them).

## Options

```
--port N        web server port (default 8080; the next free one if taken)
--flash FILE    a different flash image: one per test setup or customer
--reset         start from an erased flash (WiFi seeded, first-run setup)
--instant       refreshes take no time
--no-audio      no sound
--no-knob, --no-keys   as if that board were unplugged
```

Restarting from the page, the settings menu, or the firmware's own reboot
starts the process again with the same flash file, as a power cycle would. A
jump of the clock carries over a restart. A new run starts at the Mac's time.

## A Tesserae server and Home Assistant

These are set up as on the device: the server under Settings in the phone
app, the MQTT broker under Home Assistant. The simulator pairs as its own
device, `bedside-513d` with MAC `02:5e:5e:b3:d5:3d`, so it doesn't clash with
a real panel.

## Not simulated

Test these on the device before a release:

- **The look of e-paper:** grey levels, ghosting and fading. The glass shows
  the waveform's result, not its artefacts.
- **https:** there is no TLS. The GitHub release list, OTA and https radio
  streams fail as if unreachable. Tesserae and MQTT over plain TCP work.
- **WiFi:** joining always succeeds. The setup page is served at
  `http://localhost:8080/` in setup mode, and the network list is made up.
- **Hardware faults:** I2C glitches, flash stalls, the battery and the
  speaker's tone stage.
- **Timing:** the Mac is much faster than the RP2350 at everything except
  refreshes, which are slowed to the panel's times. Use `--instant` to skip
  those too.
- **Addresses:** the panel shows `IP:port`, so the address row in setup is
  wider than on the device.
- **The log after a clock jump:** a jump shows as a stall there ("ui took …
  ms"), and so does a refresh that was running when it happened.
