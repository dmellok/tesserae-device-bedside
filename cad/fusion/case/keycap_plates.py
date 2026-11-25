#!/usr/bin/env python3
"""Lays every keycap out on one plate, in a grid, as a single STL for each filament colour.

    python3 keycap_plates.py [preview.png]

Unlike the other scripts in this folder this one runs in plain Python, not in Fusion. It needs fonttools, manifold3d and numpy (and pillow for
the optional preview picture).

The caps with dots, a bar or nothing are the files keycap_export.py wrote to ../../print, placed as they are. The caps with a picture are
keycap-plain.stl with a Phosphor Bold icon sunk into the top face, to the same depth as the dots. The icons come from ../../../tools/Phosphor-Bold.ttf,
the font the firmware draws its own icons from, so a key can carry the same picture the screen shows for it.

Every cap lies top face down, as it prints, so in the slicer the plate is seen from the caps' open undersides. The grid is written so that it
reads left to right, top row first, in the slicer's view from above. The icons are mirrored in the file, which is what makes them read
the right way round on the finished cap, with the top of the picture towards the back of the device.
"""
import os, struct, sys
import numpy as np
from fontTools.pens.basePen import BasePen
from fontTools.ttLib import TTFont
from manifold3d import CrossSection, FillRule, Manifold, Mesh

HERE = os.path.dirname(os.path.abspath(__file__))
PRINT = os.path.normpath(os.path.join(HERE, '..', '..', 'print'))
FONT = os.path.normpath(os.path.join(HERE, '..', '..', '..', 'tools', 'Phosphor-Bold.ttf'))

DEPTH = 0.5          # how deep an icon is sunk: KC_PIT in case_mk3.py, the depth of the dots and the bar
EM = 14.0            # the size of Phosphor's 256 unit drawing square on the cap, in mm. Its bold stroke is 24 units, so the lines are 1.3 mm wide
PITCH = (21.5, 23.1) # grid pitch: a cap is 17.5 x 19.1 mm, so there is 4 mm between neighbours
STEPS = 10           # straight pieces each curve is cut into

# The grid, row by row. A name that is a file in ../../print is used as it is; any other is a Phosphor icon (name, codepoint in Phosphor 2.1.2).
ROWS = [
    ['keycap-plain', 'keycap-1-dot', 'keycap-2-dots', 'keycap-3-dots', 'keycap-4-dots', 'keycap-bar'],                                             # every mark
    [('caret-up', 0xE13C), ('caret-down', 0xE136), ('check', 0xE182), ('arrow-u-up-left', 0xE08A), ('list', 0xE2F0), ('house', 0xE2C2)],           # in a menu: up, down, select, back, menu, home
    [('minus', 0xE32A), ('plus', 0xE3D4), ('play-pause', 0xE8BE), ('speaker-high', 0xE44A), ('speaker-slash', 0xE45A), ('music-notes', 0xE340)],   # on a slider, and sound
    [('lightbulb', 0xE2DC), ('cloud-rain', 0xE1B4), ('snowflake', 0xE5AA), ('fan', 0xE9F2), ('thermometer', 0xE5C6), ('power', 0xE3DA)],           # the room: lights, rain, A/C
    [('moon', 0xE330), ('sun', 0xE472), ('alarm', 0xE006), ('bell', 0xE0CE), ('timer', 0xE492), ('gear', 0xE270)],                                 # night, day, alarms, settings
]
SETS = ['keycaps-set-grey', 'keycaps-set-orange']      # the same plate twice, one file for each filament

def read_stl(path):
    d = open(path, 'rb').read(); n = struct.unpack('<I', d[80:84])[0]
    a = np.frombuffer(d, dtype=np.dtype([('n', '<f4', 3), ('v', '<f4', (3, 3)), ('a', '<u2')]), count=n, offset=84)
    pts = np.round(a['v'].reshape(-1, 3).astype(np.float64), 4)
    verts, idx = np.unique(pts, axis=0, return_inverse=True)      # an STL repeats every corner; give the triangles shared ones again
    m = Manifold(Mesh(vert_properties=verts.astype(np.float32), tri_verts=idx.reshape(-1, 3).astype(np.uint32)))
    assert m.status().name == 'NoError' and m.volume() > 0, (path, m.status())
    return m

def write_stl(path, m):
    mesh = m.to_mesh(); v = np.asarray(mesh.vert_properties)[:, :3]; t = np.asarray(mesh.tri_verts)
    tri = v[t]; nrm = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0]); L = np.linalg.norm(nrm, axis=1); nrm[L > 0] /= L[L > 0][:, None]
    out = np.zeros(len(tri), dtype=np.dtype([('n', '<f4', 3), ('v', '<f4', (3, 3)), ('a', '<u2')])); out['n'] = nrm; out['v'] = tri
    with open(path, 'wb') as f:
        f.write(b'Bedside Dashboard keycaps'.ljust(80, b' ')); f.write(struct.pack('<I', len(tri))); f.write(out.tobytes())

class Outline(BasePen):
    # collects a glyph's contours as closed runs of straight pieces
    def __init__(self, gs): super().__init__(gs); self.contours = []
    def _moveTo(self, p): self.contours.append([p])
    def _lineTo(self, p): self.contours[-1].append(p)
    def _curveToOne(self, b, c, d):
        a = self.contours[-1][-1]
        for i in range(1, STEPS + 1):
            t = i/STEPS; s = 1 - t
            self.contours[-1].append(tuple(s*s*s*a[k] + 3*s*s*t*b[k] + 3*s*t*t*c[k] + t*t*t*d[k] for k in (0, 1)))

def icon(font, code):
    # the icon as a flat shape in mm, centred on the origin and mirrored left to right
    gs = font.getGlyphSet(); pen = Outline(gs); gs[font.getBestCmap()[code]].draw(pen)
    k = EM/font['head'].unitsPerEm
    polys = [np.array(c, dtype=np.float64)*k for c in pen.contours if len(c) > 2]
    allp = np.vstack(polys); mid = (allp.min(axis=0) + allp.max(axis=0))/2
    polys = [(p - mid)*np.array([-1.0, 1.0]) for p in polys]
    return CrossSection(polys, FillRule.NonZero), allp.max(axis=0) - allp.min(axis=0)

def build():
    font = TTFont(FONT); plain = read_stl(os.path.join(PRINT, 'keycap-plain.stl'))
    lo, hi = plain.bounding_box()[:3], plain.bounding_box()[3:]
    cx, cy, z0 = (lo[0] + hi[0])/2, (lo[1] + hi[1])/2, lo[2]      # the middle of the top face, which is the face on the bed
    caps, names = [], []
    for r, row in enumerate(ROWS):
        for c, item in enumerate(row):
            if isinstance(item, str): cap, name = read_stl(os.path.join(PRINT, item + '.stl')), item.replace('keycap-', '')
            else:
                shape, size = icon(font, item[1]); name = item[0]
                cap = plain - shape.extrude(DEPTH + 0.1).translate((cx, cy, z0 - 0.1))
                assert len(cap.decompose()) == 1, name
                print('%-16s %4.1f x %4.1f mm, %5.1f mm3 sunk' % (name, size[0], size[1], plain.volume() - cap.volume()))
            # the slicer shows the plate from above, so the first row goes at the back and each row runs left to right
            caps.append(cap.translate((c*PITCH[0] - cx, -r*PITCH[1] - cy, -z0))); names.append(name)
    return Manifold.compose(caps), caps, names

def preview(path, caps, names):
    # the tops of the caps as they will be seen on the keys: the bed face from below, so left and right are swapped back
    from PIL import Image, ImageDraw
    S = 14; W, H = int(len(ROWS[0])*PITCH[0]*S), int(len(ROWS)*PITCH[1]*S)
    im = Image.new('RGB', (W, H), (245, 243, 238)); d = ImageDraw.Draw(im)
    def px(p): return ((PITCH[0]*(len(ROWS[0]) - 0.5) - p[0])*S, (PITCH[1]/2 - p[1])*S)
    def area(p): return float(np.sum(p[:, 0]*np.roll(p[:, 1], -1) - np.roll(p[:, 0], -1)*p[:, 1]))
    for cap in caps:
        # a cut halfway down the engraving: plastic in grey, whatever is sunk into it dark. Biggest outlines first, so islands land on top of their holes
        for poly in sorted(cap.slice(DEPTH/2).to_polygons(), key=lambda p: -abs(area(p))):
            d.polygon([px(q) for q in poly], fill=(176, 176, 176) if area(poly) > 0 else (50, 50, 54))
    im.save(path)

if __name__ == '__main__':
    plate, caps, names = build()
    lo, hi = plate.bounding_box()[:3], plate.bounding_box()[3:]
    print('%d caps, %.1f x %.1f x %.1f mm, %.1f cm3' % (len(caps), hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], plate.volume()/1000))
    for name in SETS:
        write_stl(os.path.join(PRINT, name + '.stl'), plate); print('wrote', name + '.stl')
    if len(sys.argv) > 1: preview(sys.argv[1], caps, names)
