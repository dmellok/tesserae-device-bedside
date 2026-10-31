#!/bin/sh
# One-time setup for over-the-air updates: write the A/B partition table
# (tools/pt.json) and load the firmware into slot A. The config sector at 15 MB
# is outside both slots, so WiFi, pairing and settings survive.
#   tools/partition.sh            (device reachable at $BEDSIDE_IP, or already in BOOTSEL)
set -e
cd "$(dirname "$0")/.."
PIO=${PIO:-$HOME/.platformio/penv/bin/pio}
PICOTOOL=${PICOTOOL:-$HOME/.platformio/packages/tool-picotool-rp2040-earlephilhower/picotool}
OUT=.pio/build/pimoroni_pico_plus_2w
IP=${BEDSIDE_IP:-192.168.50.216}

"$PIO" run
"$PICOTOOL" partition create tools/pt.json "$OUT/pt.uf2"

if ! "$PICOTOOL" info >/dev/null 2>&1; then
  echo "partition: asking the device for BOOTSEL over WiFi"
  curl -s -m 3 -X POST "http://$IP/api/cmd?bootsel" >/dev/null 2>&1 || true
  for i in $(seq 1 40); do "$PICOTOOL" info >/dev/null 2>&1 && break; sleep 0.5; done
fi
"$PICOTOOL" info >/dev/null 2>&1 || { echo "partition: no device in BOOTSEL (hold BOOTSEL and plug in)"; exit 1; }

echo "partition: writing the partition table"
"$PICOTOOL" load "$OUT/pt.uf2"
"$PICOTOOL" reboot -u                      # the table takes effect on reboot; come back to BOOTSEL
for i in $(seq 1 40); do "$PICOTOOL" info >/dev/null 2>&1 && break; sleep 0.5; done
"$PICOTOOL" partition info
echo "partition: loading the firmware into slot A"
"$PICOTOOL" load -x "$OUT/firmware.uf2"
echo "partition: done; later updates go over the air (Settings > Device > Software)"
