#!/bin/sh
# Hands-free bench flash for the Pico Plus 2 W (from the ed103tc2 handover):
# build, send the 1200-baud reset over the pico-sdk USB CDC port, wait for the
# RP2350 UF2 volume, copy the image, then tail the serial log with DTR asserted.
#   tools/flash.sh [log seconds, default 90]
set -e
cd "$(dirname "$0")/.."
PIO=${PIO:-$HOME/.platformio/penv/bin/pio}
PY=${PY:-$HOME/.platformio/penv/bin/python}
UF2=.pio/build/pimoroni_pico_plus_2w/firmware.uf2
SECS=${1:-90}

"$PIO" run

if [ ! -d /Volumes/RP2350 ] && [ -n "${BEDSIDE_IP:-192.168.50.216}" ]; then
  # Over WiFi first: the firmware's web API reboots into the bootloader.
  if curl -s -m 3 -X POST "http://${BEDSIDE_IP:-192.168.50.216}/api/cmd?bootsel" >/dev/null 2>&1; then
    echo "flash: asked the device for BOOTSEL over WiFi"
    for i in $(seq 1 30); do [ -d /Volumes/RP2350 ] && break; sleep 0.5; done
  fi
fi
if [ ! -d /Volumes/RP2350 ]; then
  PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
  if [ -n "$PORT" ]; then
    echo "flash: 1200-baud reset on $PORT"
    "$PY" - "$PORT" <<'PYEOF'
import serial, sys, time
try:
    s = serial.Serial(sys.argv[1], 1200); s.dtr = False; time.sleep(0.3); s.close()
except Exception as e:
    print("flash: reset request:", e)
PYEOF
  else
    echo "flash: no CDC port; hold BOOTSEL and plug in"
  fi
  for i in $(seq 1 60); do [ -d /Volumes/RP2350 ] && break; sleep 0.5; done
fi
if [ ! -d /Volumes/RP2350 ]; then
  # In BOOTSEL but macOS did not mount the drive: load over USB with picotool instead.
  PICOTOOL="$HOME/.platformio/packages/tool-picotool-rp2040-earlephilhower/picotool"
  if [ -x "$PICOTOOL" ] && ioreg -p IOUSB 2>/dev/null | grep -q "RP2350 Boot"; then
    echo "flash: drive not mounted; loading with picotool"
    "$PICOTOOL" load -x "$UF2" >/dev/null 2>&1 && { echo "flash: loaded, board rebooting"; exit 0; }
    echo "flash: picotool load failed"; exit 1
  fi
  echo "flash: RP2350 volume did not appear"; exit 1
fi
echo "flash: copying $(stat -f %z "$UF2") bytes"
sleep 1                                    # let the volume finish mounting
for i in $(seq 1 10); do
  if cp -X "$UF2" /Volumes/RP2350/ 2>/dev/null; then break; fi
  [ -d /Volumes/RP2350 ] || break          # gone = the copy took and the board rebooted
  sleep 0.5
done
sleep 2

echo "flash: waiting for the CDC port"
PORT=""
for i in $(seq 1 40); do PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1); [ -n "$PORT" ] && break; sleep 0.5; done
[ -n "$PORT" ] || { echo "flash: board did not re-enumerate (replug USB)"; exit 1; }
echo "flash: logging $PORT for ${SECS}s"
"$PY" - "$PORT" "$SECS" <<'PYEOF'
import serial, sys, time
port, secs = sys.argv[1], float(sys.argv[2])
end = time.time() + secs
s = None
while time.time() < end:
    try:
        if s is None:
            s = serial.Serial(port, 115200, timeout=0.5); s.dtr = True; s.rts = True
        line = s.readline()
        if line: sys.stdout.write(line.decode("utf-8", "replace")); sys.stdout.flush()
    except Exception as e:
        s = None; time.sleep(0.5)
PYEOF
