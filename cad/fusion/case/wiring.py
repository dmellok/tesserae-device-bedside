# Wiring: every cable in the device, in the colours of docs/power-amp-board.html. Builds the top-level component "Wiring".
# The wires are soldered straight into the protoboard's bottom-edge holes (no J1/J3 headers), and plug onto the splitter's
# right-angle header with single Dupont housings. As built, the stack is the other way up from how it was first drawn: header pins 1 and 2 are at the bottom end, and the Pico has its USB-C at the bottom and its Qw/ST socket at the top. ASSUMED: odd pins in the row farther from the HAT;
# USB-C breakout VBUS and GND on the two pads nearest the board; the switch uses its centre and top terminals.
S, C = math.sin(math.radians(15)), math.cos(math.radians(15))
def F(u, v, w): return (u, w*C + v*S, 12 + v*C - w*S)
RED, BLK, GRN, YEL, BLU, WHT, GRY = (200, 40, 30), (26, 26, 28), (40, 150, 70), (235, 200, 40), (40, 90, 200), (236, 236, 230), (140, 142, 146)
def fbox(u0, v0, w0, u1, v1, w1):
    c = F((u0 + u1)/2, (v0 + v1)/2, (w0 + w1)/2)
    obb = adsk.core.OrientedBoundingBox3D.create(P(*c), adsk.core.Vector3D.create(1, 0, 0), adsk.core.Vector3D.create(0, S, C), abs(u1 - u0)*MM, abs(v1 - v0)*MM, abs(w1 - w0)*MM)
    return tbm.createBox(obb)
def spline(pts, n=5):
    out = []; q = [pts[0]] + list(pts) + [pts[-1]]
    for i in range(1, len(q) - 2):
        p0, p1, p2, p3 = q[i - 1], q[i], q[i + 1], q[i + 2]
        for k in range(n):
            t = k/n
            out.append(tuple(0.5*((2*p1[j]) + (-p0[j] + p2[j])*t + (2*p0[j] - 5*p1[j] + 4*p2[j] - p3[j])*t*t + (-p0[j] + 3*p1[j] - 3*p2[j] + p3[j])*t*t*t) for j in range(3)))
    out.append(tuple(pts[-1])); return out
LEN = {}
def tube(name, pts, r):
    s = spline(pts); body = None; total = 0
    for a, b in zip(s, s[1:]):
        d = [b[j] - a[j] for j in range(3)]; L = math.sqrt(sum(x*x for x in d))
        if L < 0.05: continue
        total += L; e = 0.2*r/L
        c = cyl(tuple(a[j] - d[j]*e for j in range(3)), tuple(b[j] + d[j]*e for j in range(3)), r)
        if body is None: body = c
        else:
            try: tbm.booleanOperation(body, c, adsk.fusion.BooleanTypes.UnionBooleanType)
            except RuntimeError: pass
    LEN[name] = round(total)
    return body
def ribbon(name, pts, spreads, cols, r=0.5, pitch=1.0):
    out = []
    for i, col in enumerate(cols):
        k = (i - (len(cols) - 1)/2)*pitch
        out.append(('%s %d' % (name, i + 1), tube(name, [tuple(p[j] + d[j]*k for j in range(3)) for p, d in zip(pts, spreads)], r), col))
    return out
def hole(letter): return 73.92 + 2.54*(ord(letter) - ord('E'))
def pin(n): return 20.57 + 2.54*((n - 1)//2), (44.0 if n % 2 else 46.5)
def run(_context: str):
    case = doc_named('Bedside case'); case.activate()
    design = adsk.fusion.Design.cast(app.activeProduct); rc = design.rootComponent
    for o in [o for o in rc.occurrences if o.name.startswith('Wiring')]: o.deleteMe()
    st = rc.occurrences.itemByName('Display driver stack:1').component.occurrences.itemByName('Dupont harness (envelope):1')
    if st: st.deleteMe()
    pab = rc.occurrences.itemByName('Power and amp board:1').component
    for n in ('J1 header 1x7:1', 'J3 header 1x7:1'):
        o = pab.occurrences.itemByName(n)
        if o: o.deleteMe()
    items = []; R = 0.65
    # --- protoboard to the splitter's 40-pin header ---
    GP = [('W', 2, RED, '5V'), ('V', 6, BLK, 'GND'), ('U', 36, WHT, 'SD'), ('S', 40, BLU, 'DIN'), ('R', 12, YEL, 'BCLK'), ('Q', 35, GRN, 'LRC'),
          ('K', 22, BLU, 'low battery'), ('H', 37, GRY, 'battery sense'), ('G', 13, GRN, 'mains sense')]
    # One loom: the nine wires lie three by three along the board's edge, turn down the gap beside the board, and peel off to their pins. Cable ties hold it.
    for i, (h, n, col, what) in enumerate(GP):
        uh = hole(h); vp, wp = pin(n); wl = 26.0 + 1.35*(i % 3); ul = 129.3 + 1.35*(i//3); vt = 60.0 + 1.35*(i//3)
        pts = [F(uh, 58.6, 17.6), F(uh, 58.6, 22.5), F(uh + 3, vt, wl), F(min(uh + 12, 123), vt, wl)]
        if abs(vp - vt) > 6:
            pts += [F(126.0, vt, wl)]
            if vp < vt - 14: pts += [F(ul, vt - 7.0, wl)]                                                    # round the corner, then straight down
            pts += [F(ul, vp + (5.0 if vp < vt else -2.0), wl)]
        else: pts += [F(127.5, (vt + vp)/2, wl)]
        k = 0.8*(wp - 45.25)                                                                               # the two rows' wires come in one above the other, 2 mm apart
        pts += [F(132.4, vp, 31.8 + k), F(133.6 + k/2, vp, 38.5 + k), F(135.2, vp, wp), F(137.0, vp, wp)]
        items.append(('GPIO pin %d, %s' % (n, what), tube('GPIO %d' % n, pts, R), col))
    items.append(('Dupont plug, 2x20', fbox(135.0, 19.3, 42.7, 149.0, 70.1, 47.8), (24, 24, 26)))
    def tie(k, lo, hi):
        band = SUB(fbox(*lo, *hi), [fbox(*[(lo[j] + 0.4 if hi[j] - lo[j] > 3 else lo[j] - 1) for j in range(3)], *[(hi[j] - 0.4 if hi[j] - lo[j] > 3 else hi[j] + 1) for j in range(3)])])
        items.append(('GPIO cable tie %d' % k, band, (236, 236, 230)))
    tie(1, (123.4, 58.85, 24.85), (125.8, 63.85, 29.85))
    tie(2, (128.15, 46.0, 24.85), (133.15, 48.4, 29.85))
    tie(3, (128.15, 33.5, 24.85), (133.15, 35.9, 29.85))
    # --- USB-C inlet and power switch, in the plinth ---
    def down(h, k): uh = hole(h); return [F(uh, 58.6, 17.6), F(uh, 58.6, 24.5), F(uh, 57.3 - k/2, 30.6), F(uh + 1, 51.5 - k, 35.0), F(uh + 2, 46.5 - k, 40.5)]
    for h, col, xp, name in (('F', RED, 107.4, 'USB-C VBUS'), ('E', BLK, 109.94, 'USB-C GND')):
        items.append((name, tube(name, down(h, 0 if h == 'F' else 1.6) + [(xp - 14, 61, 51.5), (xp - 3, 68.5, 50.5), (xp, 72.05, 47.5), (xp, 72.05, 42.3)], R), col))
    for h, col, z, name in (('I', WHT, 50.0, 'Switch EN'), ('J', BLK, 54.7, 'Switch GND')):
        items.append((name, tube(name, down(h, 0 if h == 'I' else 1.6) + [(88.5, 58, z + 1.5), (90.0, 64, z + 0.6), (90.0, 67.0, z), (90.0, 69.5, z)], R), col))
    # --- speaker, on a twisted pair from the amp's terminal block ---
    for ue, col, tag, k, name in ((110.25, RED, (37.5, 70.9, 17.0), 0.0, 'Speaker +'), (113.75, BLK, (37.5, 65.3, 11.4), 1.5, 'Speaker -')):
        pts = [F(ue, 22.5, 27.0), F(ue, 17.0, 27.0), F(ue - 3, 11.5, 28.5 + k), (ue - 12, 36.5 + k, 10.0), (70, 40 + k, 9.5), (58, 44 + k, 9.5), (48, 56 + k, 9.5), (41.5, tag[1] - 1.5, tag[2] - 3.5), tag]
        items.append((name, tube(name, pts, R), col))
    # --- battery lead to the PowerBoost's JST socket ---
    for uj, col, z0, k, name in ((73.3, RED, 22.0, 0.0, 'Battery +'), (75.3, BLK, 26.0, 1.5, 'Battery -')):
        pts = [(119.0, 59, z0), (122.5, 59.5, z0 + 1), (124.6, 61.5 + k, 31), (124.2, 61.5 + k, 39.5), (119.5, 60 + k, 42.9), (104, 56 + k, 42.6), (86, 52 + k, 42.6), (79.5 + k, 48.5, 42.2), (77.0 + k, 45.3, 36.0),
               (uj + 1.5, 38.0, 22.0), F(uj, 7.0, 29.0), F(uj, 9.5, 26.1), F(uj, 16.0, 26.1)]
        items.append((name, tube(name, pts, 0.7), col))
    items.append(('Battery plug, JST-PH', fbox(71.3, 10.8, 23.9, 77.3, 14.4, 28.3), (236, 236, 230)))
    # --- STEMMA QT: Pico -> encoder -> keys ---
    QT = [BLK, RED, BLU, YEL]; Ux, Wx = (1, 0, 0), (0, C, -S)
    # from the Pico's socket at the top of the board: up, forward through the opening in the shell's back wall, across behind the HAT below the tips of its
    # 13-pin header, up past the end of that header and on to the encoder
    p1 = [F(177.8, 66.4, 39.5), F(177.8, 70.5, 39.0), F(180.0, 72.8, 36.5), F(184.0, 73.3, 33.8), F(189.0, 73.3, 31.9), F(198.0, 73.3, 31.6), F(206.0, 74.0, 31.6), F(207.0, 79.0, 31.6), F(207.0, 86.0, 30.0),
          F(219.0, 108.0, 24.0), F(228.5, 126.0, 18.7), F(228.5, 131.0, 18.7), F(228.5, 136.0, 18.7)]
    s1 = [Ux]*9 + [(0.7, 0.7*C, -0.7*S), Wx, Wx, Wx]
    items += ribbon('STEMMA QT, Pico to encoder', p1, s1, QT)
    items.append(('STEMMA QT plug, Pico', fbox(174.8, 65.1, 37.6, 180.8, 69.1, 41.5), (236, 236, 230)))
    items.append(('STEMMA QT plug, encoder in', fbox(227.2, 134.0, 15.9, 229.8, 137.4, 21.5), (236, 236, 230)))
    # the back post at (228, 178) leaves 2 mm behind the key module's socket, so this cable turns down as soon as it leaves the plug
    p2 = [F(228.5, 164.0, 18.7), F(228.5, 167.0, 18.5), F(226.6, 170.3, 17.6), F(224.0, 172.6, 16.2), F(223.4, 175.2, 15.0), F(222.2, 176.5, 14.6), F(220.5, 176.5, 14.6)]
    items += ribbon('STEMMA QT, encoder to keys', p2, [Wx]*7, QT)
    items.append(('STEMMA QT plug, encoder out', fbox(227.2, 162.6, 15.9, 229.8, 166.0, 21.5), (236, 236, 230)))
    items.append(('STEMMA QT plug, keys', fbox(218.9, 175.2, 11.8, 222.3, 177.8, 17.4), (236, 236, 230)))
    occ = rc.occurrences.addNewComponent(adsk.core.Matrix3D.create()); occ.component.name = 'Wiring'
    add_bodies(occ.component, [it for it in items if it[1] is not None], design)
    print('bodies', len(items), 'missing', [it[0] for it in items if it[1] is None])
    print('lengths mm', LEN)
    print('save', case.save('Wiring: every cable modelled'))
