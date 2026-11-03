#!/usr/bin/env python3
"""Regenerate src/soma_snapshot.c from SomaFM's channel list (the firmware's
offline copy; the panel fetches the live list once it is online):
    python3 tools/soma_snapshot.py > src/soma_snapshot.c
"""
import json, datetime, urllib.request
req = urllib.request.Request("https://api.somafm.com/channels.json", headers={"User-Agent": "tesserae-device-bedside"})
ch = json.load(urllib.request.urlopen(req, timeout=30))["channels"]
def esc(s): return s.replace("\\", "\\\\").replace('"', '\\"')
print("/*\n * soma_snapshot.c: SomaFM's channel list as it was on %s, from\n * https://api.somafm.com/channels.json. The panel shows this until it has\n * fetched the live list (radio.c), and whenever it has no network.\n * Regenerate: python3 tools/soma_snapshot.py > src/soma_snapshot.c\n */\n#include \"radio.h\"\n\nconst soma_channel_t SOMA_SNAPSHOT[] = {" % datetime.date.today().isoformat())
for c in ch:
    d = c["description"].strip()
    if len(d) > 79: d = d[:76].rstrip() + "..."
    print('    { "%s", "%s", "%s" },' % (esc(c["id"][:15]), esc(c["title"][:31]), esc(d)))
print("};\nconst int SOMA_SNAPSHOT_N = %d;" % len(ch))
