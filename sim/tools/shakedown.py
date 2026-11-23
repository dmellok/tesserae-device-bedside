#!/usr/bin/env python3
"""
shakedown.py: drive the simulator through the panel's features and report what
looks wrong.

    make -C sim SAN=1 && python3 sim/tools/shakedown.py [--out DIR] [--only NAME,...]

Starts its own simulator (the AddressSanitizer build, its own flash file, port
8091, no sound) and after every step checks:
  - the glass against the firmware's own picture of the screen (/frame): a
    region that differs once the panel is idle is a paint that went wrong
    (two-level DU leftovers are counted apart: the settle pass fixes those)
  - /api/state parses and fits the device's 10 KB reply buffer (net.c)
  - the log: sanitizer reports, errors, stalls
  - the process is still alive
Findings go to DIR/report.md with pictures (glass | intended | difference).
"""
import argparse, json, os, random, re, shutil, signal, socket, socketserver, ssl, struct, subprocess, sys, threading, time, traceback, urllib.request, zlib

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
W, H, ROWB = 1872, 1404, 936
DEVICE_JSON_BUF = 10240          # net.c: s_json[10240] holds the /api/state reply on the device

# ---------------------------------------------------------------------------------------------
class Sim:
    def __init__(self, out, port=8091, binary=None):
        self.out, self.port = os.path.abspath(out), port   # absolute: the simulator runs from the repo root
        self.base = "http://127.0.0.1:%d" % port
        self.bin = binary or os.path.join(ROOT, "sim", "build-san", "bedside-sim")
        self.flash = os.path.join(self.out, "flash.bin")
        self.logpath = os.path.join(self.out, "sim.log")
        self.proc = None
        self.log_mark = 0
        self.warped = False

    def start(self, reset=True):
        if reset and os.path.exists(self.flash): os.remove(self.flash)
        env = dict(os.environ, ASAN_OPTIONS="halt_on_error=0:detect_leaks=0", UBSAN_OPTIONS="print_stacktrace=1")
        self.logf = open(self.logpath, "ab")
        self.proc = subprocess.Popen([self.bin, "--port", str(self.port), "--flash", self.flash, "--no-audio"],
                                     stdout=self.logf, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, cwd=ROOT)
        self.wait_up()

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try: self.proc.wait(5)
            except subprocess.TimeoutExpired: self.proc.kill()

    def alive(self): return self.proc and self.proc.poll() is None

    def wait_up(self, timeout=40):
        t = time.time()
        while time.time() - t < timeout:
            try:
                self.get("/sim/status", timeout=1); return True
            except Exception: time.sleep(0.3)
        raise RuntimeError("the simulator did not come up")

    def get(self, path, timeout=10, method="GET"):
        req = urllib.request.Request(self.base + path, method=method)
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.read(), r.status, dict(r.headers)

    def post(self, path): return self.get(path, method="POST")[0]
    def js(self, path): return json.loads(self.get(path)[0].decode("utf-8", "replace"))
    def cmd(self, name, value=""): return self.post("/api/cmd?%s=%s" % (name, urllib.request.quote(str(value), safe="")))
    def set(self, key, val): return self.cmd("set", "%s|%s" % (key, val))
    def key(self, i, act="tap"): self.post("/sim/input?key=%d&act=%s" % (i, act))
    def knob(self, act): self.post("/sim/input?knob=%s" % act)
    def turn(self, n):
        if n: self.post("/sim/input?knob=%s&n=%d" % ("cw" if n > 0 else "ccw", abs(n)))
    def menu(self): return self.js("/sim/menu")
    def status(self): return self.js("/sim/status")
    def instant(self, on): self.post("/sim/speed?instant=%d" % (1 if on else 0))
    def warp_to(self, hhmm): self.warped = True; return json.loads(self.post("/sim/time?to=" + hhmm))
    def warp(self, s): self.warped = True; return json.loads(self.post("/sim/time?add=%d" % s))

    def state(self):
        raw = self.get("/api/state")[0]
        self.last_state_raw = raw
        return json.loads(raw.decode("utf-8", "replace")), len(raw)

    def new_log(self):
        """Log lines since the last call."""
        self.logf.flush()
        with open(self.logpath, "rb") as f:
            f.seek(self.log_mark); data = f.read(); self.log_mark = f.tell()
        return data.decode("utf-8", "replace").splitlines()

    def wait_idle(self, quiet=1.2, timeout=40):
        """Until the panel has been idle for `quiet` seconds (a paint queued behind another shows as busy again)."""
        t0 = time.time(); since = None
        while time.time() - t0 < timeout:
            s = self.status()
            if s["panel"]["busy"]: since = None
            else:
                since = since or time.time()
                if time.time() - since >= quiet: return True
            time.sleep(0.1)
        return False

    def settle(self, timeout=6):
        """After a knob or key action: until the menu stops changing and the panel is idle."""
        t0 = time.time(); last = None; same = 0
        while time.time() - t0 < timeout:
            time.sleep(0.25)
            m = json.dumps(self.menu()); busy = self.status()["panel"]["busy"]
            same = same + 1 if (m == last and not busy) else 0
            last = m
            if same >= 3: break
        return self.menu()

    def glass(self): return self.get("/sim/glass")[0]
    def preview(self):
        b = self.get("/frame?s=2")[0]
        w, h = b[0] | b[1] << 8, b[2] | b[3] << 8
        return w, h, b[4:]

# ---------------------------------------------------------------------------------------------
def png(path, w, h, rows):
    """rows: list of bytearrays of RGB."""
    raw = b"".join(b"\x00" + bytes(r) for r in rows)
    c = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + c(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + c(b"IDAT", zlib.compress(raw, 6)) + c(b"IEND", b""))

def compare(sim):
    """The glass against the firmware's intended picture, sampled as /frame?s=2 samples (even x, even y)."""
    g = sim.glass()
    w, h, p = sim.preview()
    wrong = two = 0
    gimg, pimg = [], []
    boxes = [W, H, 0, 0]
    for j in range(h):
        gy = j * 2
        grow = g[gy * ROWB:(gy + 1) * ROWB]
        prow = p[j * (w // 2):(j + 1) * (w // 2)]
        gr, pr = bytearray(), bytearray()
        for i in range(w):
            gv = grow[i] >> 4                      # x = 2i: always the high nibble
            pv = (prow[i >> 1] >> 4) if i % 2 == 0 else (prow[i >> 1] & 15)
            gr.append(gv); pr.append(pv)
            if gv != pv:
                if gv in (0, 15) and (pv >= 8) == (gv == 15): two += 1
                else:
                    wrong += 1
                    boxes[0] = min(boxes[0], i * 2); boxes[1] = min(boxes[1], gy)
                    boxes[2] = max(boxes[2], i * 2); boxes[3] = max(boxes[3], gy)
        gimg.append(gr); pimg.append(pr)
    n = w * h
    return {"wrong": wrong / n, "two": two / n, "box": boxes if wrong else None, "g": gimg, "p": pimg, "w": w, "h": h}

def save_cmp(c, path):
    """glass | intended | difference (red: wrong, amber: two-level), at quarter size."""
    w, h = c["w"] // 2, c["h"] // 2
    rows = []
    for j in range(h):
        r = bytearray()
        gr, pr = c["g"][j * 2], c["p"][j * 2]
        for src in (gr, pr):
            for i in range(w): v = src[i * 2] * 17; r += bytes((v, v, v))
            r += b"\x80\x80\x80" * 8
        for i in range(w):
            gv, pv = gr[i * 2], pr[i * 2]
            if gv == pv: v = 230 if gv > 7 else 120; r += bytes((v, v, v))
            elif gv in (0, 15) and (pv >= 8) == (gv == 15): r += b"\xe3\x8b\x2c"
            else: r += b"\xd0\x20\x20"
        rows.append(r)
    png(path, w * 3 + 16, h, rows)

# ---------------------------------------------------------------------------------------------
BAD_LOG = [
    (re.compile(r"AddressSanitizer|runtime error:|UndefinedBehaviorSanitizer|SUMMARY: "), "sanitizer"),
    (re.compile(r"\b(FAILED|failed|error|overflow|truncated|too large|unknown|invalid|bad )", re.I), "log"),
    (re.compile(r"^stall: "), "stall"),
]
LOG_OK = re.compile(r"SomaFM|no address for|rest: connect failed|could not fetch|mqtt:|tap expired|"
                    r"connect failed \(|no server|bind: unknown input|"
                    r"radio: .*; retry \d+ in|radio: tcp error|radio: stopped \(|"
                    r"sim: tls: handshake failed .*not correctly signed by the trusted CA", re.I)   # the radio and https scenarios' broken servers, on purpose

# Network fetches that hold the main loop by design (radio.h: the SomaFM list
# "blocks a few seconds"; the release list): a stall right after one is noted.
KNOWN_WAIT = re.compile(r"radio: SomaFM list|could not fetch the SomaFM list|ota: \d+ release\(s\) on GitHub|ota: could not reach GitHub")

class Run:
    def __init__(self, sim, out):
        self.sim, self.out = sim, out
        self.findings, self.notes, self.steps = [], [], 0
        self.max_json = 0
        self.seen = set()

    def find(self, area, title, detail, pic=None, key=None):
        k = key or (area, title)
        if k in self.seen: return
        self.seen.add(k)
        self.findings.append({"area": area, "title": title, "detail": detail, "pic": pic})
        print("  !! [%s] %s: %s" % (area, title, detail.splitlines()[0] if detail else ""))

    def check(self, step, scenario, paint=True, settle_retry=True):
        """After a step: alive, JSON, log, and (when idle) the glass."""
        self.steps += 1
        sim = self.sim
        if not sim.alive():
            tail = "\n".join(open(sim.logpath, errors="replace").read().splitlines()[-40:])
            self.find(scenario, "the simulator stopped (crash) at: " + step, tail, key=("crash", step))
            sim.start(reset=False)
            return
        try:
            st, size = sim.state()
            self.max_json = max(self.max_json, size)
            try: sim.last_state_raw.decode("utf-8")
            except UnicodeDecodeError as e:
                raw = sim.last_state_raw; at = e.start
                self.find("web", "/api/state carries the panel font's private codes (not UTF-8): the web app shows a replacement character",
                          "byte 0x%02x in: ...%s..." % (raw[at], raw[max(0, at - 60):at + 20].decode("latin-1")), key=("utf8", raw[max(0, at - 20):at]))
            if size >= DEVICE_JSON_BUF - 1:
                self.find("web", "/api/state is %d bytes: the device's buffer is %d, so its reply is cut short" % (size, DEVICE_JSON_BUF),
                          "after: %s (%s)" % (step, scenario), key=("json-size",))
        except Exception as e:
            self.find("web", "/api/state is not valid JSON", "after %s: %s" % (step, e), key=("json", step))
        recent = []                                                      # the lines just before, for KNOWN_WAIT
        for line in sim.new_log():
            for rx, kind in BAD_LOG:
                if rx.search(line) and not LOG_OK.search(line):
                    if kind == "stall" and sim.warped: continue          # a clock jump reads as a stall
                    hit = [l for l in recent if KNOWN_WAIT.search(l)]
                    if kind == "stall" and hit:                          # a fetch the firmware makes in the loop on purpose
                        self.notes.append("known wait: %s after \"%s\"" % (line.strip(), hit[-1].strip()[:60])); continue
                    self.find(scenario if kind != "sanitizer" else "memory", "%s line: %s" % (kind, line.strip()[:110]),
                              "during: %s" % step, key=(kind, re.sub(r"\d+", "#", line.strip())[:80]))
            recent = (recent + [line])[-8:]
        sim.warped = False
        if not paint: return
        try:
            if sim.state()[0].get("content") == "WiFi setup": return
        except Exception: pass
        sim.wait_idle()
        c = compare(sim)
        if c["wrong"] > 0.004 and settle_retry:                       # a paint may still be on its way: look again
            time.sleep(2.5); sim.wait_idle(); c = compare(sim)
        if c["wrong"] > 0.004:
            name = "glass-%03d.png" % self.steps
            save_cmp(c, os.path.join(self.out, name))
            b = c["box"]
            self.find(scenario, "the panel doesn't show what the firmware drew, after: " + step,
                      "%.1f%% of the screen differs once the panel is idle, around x %d-%d, y %d-%d (screen: %s)"
                      % (c["wrong"] * 100, b[0], b[2], b[1], b[3], self.screen()), pic=name, key=("glass", scenario, step))
        return c

    def screen(self):
        try:
            m = self.sim.menu()
            return m.get("title") or m["mode"]
        except Exception: return "?"

# ---------------------------------------------------------------------------------------------
# Scenarios
def go_home(sim):
    for _ in range(6):
        if sim.menu()["mode"] == "home": return
        sim.knob("hold"); sim.settle()
    sim.cmd("page", "home"); sim.settle()

def sc_boot(run):
    sim = run.sim
    time.sleep(3)
    run.check("first boot", "boot", paint=False)
    sim.wait_idle(quiet=2.5, timeout=60)
    m = sim.menu()
    if not (m["mode"] == "list" and "Setup" in m.get("title", "")):
        run.find("setup", "a fresh panel did not open first-run setup", json.dumps(m)[:300])
    run.check("first boot: setup step 1", "boot")

def sc_wizard(run):
    sim = run.sim
    seen = []
    for step in range(12):
        m = sim.menu()
        if m["mode"] != "list" or "Setup" not in m.get("title", ""): break
        seen.append(m["title"])
        labels = [r["label"] for r in m["rows"]]
        if "Add an alarm" in labels:                                   # the alarm step: add one, it should stay here
            sim.turn(labels.index("Add an alarm") - m["sel"]); time.sleep(0.6); sim.knob("press"); time.sleep(2)
            run.check("setup: add an alarm", "setup")
            m2 = sim.menu()
            if m2.get("title") != m["title"]: run.find("setup", "Add an alarm in setup left the step", json.dumps(m2)[:300])
            if step == 4 or "Alarm" in m.get("title", ""): pass
            m = sim.menu(); labels = [r["label"] for r in m["rows"]]
        if step == 2:                                                  # Back from step 3 goes to step 2, then forward again
            sim.knob("hold"); time.sleep(2)
            mb = sim.menu()
            if mb.get("title") != seen[-2]: run.find("setup", "Back in setup did not go to the step before", "%s -> %s" % (m["title"], mb.get("title")))
            run.check("setup: back a step", "setup")
            sim.knob("press") if mb["rows"][mb["sel"]]["kind"] == "action" else None
            time.sleep(2); m = sim.menu(); labels = [r["label"] for r in m["rows"]]
        go = next((i for i, r in enumerate(m["rows"]) if r["label"] in ("Start", "Continue", "Skip for now", "Finish")), None)
        if go is None: run.find("setup", "no way on from a setup step", json.dumps(m)[:300]); break
        sim.turn(go - m["sel"]); time.sleep(0.6)
        sim.knob("press"); time.sleep(1.5)
        run.check("setup: on from '%s'" % m["title"], "setup")
    if len(set(seen)) < 7: run.find("setup", "setup showed %d of 7 steps" % len(set(seen)), " / ".join(seen))
    m = sim.menu()
    if m["mode"] != "home": run.find("setup", "Finish did not go home", json.dumps(m)[:200])
    st, _ = sim.state()
    if st["ui"]["wiz"] != -1: run.find("setup", "setup still marked running after Finish", "wiz=%s" % st["ui"]["wiz"])
    # from the phone: start it again, then finish
    sim.set("wiz", 0); time.sleep(2.5)
    if "Setup" not in sim.menu().get("title", ""): run.find("setup", "set wiz|0 from the web app did not show setup on the panel", "")
    run.check("setup started from the web app", "setup")
    sim.set("wiz", 3); time.sleep(2.5); run.check("setup step 4 from the web app", "setup")
    sim.set("wiz", -1); time.sleep(2.5); run.check("setup finished from the web app", "setup")

DANGER = re.compile(r"reset|restart|wi-?fi setup|update|install|forget|re-?pair|erase|delete|remove|setup again|factory|reboot|clear screen|sleep the panel", re.I)

def sc_menus(run):
    """Every list in the menus: enter each submenu, flip each toggle and back, open each choice and value and leave."""
    sim = run.sim
    sim.instant(True)
    go_home(sim)
    sim.knob("press"); time.sleep(1.2)
    root = sim.menu()
    if root["mode"] != "list": run.find("menus", "the knob on the home page did not open the menu", json.dumps(root)[:200]); return
    visited, count = set(), [0]

    def goto(i):
        m = sim.menu(); sim.turn(i - m["sel"]); m = sim.settle()
        if m["sel"] != i: run.find("menus", "the highlight could not reach row %d of %s" % (i, m.get("title")), "it stopped at %d of %d" % (m["sel"], len(m["rows"])), key=("reach", m.get("title"), i))
        return m

    def walk(depth):
        m = sim.menu()
        title = m.get("title", "?")
        if title in visited or depth > 5: return
        visited.add(title)
        for i, r in enumerate(m["rows"]):
            if count[0] > 400: return
            if r["kind"] in ("info", "back") or DANGER.search(r["label"]): continue
            count[0] += 1
            m = goto(i)
            if m.get("title") != title: return
            label = "%s > %s" % (title, r["label"])
            sim.knob("press"); after = sim.settle()
            run.check("press " + label, "menus", settle_retry=False)
            if r["kind"] == "sub":
                if after["mode"] == "list" and after.get("title") != title:
                    walk(depth + 1)
                    sim.knob("hold"); sim.settle()
                elif after["mode"] == "home":
                    sim.knob("press"); sim.settle()          # a page, not a list: back into the menu
                    run.find("menus", "%s opened the home page, not a list" % label, json.dumps(after)[:200], key=("subhome", label))
                    return
                if sim.menu().get("title") != title:
                    run.find("menus", "Back from %s did not return to %s" % (label, title), json.dumps(sim.menu())[:200], key=("back", label))
                    return
            elif r["kind"] == "toggle":
                if after.get("title") == title and after["rows"][i]["value"] == r["value"] and r["value"]:
                    run.find("menus", "%s: pressing the toggle didn't change it" % label, "value stays %r" % r["value"], key=("toggle", label))
                sim.knob("press"); sim.settle()            # flip it back
                run.check("press again " + label, "menus", paint=False)
            elif r["kind"] in ("choice", "value"):
                if after["mode"] in ("choice", "value"):
                    sim.knob("hold"); sim.settle()         # leave without choosing
                    if sim.menu()["mode"] != "list": run.find("menus", "%s: Back from the %s didn't return to the list" % (label, r["kind"]), json.dumps(sim.menu())[:200])
                elif after.get("title") != title:
                    sim.knob("hold"); sim.settle()
            elif r["kind"] == "action":
                if sim.menu()["mode"] == "home": return     # an action that went home (a page): stop this list
                if sim.menu().get("title") != title:
                    sim.knob("hold"); sim.settle()
            if sim.menu().get("title") != title: return
    walk(0)
    run.notes.append("menus: %d lists visited, %d rows pressed: %s" % (len(visited), count[0], ", ".join(sorted(visited))))
    go_home(sim)
    sim.instant(False)
    run.check("back home after the menu walk", "menus")

def sc_keys(run):
    sim = run.sim
    go_home(sim); sim.wait_idle()
    st0, _ = sim.state()
    for i, name in enumerate(["rain", "lights", "ac"]):
        sim.key(i, "tap"); time.sleep(1.2)
        run.check("key %d (%s) tap" % (i + 1, name), "keys")
        sim.wait_idle(); time.sleep(4.5)                               # the confirmation card times out
        run.check("key %d card gone" % (i + 1), "keys")
        sim.key(i, "tap"); time.sleep(1.2); sim.wait_idle(); time.sleep(4.5)
        run.check("key %d tap again" % (i + 1), "keys")
    st1, _ = sim.state()
    if st1["local"]["rain_on"] != st0["local"]["rain_on"]: run.find("keys", "two taps of the Rain key didn't come back to where they started", "")
    for i in range(3):
        sim.key(i, "hold"); time.sleep(2.5)
        run.check("key %d hold" % (i + 1), "keys")
        go_home(sim)
    sim.key(3, "tap"); time.sleep(1.5)
    if sim.menu()["mode"] != "list": run.find("keys", "key 4 (Menu) did not open the menu", json.dumps(sim.menu())[:200])
    run.check("key 4 opens the menu", "keys")
    sim.key(3, "tap"); time.sleep(1.5)
    run.check("key 4 again", "keys")
    go_home(sim)
    # a quick tap while a full repaint runs is dropped on purpose (inputs.c); a hold through it should still count
    # web app virtual keys
    for k in (1, 2, 3):
        sim.cmd("key", k); time.sleep(1.5); run.check("web app key %d" % k, "keys")
        sim.cmd("key", k); time.sleep(1.5)
    go_home(sim)

def sc_alarm(run):
    sim = run.sim
    go_home(sim)
    st, _ = sim.state()
    n = len(st["ui"]["alarms"])
    if n == 0: sim.set("alarm_add", 1); time.sleep(0.5)
    sim.set("alarm", "0,6,30,1,127,15"); time.sleep(1.5)
    run.check("alarm 0 at 06:30 every day, 15 min early", "alarm")
    # the dawn chorus, stopped from the knob
    sim.warp_to("06:16"); time.sleep(3)
    st, _ = sim.state()
    if st["local"]["phase"] != 1: run.find("alarm", "no dawn chorus at 06:16 for a 06:30 alarm 15 min early", "phase=%s" % st["local"]["phase"])
    run.check("dawn chorus running", "alarm")
    sim.knob("press"); time.sleep(1.0)
    run.check("dawn chorus stopped with the knob: the home page", "alarm", settle_retry=False)
    st, _ = sim.state()
    if st["local"]["phase"] != 0: run.find("alarm", "the knob didn't stop the dawn chorus", "phase=%s" % st["local"]["phase"])
    sim.warp_to("06:31"); time.sleep(3)
    st, _ = sim.state()
    if st["local"]["phase"] != 0: run.find("alarm", "the alarm rang after its dawn chorus was stopped (that ring should be skipped)", "phase=%s" % st["local"]["phase"])
    run.check("06:31 after a skipped ring", "alarm")
    # next day: let the chorus run into the ring
    sim.warp_to("06:29"); time.sleep(2); sim.warp(70); time.sleep(3)
    st, _ = sim.state()
    if st["local"]["phase"] != 2: run.find("alarm", "the alarm didn't ring at 06:30", "phase=%s" % st["local"]["phase"])
    run.check("alarm ringing", "alarm")
    sim.set("alarm_snooze", 1); time.sleep(2)
    run.check("snoozed from the web app", "alarm")
    st, _ = sim.state()
    snooze = st["local"].get("snooze_min") or 9
    sim.warp(snooze * 60 + 30); time.sleep(3)
    st, _ = sim.state()
    if st["local"]["phase"] != 2: run.find("alarm", "the snoozed alarm didn't ring again", "phase=%s after %d min" % (st["local"]["phase"], snooze))
    run.check("ringing again after the snooze", "alarm")
    sim.key(0, "tap"); time.sleep(2)
    run.check("key 1 while ringing", "alarm")
    st, _ = sim.state()
    if st["local"]["phase"] != 2:
        # key 1 snoozed: switching the alarm off has to take its snooze with it
        sim.set("alarm", "0,6,30,0,127,15"); time.sleep(1)
        st, _ = sim.state()
        snz = (st.get("ui", {}).get("chorus") or st.get("chorus") or {}).get("snooze_s", 0)
        if snz: run.find("alarm", "the snooze is still pending after its alarm was switched off", "snooze_s=%s" % snz)
        sim.warp(snooze * 60 + 30); time.sleep(3)
        st, _ = sim.state()
        if st["local"]["phase"] == 2: run.find("alarm", "a snoozed alarm rang after it was switched off", "phase=2")
        run.check("snoozed, then the alarm switched off", "alarm")
        sim.set("alarm", "0,6,30,1,127,15"); time.sleep(1)
        st, _ = sim.state()
    if st["local"]["phase"] == 2:
        sim.knob("hold"); time.sleep(2)
    st, _ = sim.state()
    if st["local"]["phase"] != 0: run.find("alarm", "couldn't stop the ringing alarm with the knob", "phase=%s" % st["local"]["phase"])
    run.check("alarm stopped", "alarm")
    # four alarms: the most the JSON carries
    for _ in range(4): sim.set("alarm_add", 1)
    time.sleep(1)
    run.check("four alarms", "alarm")
    st, size = sim.state()
    run.notes.append("alarms: %d set; /api/state %d bytes" % (len(st["ui"]["alarms"]), size))
    sim.set("alarm_del", 3); sim.set("alarm_del", 2); sim.set("alarm_del", 1)
    time.sleep(1); run.check("alarms deleted", "alarm")

def sc_sleep(run):
    sim = run.sim
    go_home(sim)
    st, _ = sim.state()
    run.notes.append("sleep: alarms before the timer test: %s" % json.dumps(st["ui"]["alarms"]))
    for i, a in enumerate(st["ui"]["alarms"]):
        sim.set("alarm", "%d,%d,%d,0,%d,%d" % (i, a.get("h", 7), a.get("m", 0), a.get("mask", a.get("days", 31)) if isinstance(a.get("mask", a.get("days", 31)), int) else 31, a.get("early", 15)))
    sim.set("rain_on", 1); sim.set("sleep", 15); time.sleep(2)
    run.check("rain with a 15 min sleep timer", "sleep")
    sim.warp(14 * 60 + 30); time.sleep(3)
    st, _ = sim.state()
    if not st["local"]["rain_on"]: run.find("sleep", "the sleep timer stopped the sound early", "at 14.5 of 15 min")
    sim.warp(90); time.sleep(3)
    st, _ = sim.state()
    if st["local"]["rain_on"]: run.find("sleep", "the sleep timer didn't stop the sound", "16 min after a 15 min timer")
    run.check("sleep timer ended", "sleep")

SETS = {
    "vol": [0, 10, 5], "rain_lvl": [1, 5, 3], "rain_kind": [0, 1, 2, 3, 4, 0], "lights_on": [1, 0], "bright": [0, 100, 30],
    "colour": [1, 0], "ac_mode": [1, 2, 3, 4, 0], "setpoint": [32, 60, 44], "fan": [0, 3, 1], "night": [1, 0],
    "keylights": [0, 255, 40], "patter": [0, 100, 60], "wash": [0, 100, 55], "tone": [0, 100, 50], "wind": [0, 100, 20],
    "thunder": [100, 0], "thunder_near": [100, 35], "surface": [1, 2, 3, 0], "key_col": [1, 0], "menu_col": [1, 0],
    "night_col": [1, 0], "knob_light": [0, 1], "auto_night": [0, 1, 2], "clock12": [1, 0], "snooze": [5, 15, 9],
    "night_keys": [1, 2, 0], "overlay": [1, 0], "show": [0, 15, 7], "at_night": [0, 1, 2], "large_text": [1, 0],
    "clock_du": [1, 2, 0], "live_demo": [1, 0], "live_layout": [1, 2, 3, 0], "live_dark": [1, 2, 0], "tz": [1, 5, 0],
    "autoupdate": [1, 0], "speaker": [0, 1], "bird_soft": [0, 100, 35], "spk_gain": [-6, 6, 0], "spk_soft": [1, 0],
    "spk_hpf": [1, 0], "spk_limit": [1, 0], "refresh_mode": [0, 2, 1], "turn_hints": [1, 0], "quick_menus": [0, 1],
    "settle_full": [1, 0], "stop_clean": [0, 1], "clean_min": [5, 120, 60], "wf_temp": [10, 30, 0], "auto_time": [0, 1],
    "birds": [1, 255], "bird_vol": [0, 10, 6], "dash": [1, 2, 0], "sleep": [30, 0],
}
JUNK = ["-1", "999999", "abc", "", "2147483647", "-2147483648", "1|2|3"]

def sc_settings(run):
    sim = run.sim
    go_home(sim)
    sim.instant(True)
    for k, vals in SETS.items():
        for v in vals:
            sim.set(k, v); time.sleep(0.25)
        run.check("set %s through %s" % (k, vals), "settings", paint=(k in ("night", "large_text", "live_layout", "live_dark", "clock12", "tz", "dash", "live_demo", "overlay", "show", "clock_du")))
    for k in SETS:
        for j in JUNK: sim.set(k, j)
        run.check("junk values for %s" % k, "settings", paint=False)
    for k in ("alarm", "alarm_del", "station", "soma", "rec", "wiz", "pick"):
        for j in JUNK + ["0,99,99,1,1,1", "9,1,1,1,1,1", "0,1,1,1,128,61"]: sim.set(k, j)
        run.check("junk values for %s" % k, "settings", paint=False)
    for name in ("knob", "key", "keyhold", "page", "scenario", "bind", "label", "backlight", "show", "movemode", "longpress", "name", "device", "live_loc", "live_src", "live_base"):
        for j in JUNK + ["x" * 300]: sim.cmd(name, j)
        run.check("junk for the %s command" % name, "settings", paint=False)
    sim.set("wiz", -1); go_home(sim); sim.instant(False)
    time.sleep(2); run.check("home after the settings sweep", "settings")

def sc_web(run):
    sim = run.sim
    for path in ("/", "/desktop", "/api/files", "/api/soma", "/api/log?from=0", "/frame?s=4", "/frame?s=8", "/frame?s=1", "/nope"):
        try:
            b, code, _ = sim.get(path)
            if path != "/nope" and code != 200: run.find("web", "%s answered %d" % (path, code), "")
        except urllib.error.HTTPError as e:
            if path != "/nope": run.find("web", "%s answered %d" % (path, e.code), "")
        except Exception as e:
            run.find("web", "%s failed: %s" % (path, e), "")
    # a file: upload in two chunks, list, download, delete
    data = bytes(range(256)) * 200                                     # 51 200 bytes
    try:
        for off in (0, 32768):
            chunk = data[off:off + 32768]
            req = urllib.request.Request("%s/api/file?name=shake.bin&off=%d&total=%d" % (sim.base, off, len(data)), data=chunk, method="POST")
            r = json.loads(urllib.request.urlopen(req, timeout=10).read())
            if not r.get("ok"): run.find("files", "upload chunk at %d refused: %s" % (off, r), "")
        files = sim.js("/api/files")
        if "shake.bin" not in json.dumps(files): run.find("files", "the uploaded file isn't listed", json.dumps(files)[:300])
        back = sim.get("/api/file?name=shake.bin")[0]
        if back != data: run.find("files", "the downloaded file differs from the upload", "%d bytes back of %d" % (len(back), len(data)))
        r = json.loads(sim.post("/api/file?name=shake.bin&delete=1"))
        if not r.get("ok"): run.find("files", "delete refused", json.dumps(r))
    except Exception as e:
        run.find("files", "file round trip failed: %s" % e, traceback.format_exc()[-500:])
    run.check("web routes and a file", "web", paint=False)

def sc_reboot(run):
    sim = run.sim
    sim.set("vol", 7); sim.set("clock12", 1); sim.set("night_keys", 1); sim.set("snooze", 15)
    time.sleep(4.5)                                                    # settings save 3 s after a change
    st0, _ = sim.state()
    sim.post("/sim/reboot"); time.sleep(2); sim.wait_up(); time.sleep(3)
    run.check("reboot", "reboot", paint=False)
    sim.wait_idle(quiet=3, timeout=60)
    st1, _ = sim.state()
    for k in ("vol", "clock12", "night_keys", "snooze_min"):
        a, b = st0["local"].get(k), st1["local"].get(k)
        if a != b: run.find("reboot", "%s didn't survive a restart" % k, "%r before, %r after" % (a, b))
    if st1["ui"]["wiz"] != -1: run.find("reboot", "setup came back after a restart", "wiz=%s" % st1["ui"]["wiz"])
    run.check("the home page after a restart", "reboot")
    # a menu opened while the first home page is still painting must survive it
    sim.post("/sim/reboot"); time.sleep(2); sim.wait_up()
    t0 = time.time()
    while time.time() - t0 < 30:
        if sim.state()[0]["content"] == "live": break
        time.sleep(0.2)
    time.sleep(0.6)
    sim.knob("press"); time.sleep(0.5)
    run.check("menu opened as the first home page paints", "reboot")
    go_home(sim); sim.wait_idle(quiet=2)

def sc_wifi_setup(run):
    sim = run.sim
    sim.cmd("wifisetup", 1); time.sleep(2); sim.wait_up(); time.sleep(4)
    st, _ = sim.state()
    if st["content"] != "WiFi setup": run.find("wifi", "WiFi setup didn't show the setup screen", "content=%s" % st["content"])
    run.check("WiFi setup screen", "wifi")
    try:
        page = sim.get("/")[0].decode()
        if "Set up your bedside panel" not in page: run.find("wifi", "the setup page isn't served in setup mode", page[:200])
        info = sim.js("/api/setupinfo")
        if not info.get("networks"): run.find("wifi", "setupinfo lists no networks", json.dumps(info))
    except Exception as e: run.find("wifi", "setup page failed: %s" % e, "")
    sim.cmd("wifi", "Simulator|"); time.sleep(0.5); sim.cmd("reboot", 1); time.sleep(2); sim.wait_up(); time.sleep(4)
    sim.wait_idle(quiet=2, timeout=60)
    st, _ = sim.state()
    if st["content"] == "WiFi setup": run.find("wifi", "still in setup after saving the WiFi", "")
    run.check("back on the network after setup", "wifi")

def sc_night(run):
    sim = run.sim
    go_home(sim)
    sim.set("auto_night", 1); sim.set("night", 0); time.sleep(1)
    sim.warp_to("20:58"); time.sleep(2); sim.warp(180); time.sleep(4)
    st, _ = sim.state()
    if not st["local"].get("night"): run.find("night", "auto night (21:00 to 07:00) didn't come on at 21:00", "night=%s at 21:01" % st["local"].get("night"))
    run.check("auto night in the evening", "night")
    sim.warp_to("07:01"); time.sleep(4)
    st, _ = sim.state()
    if st["local"].get("night"): run.find("night", "auto night didn't go off at 07:00", "")
    run.check("auto night in the morning", "night")
    sim.set("auto_night", 0)
    sim.set("night", 1); time.sleep(2); run.check("night on by hand", "night")
    sim.knob("press"); time.sleep(1.5); run.check("menu at night", "night")
    go_home(sim); sim.set("night", 0); time.sleep(2); run.check("night off", "night")

def sc_reset(run):
    sim = run.sim
    sim.cmd("factory_reset", "yes"); time.sleep(2); sim.wait_up(); time.sleep(4)
    sim.wait_idle(quiet=2.5, timeout=60)
    m = sim.menu()
    if "Setup" not in m.get("title", ""): run.find("reset", "after a factory reset, first-run setup didn't start", json.dumps(m)[:200])
    run.check("after a factory reset", "reset")

# ---------------------------------------------------------------------------------------------
# The radio, against a stream server on this Mac: no internet, and every way a
# stream can go wrong on demand. The audio is silent MP3 frames (MPEG-1 Layer
# III, 128 kbps, 44.1 kHz, mono, empty main data): real frames the decoder
# syncs to and decodes, without an encoder on the Mac.
MP3_FRAME = bytes([0xFF, 0xFB, 0x90, 0xC0]) + bytes(417 - 4)
RADIO_TITLE, RADIO_NAME = "Shakedown - Silence", "Shakedown FM"

class StreamServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
    def __init__(self, tls=None):
        self.hits, self.tls, self.tls_errors = {}, tls, []
        super().__init__(("127.0.0.1", 0), StreamHandler)
        self.port = self.server_address[1]
        threading.Thread(target=self.serve_forever, daemon=True).start()

class StreamHandler(socketserver.BaseRequestHandler):
    def handle(self):
        if self.server.tls:                                         # https: the handshake, then the same streams
            try: self.request = self.server.tls.wrap_socket(self.request, server_side=True)
            except (ssl.SSLError, OSError) as e: self.server.tls_errors.append(str(e)); return
        req = b""
        self.request.settimeout(5)
        while b"\r\n\r\n" not in req:
            d = self.request.recv(4096)
            if not d: return
            req += d
        head = req.decode("latin-1")
        path = head.split(" ")[1] if " " in head else "/"
        srv = self.server
        srv.hits[path] = srv.hits.get(path, 0) + 1
        send = self.request.sendall
        try:
            if path == "/404":
                send(b"HTTP/1.0 404 Not Found\r\nContent-Type: text/plain\r\n\r\nno such stream"); return
            if path == "/aac":
                send(b"HTTP/1.0 200 OK\r\nContent-Type: audio/aac\r\n\r\n" + bytes(4096)); return
            if path == "/ua" and "user-agent:" in head.lower():
                send(b"HTTP/1.0 403 Forbidden\r\n\r\n"); return
            if path == "/garbage":
                send(b"HTTP/1.0 200 OK\r\nContent-Type: audio/mpeg\r\n\r\n")
                rnd = random.Random(7)
                for _ in range(120): send(bytes(rnd.getrandbits(8) for _ in range(2000))); time.sleep(0.05)
                return
            # a good stream; /drop ends after 3 s (a dropped connection)
            metaint = 8192
            send(("ICY 200 OK\r\nContent-Type: audio/mpeg\r\nicy-name: %s\r\nicy-br: 128\r\nicy-metaint: %d\r\n\r\n"
                  % (RADIO_NAME, metaint)).encode())
            meta = ("StreamTitle='%s';" % RADIO_TITLE).encode()
            meta += bytes(-len(meta) % 16)
            left, t0, sent = metaint, time.time(), 0
            while not getattr(srv, "closing", False):
                if path == "/drop" and time.time() - t0 > 3: return
                buf = MP3_FRAME * 4
                while buf:
                    n = min(left, len(buf))
                    send(buf[:n]); buf = buf[n:]; left -= n; sent += n
                    if left == 0: send(bytes([len(meta) // 16]) + meta); left = metaint
                ahead = sent / 16000.0 - (time.time() - t0)       # 128 kbps, a few seconds ahead at most
                if ahead > 3: time.sleep(ahead - 3)
        except OSError:
            pass

def sc_radio(run):
    sim = run.sim
    go_home(sim)
    srv = StreamServer()
    base = "http://127.0.0.1:%d" % srv.port
    log_at = os.path.getsize(sim.logpath)
    def log_since():
        sim.logf.flush()
        with open(sim.logpath, "rb") as f: f.seek(log_at); return f.read().decode("utf-8", "replace")
    def radio(): return sim.state()[0]["ui"]["radio"]
    def wait(pred, timeout):
        t = time.time()
        while time.time() - t < timeout:
            r = radio()
            if pred(r): return r
            time.sleep(0.4)
        return radio()
    def play(path, name="Shakedown"):
        nonlocal log_at
        sim.set("rain_on", 0); time.sleep(2.5)                  # the radio stops 2 s after the sound
        log_at = os.path.getsize(sim.logpath)
        sim.cmd("radio", "6|%s|%s%s" % (name, base if not path.startswith(("http", "ftp")) else "", path))
        sim.set("station", 6); sim.set("rain_kind", 5); sim.set("rain_on", 1)
    try:
        # 1. a good stream: plays, with the stream's name and song title
        play("/good")
        r = wait(lambda r: r["state"] == "playing" and r["title"], 20)
        if r["state"] != "playing": run.find("radio", "a good MP3 stream didn't start playing", json.dumps(r)[:300])
        else:
            if r["title"] != RADIO_TITLE: run.find("radio", "the stream's song title wasn't read", "title=%r" % r["title"])
            if r["station"] != "Shakedown": run.find("radio", "the station isn't shown by its preset name", "station=%r" % r["station"])
            if "radio: %s," % RADIO_NAME not in log_since(): run.find("radio", "the stream's icy-name wasn't read", log_since()[-300:])
            if r["buf_ms"] <= 0: run.find("radio", "playing, but nothing decoded in hand", json.dumps(r)[:300])
        run.check("radio: a good stream playing", "radio")
        # 2. a server that refuses the device's User-Agent (SomaFM-like): retried without one
        play("/ua")
        r = wait(lambda r: r["state"] == "playing", 20)
        if r["state"] != "playing" or "trying without a User-Agent" not in log_since():
            run.find("radio", "a 403 wasn't retried without the User-Agent", "state=%s hits=%s" % (r["state"], srv.hits.get("/ua")))
        run.check("radio: 403, then playing without a User-Agent", "radio")
        # 3. the connection drops mid-stream: it reconnects by itself
        play("/drop")
        wait(lambda r: r["state"] == "playing", 15)
        t = time.time()
        while srv.hits.get("/drop", 0) < 2 and time.time() - t < 15: time.sleep(0.5)
        r = wait(lambda r: r["state"] == "playing", 10)
        if srv.hits.get("/drop", 0) < 2: run.find("radio", "a dropped stream didn't reconnect", "connections=%s state=%s" % (srv.hits.get("/drop"), r["state"]))
        run.check("radio: a dropped stream reconnects", "radio")
        # 4-6. streams that can't play: a clear reason, no crash, no sound engine stuck
        for path, want in (("/aac", "not MP3"), ("/404", "HTTP 404"), ("http://127.0.0.1:9/", "")):
            play(path)
            r = wait(lambda r: r["err"] or r["state"] in ("failed",), 12)
            if r["state"] == "playing" or not r["err"] or (want and want not in r["err"]):
                run.find("radio", "a bad stream (%s) didn't fail with a clear reason" % path, json.dumps(r)[:300])
            run.check("radio: bad stream %s" % path, "radio")
        # 7. noise sent as audio/mpeg: the decoder hunts for frames; nothing breaks
        play("/garbage"); time.sleep(6)
        run.check("radio: random bytes as MP3", "radio")
        # 8. off: the stream stops
        sim.set("rain_on", 0)
        r = wait(lambda r: r["state"] == "off", 8)
        if r["state"] != "off": run.find("radio", "the radio didn't stop when the sound was turned off", json.dumps(r)[:200])
        # 9. a URL the firmware refuses to save
        sim.cmd("radio", "6|Bad|ftp://example.com/x"); time.sleep(0.5)
        if "preset 6 rejected" not in log_since(): run.find("radio", "a non-http station URL was accepted", log_since()[-300:])
        run.check("radio: off, and a bad URL refused", "radio")
        run.notes.append("radio: local stream server; connections %s" % json.dumps(srv.hits))
    finally:
        srv.closing = True; srv.shutdown(); srv.server_close()
        sim.set("rain_on", 0)

# ---------------------------------------------------------------------------------------------
# https: the simulator's TLS is the device's mbedTLS with its config and trusted
# roots. A local https server with a certificate of its own must be refused; the
# real services (SomaFM, GitHub) must work. Those need the internet: offline,
# they are skipped with a note, not reported.
def online(host):
    try: socket.getaddrinfo(host, 443); return True
    except OSError: return False

def sc_https(run):
    sim = run.sim
    go_home(sim)
    log_at = [os.path.getsize(sim.logpath)]
    def log_since():
        sim.logf.flush()
        with open(sim.logpath, "rb") as f: f.seek(log_at[0]); return f.read().decode("utf-8", "replace")
    def radio(): return sim.state()[0]["ui"]["radio"]
    def play(url):
        sim.set("rain_on", 0); time.sleep(2.5)
        log_at[0] = os.path.getsize(sim.logpath)
        sim.cmd("radio", "6|Shakedown|" + url); sim.set("station", 6); sim.set("rain_kind", 5); sim.set("rain_on", 1)
    # 1. a server with a certificate the device doesn't trust: no handshake, no sound, a clear log line
    if shutil.which("openssl"):
        d = os.path.join(sim.out, "tls"); os.makedirs(d, exist_ok=True)
        key, crt = os.path.join(d, "key.pem"), os.path.join(d, "crt.pem")
        subprocess.run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes",
                        "-keyout", key, "-out", crt, "-days", "2", "-subj", "/CN=127.0.0.1"], check=True, capture_output=True)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(crt, key)
        srv = StreamServer(tls=ctx)
        try:
            play("https://127.0.0.1:%d/good" % srv.port); time.sleep(8)
            r, log = radio(), log_since()
            if r["state"] == "playing": run.find("https", "a stream with an untrusted certificate played", json.dumps(r)[:300])
            if "sim: tls: handshake failed" not in log: run.find("https", "no TLS handshake failure for an untrusted certificate", log[-400:])
            elif "trusted CA" not in log:
                run.notes.append("https: untrusted certificate refused, but the reason wasn't 'not trusted': " +
                                 [l for l in log.splitlines() if "handshake failed" in l][0])
            run.check("https: an untrusted certificate is refused", "https")
        finally:
            srv.closing = True; srv.shutdown(); srv.server_close(); sim.set("rain_on", 0)
    else:
        run.notes.append("https: openssl not found, the untrusted-certificate check was skipped")
    # 2. the real services over the device's TLS
    if not online("api.somafm.com"):
        run.notes.append("https: offline, SomaFM and GitHub checks skipped"); return
    log_at[0] = os.path.getsize(sim.logpath)
    sim.cmd("somafm", "1"); time.sleep(10)
    r = radio()
    if not r["soma_live"]: run.find("https", "SomaFM's channel list (https) didn't load", log_since()[-400:])
    play("https://ice1.somafm.com/groovesalad-128-mp3")
    t = time.time()
    while time.time() - t < 25 and radio()["state"] != "playing": time.sleep(0.5)
    r = radio()
    if r["state"] != "playing": run.find("https", "an https radio stream didn't play", json.dumps(r)[:300] + "\n" + log_since()[-400:])
    run.check("https: SomaFM list and an https stream", "https")
    sim.set("rain_on", 0); time.sleep(2.5)
    log_at[0] = os.path.getsize(sim.logpath)
    sim.cmd("otacheck", "1"); time.sleep(12)
    log = log_since()
    if not re.search(r"ota: \d+ release\(s\) on GitHub", log): run.find("https", "the GitHub release list (https) didn't load", log[-400:])
    run.check("https: the GitHub release list", "https")
    run.notes.append("https: " + "; ".join(sorted(set(l.split("sim: tls: ")[1] for l in log_since().splitlines() if "sim: tls: TLS" in l))) )

SCENARIOS = [("boot", sc_boot), ("wizard", sc_wizard), ("keys", sc_keys), ("alarm", sc_alarm), ("sleep", sc_sleep),
             ("night", sc_night), ("radio", sc_radio), ("https", sc_https), ("web", sc_web), ("menus", sc_menus), ("settings", sc_settings), ("reboot", sc_reboot),
             ("wifi", sc_wifi_setup), ("reset", sc_reset)]

# ---------------------------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(ROOT, "sim", "build-san", "shakedown"))
    ap.add_argument("--only", default="")
    ap.add_argument("--port", type=int, default=8091)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    for f in os.listdir(a.out):
        if f.endswith(".png") or f in ("sim.log", "report.md"): os.remove(os.path.join(a.out, f))
    sim = Sim(a.out, a.port)
    run = Run(sim, a.out)
    only = set(x for x in a.only.split(",") if x)
    sim.start(reset=True)
    t0 = time.time()
    try:
        for name, fn in SCENARIOS:
            if only and name not in only and name != "boot": continue
            print("== %s" % name, flush=True)
            ts = time.time()
            try: fn(run)
            except Exception as e:
                run.find(name, "the script failed: %s" % e, traceback.format_exc()[-800:], key=("script", name))
                if not sim.alive(): sim.start(reset=False)
            print("   %.0f s" % (time.time() - ts), flush=True)
    finally:
        sim.stop()
    with open(os.path.join(a.out, "report.md"), "w") as f:
        f.write("# Shakedown\n\n%d steps in %.0f s; %d findings; largest /api/state %d bytes (device buffer %d)\n\n"
                % (run.steps, time.time() - t0, len(run.findings), run.max_json, DEVICE_JSON_BUF))
        for x in run.findings:
            f.write("## [%s] %s\n\n```\n%s\n```\n%s\n\n" % (x["area"], x["title"], x["detail"], ("![](%s)" % x["pic"]) if x["pic"] else ""))
        f.write("## Notes\n\n" + "\n".join("- " + n for n in run.notes) + "\n")
    print("\n%d findings, %d steps; report: %s" % (len(run.findings), run.steps, os.path.join(a.out, "report.md")))

if __name__ == "__main__":
    main()
