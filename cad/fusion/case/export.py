import json, array
OUT = '/Users/kayden/Developer/PlatformIO/tesserae-device-bedside/cad/viewer/data/'
S, C = math.sin(math.radians(15)), math.cos(math.radians(15))
W = (0, C, -S); V = (0, S, C)
def mul(v, k): return [round(x*k, 3) for x in v]
Z3 = [0, 0, 0]
ONLY = None
KEPT = {'mk1': 'MK I ', 'mk2': 'MK II ', 'mk21': 'MK II.I '}          # earlier case versions, parked in the design as hidden components with these prefixes
def kept(top):
    for k, pre in KEPT.items():
        if top.startswith(pre): return k, top[len(pre):]
    return None, top
SCREWS = {'Bezel': ('Bezel screws, M3 x 8', Z3, mul(W, 26)), 'Shell': ('Base screws, M3 x 12', [0, 0, -45], Z3), 'Back': ('Back screws, M3 x 8', [0, 205, 0], Z3), 'Frame': ('Frame screws, M3 x 8', [0, 0, -45], Z3), 'Corner': ('Shell corner screws, M3 x 8', [0, 0, -45], Z3),
          'HAT': ('Driver HAT screws, M2.5 x 8', Z3, mul(W, 12)), 'Board': ('Power board screws, M2 x 6', Z3, mul(W, 9)), 'Adapter': ('Flex adapter screws, M2.5 x 6', Z3, [0, 36, 0]),
          'Key': ('Key module screws, M2.5 x 8', Z3, mul(V, 9)), 'Speaker': ('Speaker screws and nuts, M4 x 8', Z3, [28, 0, 0]), 'USB': ('USB-C inlet screws, M2.5 x 8', Z3, [0, 70, 42])}
def classify(path, bname=''):
    top = kept(path[0])[1]; sub = path[1] if len(path) > 1 else ''
    if top.startswith('Front bezel'): return 'Front bezel', mul(W, -110), Z3
    if top.startswith('Floppy slot insert'): return 'Floppy slot insert', mul(W, -110), mul(W, 14)
    if top.startswith('Accent stripe'): return 'Accent stripe', mul(W, -110), mul(W, -12)
    if top.startswith('E Ink'): return 'E Ink ED103TC2 panel', mul(W, -55), Z3
    if top.startswith('Mid chassis'): return 'Mid chassis and base', Z3, Z3
    if top.startswith('Base tray'): return 'Base tray', Z3, Z3
    if top.startswith('Frame plate'): return 'Frame plate', Z3, Z3
    if top.startswith('Rear shell'): return 'Rear shell', [0, 170, 0], Z3
    if top.startswith('Plinth'): return 'Plinth', [0, 250, 0], Z3
    if top.startswith('Screws'): return SCREWS[bname.split()[0]]
    if top.startswith('Access cover'): return 'Access cover', [0, 215, 30], Z3
    if top.startswith('Power switch'): return 'Power switch', Z3, [0, 60, 0]
    if top.startswith('Rubber feet'): return 'Rubber feet', [0, 0, -25], Z3
    if top.startswith('Wiring'):
        for pre, label in (('GPIO', 'Wires to the GPIO header'), ('Dupont', 'Wires to the GPIO header'), ('USB', 'USB-C inlet wires'), ('Switch', 'Power switch wires'),
                           ('Speaker', 'Speaker leads'), ('Battery', 'Battery lead'), ('STEMMA', 'STEMMA QT cables')):
            if bname.startswith(pre): return label, Z3, Z3
    if top.startswith('Keycaps'): return 'Keycaps', mul(V, 35), mul(V, 14)
    if top.startswith('Key module'):
        return ('Cherry MX Brown switches', Z3, mul(V, 14)) if sub.startswith('Cherry') else ('Adafruit NeoKey 1x4 QT', Z3, Z3)
    if top.startswith('Knob'): return 'Knob', [40, 0, 0], [15, 0, 0]
    if top.startswith('5880'): return 'Adafruit I2C rotary encoder', Z3, [15, 0, 0]
    if top.startswith('Display driver stack'):
        if sub.startswith('Waveshare'): return 'Waveshare IT8951 driver HAT', Z3, Z3
        if sub.startswith('GPIO splitter'): return 'GPIO splitter', Z3, [0, 28, 0]
        if sub.startswith('Pimoroni'): return 'Pimoroni Pico Plus 2 W', Z3, [0, 50, 0]
        if sub.startswith('Pico sockets'): return 'Pico sockets and headers', Z3, [0, 68, 0]
        return 'Hard Stuff Pico to Pi HAT', Z3, [0, 95, 0]
    if top.startswith('Power and amp board'):
        if sub.startswith('2465'): return 'Adafruit PowerBoost 1000C', Z3, [0, 34, 0]
        if sub.startswith('3006'): return 'Adafruit MAX98357A amplifier', Z3, [0, 34, 0]
        if sub.startswith('Protoboard') or sub.startswith('Bus wires'): return 'Power and amp protoboard', Z3, Z3
        return 'Headers, resistors, capacitors and diode', Z3, [0, 14, 0]
    if top.startswith('LiPo'): return 'LiPo battery, 4400 mAh', Z3, [0, 70, 0]
    if top.startswith('Visaton'): return 'Visaton FRS 5 X speaker', Z3, [-45, 0, 0]
    if top.startswith('4090'): return 'USB-C power inlet', Z3, [0, 70, 22]
    if top.startswith('Waveshare 10.3'): return 'E-paper flex adapter', Z3, [0, 26, 0]
    return top.split(':')[0], Z3, Z3
def colour(body):
    a = body.appearance
    name = a.name if a else ''
    op = 0.28 if ('Clear' in name or 'Glass' in name) else 1.0
    rgb = None
    if a:
        for i in range(a.appearanceProperties.count):
            p = a.appearanceProperties.item(i)
            if isinstance(p, adsk.core.ColorProperty) and p.value is not None:
                rgb = (p.value.red, p.value.green, p.value.blue); break
    return (rgb or (60, 62, 66)), op, name
def run(_context: str):
    case = doc_named('Bedside case'); case.activate()
    design = adsk.fusion.Design.cast(app.activeProduct); rc = design.rootComponent
    groups = {}; nb = 0; nomesh = 0; names = {}
    for occ in rc.allOccurrences:
        path = occ.fullPathName.split('+'); mk1 = kept(path[0])[0]
        if mk1 != ONLY or (not mk1 and not occ.isVisible): continue
        for body in occ.bRepBodies:
            if not mk1 and not body.isVisible: continue
            try: m = body.meshManager.displayMeshes.bestMesh
            except RuntimeError: m = None          # hidden bodies have no display mesh
            if path[0].startswith('Wiring') and not body.name.startswith(('Dupont', 'Battery plug', 'STEMMA QT plug', 'GPIO cable tie')):
                calc = body.meshManager.createMeshCalculator(); calc.surfaceTolerance = 0.006; m = calc.calculate()      # wires are thin: a coarse mesh keeps the download small
            if m is None or m.triangleCount == 0:
                calc = body.meshManager.createMeshCalculator(); calc.setQuality(adsk.fusion.TriangleMeshQualityOptions.NormalQualityTriangleMesh); m = calc.calculate()
            if m is None or m.triangleCount == 0: nomesh += 1; continue
            label, e1, e2 = classify(path, body.name)
            rgb, op, an = colour(body); names[an] = names.get(an, 0) + 1
            key = (label, rgb, op, tuple(e1), tuple(e2))
            g = groups.setdefault(key, {'e1': e1, 'e2': e2, 'pos': array.array('f'), 'nrm': array.array('f'), 'idx': array.array('I')})
            base = len(g['pos'])//3
            g['pos'].extend(m.nodeCoordinatesAsFloat); g['nrm'].extend(m.normalVectorsAsFloat)
            g['idx'].extend([i + base for i in m.nodeIndices]); nb += 1
    parts = []; blob = bytearray()
    for (label, rgb, op, _e1, _e2), g in groups.items():
        pos = array.array('f', [x*10 for x in g['pos']])
        e = {'name': label, 'color': list(rgb), 'opacity': op, 'e1': g['e1'], 'e2': g['e2'], 'verts': len(pos)//3, 'tris': len(g['idx'])//3}
        e['pos'] = len(blob); blob += pos.tobytes()
        e['nrm'] = len(blob); blob += g['nrm'].tobytes()
        e['idx'] = len(blob); blob += g['idx'].tobytes()
        parts.append(e)
    def Fp(u, v, w): return [round(u, 3), round(w*C + v*S, 3), round(12 + v*C - w*S, 3)]
    u0, v0, wq = 13.65 + 3.52, 13 + 13.62, 3.40
    screen = {'image': 'data/screen.png', 'corners': [Fp(u0, v0, wq), Fp(u0 + 209.66, v0, wq), Fp(u0 + 209.66, v0 + 157.25, wq), Fp(u0, v0 + 157.25, wq)], 'e1': mul(W, -55), 'e2': Z3}
    stem = ONLY or 'bedside'
    open(OUT + stem + '.bin', 'wb').write(blob)
    json.dump({'units': 'mm', 'up': 'z', 'parts': parts, 'screen': None if ONLY else screen}, open(OUT + stem + '.json', 'w'))
    print('bodies', nb, 'no mesh', nomesh, 'groups', len(parts), 'verts', sum(p['verts'] for p in parts), 'tris', sum(p['tris'] for p in parts), 'bytes', len(blob))
    print(sorted(names.items(), key=lambda kv: -kv[1])[:25])
    print(sorted({p['name'] for p in parts}))
