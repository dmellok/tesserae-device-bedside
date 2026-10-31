# Tesserae protocol, as this device uses it

A digest of the server's authoritative spec (`tesserae/docs/dev/client-protocol.md`,
"Client protocol", current `main`) reduced to what the bedside firmware
implements and relies on. Where this document and the server spec disagree,
the server spec wins; this one says *which parts* we use and *why*. Read it with
`src/net_rest.c` (the wire) and `src/tesserae.c` (the loop) open.

## Identity

| Field | Value | Why |
| --- | --- | --- |
| `kind` | `seeed_reterminal_e1003` (flash config, web UI "Kind"; build default `TESSERAE_KIND`) | The kind picks the panel block and renderer. The E1003 is the server's own 10.3" IT8951 / ED103TC2 board: its hardware-catalog entry fixes the panel at 1872x1404 landscape, gamut `gray_16`, renderer `esp32_gray_bin`, whose output is byte-identical to the bench driver's frame format. **Do not use bare `esp32_client`**: on the bench (29 Oct) the Register card then defaulted the panel to 800x480 Spectra 6 and the server served 192,000-byte colour frames, which the firmware refuses with "frame size wrong". The pico-bin kind packs Spectra 6 nibbles and is wrong too. Once `tesserae/hardware/waveshare_103_it8951_pico.json` is installed on the server, set the kind to `waveshare_103_it8951_pico` for a correctly named entry. |
| `gamut` | `gray_16` | 16-level greyscale, 4 bpp. Persisted onto the device's panel block at pairing. |
| `panel_w`, `panel_h`, `rotation` | 1872, 1404, 0 | The glass is landscape-native and a single controller scans it, so the framebuffer *is* the composition: no transpose. |
| `device_id` | `bedside` (config), default `bedside-<board id>` | Must match `^[a-z][a-z0-9_-]{1,31}$`. The server may answer with a different id (MAC match on a device it already knows); the firmware adopts it, because the token is bound to that id. |
| `mac` | the CYW43 station MAC | The identity for zero-touch pairing. A placeholder MAC is rejected (`400`). |
| `name` | `Bedside` (config) | Prefills the Register card's display name. Operator's choice wins afterwards. |
| `fw_version` | `FW_VERSION` | Shown on the device card. |
| `can_stay_awake` | `true`, on register and on **every** heartbeat | Mains powered. Unlocks the "Stay awake" setting; the server treats an absent key as "no change", so it is always sent explicitly. |

A ready-made hardware catalog entry for this panel is in
`tesserae/hardware/waveshare_103_it8951_pico.json`. Dropping it into the
server's `hardware/waveshare/` directory and setting the kind (web UI, or
`DEVICE_KIND` in `secrets.h`) to `waveshare_103_it8951_pico` gives the device
its own kind (name, notes, `refresh_floor_s`, no touch schema) while
inheriting the `esp32_client` protocol and the `esp32_gray_bin` renderer.
Until then the E1003 entry is the same hardware under another name.

Changing the kind of an already-registered device needs a **delete and
re-pair** on the server: a `/discover` that MAC-matches a known device, or a
`/register` for an existing id, returns the existing instance untouched and
never rebuilds its panel block. The firmware forgets its token when the kind
is changed so it re-announces on the next poll.

## Pairing

Zero-touch (path A in the spec) is the default:

1. `POST /api/v1/device/discover` with the identity above (no auth). Reply
   `registered: false` + `retry_after_s` (30 s): the device appears in the
   **Discovered** strip of Settings → Devices.
2. Admin clicks **Register** (optionally renames, picks the panel).
3. The next discover answers `registered: true` with `device_token`,
   `device_id` (adopt it; `device_id_changed` says if it differs) and `config`.
4. Token saved to flash; steady state begins.

A re-flashed board that lost its flash config re-acquires the same token by
MAC match on the first discover (path C), with no admin click.

Pairing code (path B): if a 6-digit code is stored (`PAIRING_CODE` in
`secrets.h`, or the web UI's Pair box), the device does
`POST /api/v1/device/register` with `X-Pairing-Code` instead and
self-registers. A rejected code is cleared and the device falls back to
discover. Both routes are rate limited (10 failures / 60 s / IP) and answer
`429` + `Retry-After`, which the firmware honours.

## Steady state (always on)

The protocol's "always on" variant: no deep sleep, poll on the server's
cadence, repaint only on a `200`. One cycle:

```
GET  /api/v1/device/<id>/frame        Authorization: Bearer <token>
                                      If-None-Match: "<etag>"   (unless forcing)
                                      ?button=<name>&button_event_id=<n>   (on a key press)
  200 -> {url, format:"bin", render_id, panel_w, panel_h, native_w, native_h, rotation?}
         GET <url> (no auth) -> 1,314,144 bytes into PSRAM -> paint GC16 -> store ETag
  304 -> nothing to paint (ETag + Content-Location headers still present)
  204 -> no frame rendered yet: bind a dashboard to the device
  401/403 -> token wiped; re-pair on the next cycle

POST /api/v1/device/<id>/status       Authorization: Bearer <token>
  {fw_version, model, rssi, ip, mac, panel_w, panel_h, power_plugged,
   can_stay_awake:true, next_sleep_s, button?, button_event_id?,
   viewport?:{x, y, w, h}}
  -> {config:{sleep_interval_s, always_on, awake_poll_s, ...}, next_poll_s,
      server_time, local_time, tz, tz_offset_seconds, rotation?}
```

Cadence: `next_poll_s` from the status reply wins. With **Stay awake** on,
the server derives it from `awake_poll_s` (5 to 300 s, default 15) and pulls
it earlier when a bound schedule or rotation step is about to change the
frame. The firmware clamps to 5 s to 1 h and never deep sleeps regardless;
with Stay awake off it simply polls on `sleep_interval_s`, which is slow but
harmless (the device card nags about it in the serial log).

**Viewport (Mk II).** The device's home overlay (Settings → Home screen)
takes part of the panel; `viewport` is what is left for the dashboard, sent
on every heartbeat while it is smaller than the panel: strip 0,0 1872x1228
(default), cards left 432,0 1440x1404, cards bottom 0,0 1872x1080, omitted
when the overlay is off. A server that honours it renders frames at w x h
(w*h/2 bytes, rows packed); the device paints them at x, y with white around
them. A full 1872x1404 frame is still accepted and painted whole, with the
overlay drawn over its edge, so a server that ignores the field keeps
working. A change of viewport asks for a fresh frame straight away; the
status (and so the new viewport) goes out after that frame request, so the
right-sized frame arrives on the following poll.

Frame geometry is asserted: `format` must be `bin` and `native_w/h`, when
sent, must be 1872x1404. A wrong kind or panel block fails loudly instead of
painting garbage. The download is checked against `Content-Length` and
against the exact frame size.

The heartbeat is sent **after** the frame, so a key's action (dispatched on
the frame GET) is already reflected in the frame painted on the same cycle.
`local_time` from the reply is kept for a future on-device clock overlay; the
wall clock itself is set from every response's `Date` header (no SNTP, works
on a LAN without internet).

A stored ETag with no pixels in RAM (power cycle) is handled the way the
spec describes: the first poll drops `If-None-Match`, and if a `304` ever
arrives without a held frame, `Content-Location` is fetched.

## Keys and the knob

Physical buttons use the spec's button wake path. A press queues
`button=<name>` with a monotonic `button_event_id` (persisted in flash so it
never goes backwards across power cycles), brings the next poll forward, and
rides on the frame GET; the same event is repeated on the status POST and
deduped server-side by id. A `200`/`304`/`204` on either request clears it:
one press, one event. Names are those in the device's **Controls** button
map; conventionally `left`, `right`, `refresh`. `refresh` additionally drops
the ETag so the server must answer `200`.

Which input sends which name is a device-side binding ("Server button
*name*" in the web page's Mapping table, default keys 1 and 2 = `left` and
`right`, key 3 = `refresh`); the server side of the same mapping is Settings →
Devices → Controls. The web page's virtual keys and the serial `l`/`r`/`1`-`4`
keys fire the same bindings as the hardware.

Deck navigation, local frame caches, overlays/patches (`overlay` schema 1/2)
and protocol v2 touch are **not** advertised. The device draws its own menu
and badges as row-generator overlays on top of the server frame, which needs
no server cooperation. Advertising `overlay` would make the server expect
us to apply its patch documents; revisit when key labels are drawn from
server-sent text.

## Frame format

`esp32_gray_bin`: exactly `1872 × 1404 / 2 = 1,314,144` bytes, no header.
Row-major, top-left origin, 936 bytes per row. **High nibble = left (even)
pixel, low nibble = right (odd) pixel.** `0x0` black … `0xF` white, linear.
No mirror: the firmware un-mirrors the ED103TC2 glass itself
(`EPD_MIRROR_H`, default 1). This is byte-identical to what `it8951.c`
streams and what `tools/png2frame.py` produces, so a frame goes
PSRAM → panel with no transform. Dithering (Floyd-Steinberg by default) and
contrast are server-side "Picture quality" settings on the device card.

## MQTT

Not used for the dashboard. The REST pull and the MQTT push deliver the same
envelope and artefacts, and REST needs no broker. MQTT arrives in Phase 3 for
the *device's own* topics (key labels and actions, volume, alarm offset, HA
discovery), which are outside the Tesserae device protocol.

## Server-side checklist

1. Settings → Devices: the device shows up under **Discovered** within a
   minute of boot (serial: `discovered as 'bedside'`). Click **Register**.
2. On the device card check: kind Seeed reTerminal E1003 (or the bedside
   catalog entry), panel 1872×1404 landscape, gamut `gray_16`, renderer
   `esp32_gray_bin`. Turn on **Stay awake**, set the awake poll to 10 to 15 s
   (with it off the device polls every `sleep_interval_s`, 60 s by default).
3. Bind a dashboard (1872×1404) to the device and Send. The next poll paints
   it; the web UI shows the ETag and frame count.
4. Controls: map `left` / `right` / `refresh` to rotation steps or pages.
   The web UI's key buttons fire them.
