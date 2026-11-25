BEIGE = (222, 211, 186)
YR, WF, WB, T = 84.0, -3.0, 6.0, 2.5       # rear wall outer Y (the same depth as MK II), bezel front plane, bezel back plane, wall
BX = -12.0                                 # the battery sits this far towards the speaker, to leave room for the Dupont plug and its wires
PI = 9.0                                   # the plinth and base tray stop this far short of the frame on each side, so the frame overhangs them and reads as floating
PX = -6.1                                  # the USB-C and switch panel sits this far towards the speaker, so its right edge lines up with the left edge of the hump
CS = [(6.0, 12.0), (238.0, 12.0)]          # screws up through the rear shell's floor, where it overhangs the tray, into the frame plate's bottom corners
G, CH = 0.25, 7.0                          # gap left between printed parts that slide together; chamfer around the back of the frame
def obox(u0, v0, w0, u1, v1, w1):
    c = F((u0+u1)/2, (v0+v1)/2, (w0+w1)/2)
    return tbm.createBox(adsk.core.OrientedBoundingBox3D.create(P(*c), adsk.core.Vector3D.create(*U_), adsk.core.Vector3D.create(*V_), abs(u1-u0)*MM, abs(v1-v0)*MM, abs(w1-w0)*MM))
def wcyl(u, v, w0, w1, r): return cyl(F(u, v, w0), F(u, v, w1), r)
BZ = [(6.5, 20), (6.5, 100), (6.5, 180), (237.5, 20), (237.5, 100), (237.5, 180)]   # bezel screws (u, v)
POSTS = [(60.0, YR - 8), (224.5, YR - 8)]                                          # plinth posts on the tray, against the rear wall
FS = [(44, 12.4), (100, 12.4), (144, 12.4), (200, 12.4)]                           # frame plate to base tray screws (X, Y)                            # shell screws (X, Y)
GUS = [(28.75, 30.0, 36.0), (49.75, 30.0, 36.0), (134.0, 36.0, 46.0), (149.75, 36.0, 46.0), (219.75, 36.0, 46.0)]   # gussets behind the frame plate (X, how far back the heel reaches, how far up the plate), in lanes clear of the speaker, the boards and the wiring
HS = [(x, y - 4.2) for x, y, z in GUS]                                             # a second row of tray screws, one under the heel of each gusset
FEET = [(PI + 8, 12), (244 - PI - 8, 12), (PI + 8, YR - 8), (244 - PI - 22, YR - 8)]      # stick-on rubber feet, 12 mm across and 2 mm thick, as far apart as the base allows; the back right one sits inboard of the plinth screw beside it
CB, CBR, SEAT = 3.2, 3.1, 2.0                # every screw head on the outside sits in a recess this deep and this wide (an M3 socket cap head is 3 mm tall and 5.5 mm across), with this much plastic left under it
BOSS = CB + SEAT - T                         # so the 2.5 mm walls are thickened by this much on the inside around each one
KV, KW = 150.0, 18.7                                                               # knob centre in the frame
GLASS_W = 3.65                                # where the front of the glass sits. The glass is 0.78 mm, so its back is 1.62 mm from the plate, and the 2 mm foam behind it is squeezed by 0.38 mm.
def bezel():
    b = INTER(obox(0, -40, WF, 244, 196, WB), box(-10, -60, 10, 260, 200, 300))
    cuts = [obox(16.42, 25.87, -10, 227.58, 184.62, 10),            # window
            obox(13.25, 21.0, GLASS_W - 0.05, 230.75, 187.8, 7),      # glass pocket, open to the back: its floor is the lip the glass is pressed against
            obox(13.25, 12.6, 3.0, 230.75, 21.0, 7),                 # deeper along the bottom edge, for the driver chips and the flex bonded to the front of the glass there
            obox(50, 10.2, 3.0, 195, 13, 7)]                         # flex wrap relief: the groove the fold of the panel's wide flex sits in, below the glass's bottom edge. From bezel R2 it is 1 mm taller, reaching 2.4 mm below the glass and not 1.4, so the fold has more room to turn in
    cuts += [wcyl(u, v, 0.5, 6.5, R3) for u, v in BZ] + [lead(F(u, v, WB), nW_, R3) for u, v in BZ]      # holes from the back: the M3 screws cut their own thread in the plastic
    return SUB(b, cuts)
def vadd(a, b, k=1.0): return tuple(a[i] + k*b[i] for i in range(3))
BACK = [(16, 178, 4.0), (228, 178, 4.0), (16, 105, 4.0), (228, 105, 4.0), (11.2, 72, 3.2), (232.8, 72, 3.2)]   # rear shell screws through the back (u, v, post radius); the low pair hold the overhanging bottom corners
def NUT_RELIEF(): return cyl((PI + T + 0.1, 27.79, 12.09), (15, 27.79, 12.09), 4.8)     # room for the lower speaker nut beside the front-left post
def TIP_RELIEF(): return [cyl((10.2, y, z), (15, y, z), 2.6) for y, z in ((70.21, 54.51), (27.79, 12.09))]     # room for the ends of the speaker's M4 x 10 screws past their nuts
R3, R25, R2 = 1.3, 1.075, 0.85                # holes the M3, M2.5 and M2 screws cut their own thread in: 2.6, 2.15 and 1.7 mm, about 85% of the screw. From test prints: an M2 would not start in 1.6 mm with no chamfer, was loose in 1.9 mm, and an M2.5 was tight but held in 1.9 mm.
def lead(p, d, r): return cone(vadd(p, d, -0.05), r + 0.55, vadd(p, d, 0.5), r)      # a 0.5 mm chamfer at a hole's mouth p, where d points into the hole, so the screw finds it and starts square
nW_ = tuple(-x for x in W_)
PBU, PBV = (60.25, 125.75), (14.0, 60.0)      # power and amp board posts: 65.5 x 46 mm between centres. From a photo of the 70.18 mm long board with the perspective taken out; the holes take an M2 screw.
# The e-paper adapter (the small board that joins the panel's flex to the ribbon for the HAT), measured from the real one: 42.1 mm long by caliper,
# and from a photo scaled to that, 20.5 mm wide with 3.1 mm holes 35 x 14 mm apart. It sits long side across, its lower connector just above the slot
# the panel's flex tail comes through. On the first printed plate (R1) it sat too high and the tail could not reach it.
# The panel's flex, from a photo of a real panel with a caliper across it: a strip 137.6 mm wide is bonded along the bottom of the glass and folds flat
# onto its back, reaching about 16.5 mm up from the glass's edge (v 12.6), with a few small capacitors on it. The tail, 20.5 mm wide, leaves the top
# of that strip at its u 191 end and runs on about 22 mm. So the tail starts near v 29, and only the tail can turn back through the plate: R2's slot
# at v 14 to 20 was behind the wide strip, which cannot pass. FSL is the slot from R3 on, around where the tail leaves the strip.
# The photo is good to a couple of millimetres, so the slot is tall, and the board does not stand on four posts but on two rails with a row of holes
# each, 3.5 mm apart: it can be screwed down in any of FAN places, from the lowest (drawn) upwards, wherever the tail reaches its connector without strain.
FSL = (168, 23, 194, 34)                        # the slot for the flex tail (u0, v0, u1, v1)
# Relief for the wide strip, which lies between the glass and the plate in a 1.62 mm gap. Its capacitors are in the last 20 mm at the far end from the
# tail, so the plate has a window right through there (FCW), above the power board's post. The rest of the strip gets a pocket FCD deep in the plate's
# front (FCP), from clear of the foot's screw holes up past the strip's top edge and across to the slot. The plate prints front down, so the pocket's
# roof is a short bridge three layers off the bed.
FCW, FCP, FCD = (50, 18.5, 77, 32.5), (50, 17, 194, 32.5), 0.6
FAZ, FAP, FAN = 12.5, 3.5, 4                    # the w of the board's underside, the pitch of the holes in the rails, and how many places the board can sit
FAC, FAL, FAW = (180.3, 43.25), 42.1, 20.5      # centred on the flex tail, in the lowest place: its bottom edge 1 mm short of the top of the slot
FAU, FAV = (FAC[0] - 17.5, FAC[0] + 17.5), (FAC[1] - 7.0, FAC[1] + 7.0)
FAH = sorted(set(round(v + FAP*j, 2) for v in FAV for j in range(FAN)))      # every hole in a rail (14 mm is four pitches, so the two rows share holes)
def adapter_bodies():
    u0, u1, v0, v1 = FAC[0] - FAL/2, FAC[0] + FAL/2, FAC[1] - FAW/2, FAC[1] + FAW/2
    pcb = SUB(obox(u0, v0, FAZ, u1, v1, FAZ + 1.6), [wcyl(u, v, FAZ - 0.5, FAZ + 2.5, 1.55) for u in FAU for v in FAV])
    return [('PCB', pcb, (30, 70, 150)),
            ('Panel FPC connector 40p', obox(FAC[0] - 15, v0 + 0.9, FAZ + 1.6, FAC[0] + 15, v0 + 6.3, FAZ + 3.6), BEIGE),
            ('FFC connector 40p', obox(FAC[0] - 15, v1 - 6.3, FAZ + 1.6, FAC[0] + 15, v1 - 0.9, FAZ + 3.6), BEIGE)]
# Foam tape between the back of the glass and the front of the plate: single-sided EVA, 2 mm thick and 5 mm wide, squashed to the 1.62 mm gap.
# Strips as (u0, v0, u1, v1), each along an edge of the glass, directly behind the bezel's lip, so the glass is pinched and not levered: down the left,
# down the right in two pieces either side of the encoder's opening in the plate, and across the top as far as the key module's opening.
# Nothing along the bottom, where the panel's flex folds behind the glass.
FOAM = [(13.7, 32, 18.7, 187.4), (225.4, 32, 230.4, 133.8), (225.4, 166.2, 230.4, 187.4), (20.7, 182.4, 139, 187.4)]
def foam_bodies():
    out = []
    for i, (u0, v0, u1, v1) in enumerate(FOAM):
        out.append(('Foam %d' % (i + 1), obox(u0, v0, GLASS_W + 0.78, u1, v1, 5.85), (92, 95, 102)))      # the foam is black; it is drawn grey so it shows against the black plate
        out.append(('Adhesive %d' % (i + 1), obox(u0, v0, 5.85, u1, v1, 6.05), (40, 120, 235)))      # the sticky side, drawn blue: it goes on the plate, not the panel
    return out
# Printed keycaps for the four Cherry MX switches: a flat-topped shell 17.5 mm wide with a round post underneath that pushes onto the switch's cross.
# Each runs from 0.7 mm behind the bezel to 0.8 mm short of the back of the slot in the shell, so the slot is filled: 19.1 mm front to back, with the
# post 3.1 mm behind the middle, over the switch.
# They print top face down, so the post and its socket point straight up. Positions and heights are in frame coordinates (v runs up the switch).
KCU, KCW = (152.325, 171.375, 190.425, 209.475), 17.8          # switch centres across, and front to back
KC_HALF, KC_W0, KC_W1 = 8.75, 6.7, 25.8                        # half the cap's width, and its front and back faces
KC_TOP, KC_SKIRT, KC_SEAT = 198.5, 193.0, 191.1                # top face, bottom of the skirt, and the shoulder on the switch's slider that the post sits on
KC_WALL, KC_ROOF, KC_POST, KC_SOCK = 1.2, 1.4, 2.75, 4.0       # wall, top thickness, post radius, socket depth
KC_ARM = (4.25, 1.35, 4.45)                                    # the cross socket: one arm's length, both arms' width, the other arm's length. The stem's arms are 4.0 x 1.17.
# Marks sunk into the top of each cap so the keys can be told apart by touch: a number of dots, or a bar. Sunk, not raised, because the caps print top face
# down. From the front, left to right: one, two and three dots on the grey keys, and a bar on the orange menu key.
KC_MARKS = (1, 2, 3, 'bar')
KC_MW, KC_DOT, KC_PITCH, KC_BAR, KC_PIT = (KC_W0 + KC_W1)/2, 1.0, 4.0, (8.0, 2.0), 0.5      # the marks' line across the cap, dot radius and spacing, bar size, depth
def mark_offsets(mark): return [] if mark in (0, None, 'bar') else [KC_PITCH*(i - (mark - 1)/2) for i in range(mark)]
def keycap(uc, mark=None):
    cap = keycap_plain(uc)
    if mark == 'bar': return SUB(cap, [obox(uc - KC_BAR[0]/2, KC_TOP - KC_PIT, KC_MW - KC_BAR[1]/2, uc + KC_BAR[0]/2, KC_TOP + 0.1, KC_MW + KC_BAR[1]/2)])
    dots = [cyl(F(uc + dx, KC_TOP - KC_PIT, KC_MW), F(uc + dx, KC_TOP + 0.1, KC_MW), KC_DOT) for dx in mark_offsets(mark)]
    return SUB(cap, dots) if dots else cap
def keycap_plain(uc):
    hu, w = KC_HALF, KCW
    shell = SUB(obox(uc - hu, KC_SKIRT, KC_W0, uc + hu, KC_TOP, KC_W1), [obox(uc - hu + KC_WALL, KC_SKIRT - 0.1, KC_W0 + KC_WALL, uc + hu - KC_WALL, KC_TOP - KC_ROOF, KC_W1 - KC_WALL)])
    cap = U([shell, cyl(F(uc, KC_SEAT, w), F(uc, KC_TOP - KC_ROOF + 0.1, w), KC_POST)])
    a, t, b = KC_ARM
    return SUB(cap, [obox(uc - a/2, KC_SEAT - 0.1, w - t/2, uc + a/2, KC_SEAT + KC_SOCK, w + t/2), obox(uc - t/2, KC_SEAT - 0.1, w - b/2, uc + t/2, KC_SEAT + KC_SOCK, w + b/2)])
def keycap_flat(arm=None, mark=None):
    cap = keycap_flat_plain(arm); ym = KC_MW - KCW
    if mark == 'bar': return SUB(cap, [box(-KC_BAR[0]/2, ym - KC_BAR[1]/2, -0.1, KC_BAR[0]/2, ym + KC_BAR[1]/2, KC_PIT)])
    dots = [cyl((dx, ym, -0.1), (dx, ym, KC_PIT), KC_DOT) for dx in mark_offsets(mark)]
    return SUB(cap, dots) if dots else cap
def keycap_flat_plain(arm=None):
    # the same cap as it prints: top face on Z = 0, centred on the origin
    hu, (a, t, b) = KC_HALF, (arm or KC_ARM)
    zs, zp = KC_TOP - KC_SKIRT, KC_TOP - KC_SEAT
    y0, y1 = KC_W0 - KCW, KC_W1 - KCW             # the post stays on the origin; the long side is the front of the cap
    shell = SUB(box(-hu, y0, 0, hu, y1, zs), [box(-hu + KC_WALL, y0 + KC_WALL, KC_ROOF, hu - KC_WALL, y1 - KC_WALL, zs + 0.1)])
    cap = U([shell, cyl((0, 0, KC_ROOF - 0.1), (0, 0, zp), KC_POST)])
    return SUB(cap, [box(-a/2, -t/2, zp - KC_SOCK, a/2, t/2, zp + 0.1), box(-t/2, -b/2, zp - KC_SOCK, t/2, b/2, zp + 0.1)])
# Printed knob for the rotary encoder: 32 mm across and 12 mm long, with a straight knurl of 54 V-grooves round a band and a chamfer at each end.
# 32 mm is as big as it goes without standing proud of the back of the case: its axis is 16.8 mm in from the back face.
# It pushes onto the encoder's 6 mm D-shaft: the flat is 1.5 mm off the axis and runs the last 6.5 mm of the shaft, and the shaft's tip is the stop.
# It prints outer face down, so the socket points straight up and the grooves run vertically.
KN_X0, KN_X1, KN_R = 246.0, 258.0, 16.0          # back face (2 mm off the side of the case, clear of the bushing's end when pressed), outer face, radius
KN_TIP, KN_SHAFT, KN_FLAT = 253.0, 3.05, 1.55    # where the shaft's tip sits, and the socket: radius, and the flat's distance from the axis
KN_GROOVES, KN_CUT = 54, 0.85                    # number of grooves, and the side of the square that cuts each one (0.6 mm deep)
def knob():
    ky, kz = F(0, KV, KW)[1:]
    ax = lambda x: (x, ky, kz)
    body = U([cone(ax(KN_X0), KN_R - 0.8, ax(KN_X0 + 0.8), KN_R), cyl(ax(KN_X0 + 0.8), ax(KN_X1 - 1.2), KN_R), cone(ax(KN_X1 - 1.2), KN_R, ax(KN_X1), KN_R - 1.2)])
    cuts = []
    xm, L = (KN_X0 + KN_X1)/2 - 0.2, KN_X1 - KN_X0 - 3.0
    tool = tbm.createBox(adsk.core.OrientedBoundingBox3D.create(P(xm, ky, kz + KN_R), adsk.core.Vector3D.create(1, 0, 0), adsk.core.Vector3D.create(0, 1, 1), L*MM, KN_CUT*MM, KN_CUT*MM))
    for i in range(KN_GROOVES):
        t = tbm.copy(tool); m = adsk.core.Matrix3D.create()
        m.setToRotation(2*math.pi*i/KN_GROOVES, adsk.core.Vector3D.create(1, 0, 0), P(0, ky, kz)); tbm.transform(t, m); cuts.append(t)
    # the socket: a short wide mouth over the round part of the shaft, then the D
    cuts.append(cyl(ax(KN_X0 - 0.1), ax(KN_X0 + 0.7), 3.6))
    fn = (0.0, 0.866, 0.5)                        # the way the shaft's flat faces in the model; on the real part it turns with the shaft
    cuts.append(SUB(cyl(ax(KN_X0 + 0.6), ax(KN_TIP), KN_SHAFT), [halfspace((KN_X0, ky + fn[1]*KN_FLAT, kz + fn[2]*KN_FLAT), fn)]))
    return SUB(body, cuts)
FINTOP, BOSSR = 175.8, 2.5                    # how far up the key module's fins go, and the radius of the bosses on them: as much as clears the sockets and parts under the NeoKey (177.9 or 3.0 touch them)
SKIP = {(16, 178, 'W'), (228, 178, 'E'), (228, 178, 'W')}      # braces left off because something is in the way, as (u, v, side): the top two bezel screw heads, and the key module's STEMMA QT plug
def braces(u, v, r, H, L):
    # four triangular fins round the foot of a post, so a knock or an over-tightened screw doesn't snap it off the plate. Each is 1.6 mm thick,
    # reaches L out from the post on the plate and H up it, and prints with the post. Only the tall posts have them: the short board posts are too stubby to snap.
    out = []
    for side, du, dv in (('E', 1, 0), ('W', -1, 0), ('N', 0, 1), ('S', 0, -1)):
        if (u, v, side) in SKIP: continue
        e = r + L
        b = obox(min(u, u + du*e) - abs(dv)*0.8, min(v, v + dv*e) - abs(du)*0.8, 7.5, max(u, u + du*e) + abs(dv)*0.8, max(v, v + dv*e) + abs(du)*0.8, 8.0 + H)
        n = tuple((H - 0.6)*(du*a + dv*c) + L*w for a, c, w in zip(U_, V_, W_))
        out.append(SUB(b, [halfspace(F(u + du*e, v + dv*e, 8.6), n)]))
    return out
# Revision marks. Every printed part carries "MK3 R<n>" sunk 0.4 mm into a face that is hidden once the case is together, so a print on the bench can be
# told from an older one. A part's number goes up by one whenever its printed shape changes; the first plate, printed before the marks, counts as R1.
REV = {'Frame plate': 3, 'Front bezel': 2, 'Base tray': 1, 'Plinth': 1, 'Rear shell': 1, 'Access cover': 4}      # a revision is a change to how a part fits or works; decoration such as the cats does not count
FONT = {'M': '10001 11011 10101 10101 10001 10001 10001', 'K': '10001 10010 10100 11000 10100 10010 10001', 'R': '11110 10001 10001 11110 10100 10010 10001',
        '0': '01110 10001 10011 10101 11001 10001 01110', '1': '00100 01100 00100 00100 00100 00100 01110', '2': '01110 10001 00001 00010 00100 01000 11111',
        '3': '11110 00001 00001 01110 00001 00001 11110', '4': '00010 00110 01010 10010 11111 00010 00010', '5': '11111 10000 11110 00001 00001 10001 01110',
        '6': '00110 01000 10000 11110 10001 10001 01110', '7': '11111 00001 00010 00100 01000 01000 01000', '8': '01110 10001 10001 01110 10001 10001 01110',
        '9': '01110 10001 10001 01111 00001 00010 01100', 'P': '11110 10001 10001 11110 10000 10000 10000', ' ': '00000 00000 00000 00000 00000 00000 00000'}
PXL, STAMP_D = 0.9, 0.4                        # the side of one dot of the 5 x 7 lettering, and how deep it is sunk: letters are 4.5 x 6.3 mm
def stamp(text, o, ex, ey):
    # cutting tools for text on a flat face. o: the bottom left corner of the text, on the face. ex: the way it reads; ey: up the letters, both as
    # seen by someone looking at the face. The tools go into the material along n = -(ex x ey).
    n = (-(ex[1]*ey[2] - ex[2]*ey[1]), -(ex[2]*ey[0] - ex[0]*ey[2]), -(ex[0]*ey[1] - ex[1]*ey[0]))
    out = []
    for i, ch in enumerate(text):
        for r, row in enumerate(FONT[ch].split()):
            c = 0
            while c < 5:
                if row[c] == '0': c += 1; continue
                c1 = c
                while c1 < 5 and row[c1] == '1': c1 += 1
                x, y, L = (6*i + (c + c1)/2)*PXL, (6.5 - r)*PXL, (c1 - c)*PXL
                p = tuple(o[k] + x*ex[k] + y*ey[k] + (STAMP_D/2 - 0.05)*n[k] for k in range(3))
                out.append(tbm.createBox(adsk.core.OrientedBoundingBox3D.create(P(*p), adsk.core.Vector3D.create(*ex), adsk.core.Vector3D.create(*ey), L*MM, PXL*MM, (STAMP_D + 0.1)*MM)))
                c = c1
    return out
# A doodle hidden inside the rear shell, in the manner of the drawings chip designers leave on silicon: two pixel cats, one sitting up and one asleep,
# sunk into the middle of the inside of the back, whiskers and all. Nobody sees them once the case is together. '#' is a sunk dot.
CATS = {'sitting': """
....#....#......
...###..###.....
...########.....
...#.####.#.....
##.########.##..
...###..###.....
##.########.##..
....######......
...########.....
..##########....
..##########..##
..##########..#.
..##########..#.
..####.#####..#.
..####.########.""".split(), 'asleep': """
.............######....
....#...#..###########.
...###.###.############
...#######.############
...#..#..#.############
##.#######...##########
...###.###.############
##.#######...##########
....#####.............#
.....##################""".split()}
def doodle(rows, o, ex, ey):
    # as stamp(), for a picture: o is its bottom left corner
    n = (-(ex[1]*ey[2] - ex[2]*ey[1]), -(ex[2]*ey[0] - ex[0]*ey[2]), -(ex[0]*ey[1] - ex[1]*ey[0]))
    out = []
    for r, row in enumerate(rows):
        c = 0
        while c < len(row):
            if row[c] != '#': c += 1; continue
            c1 = c
            while c1 < len(row) and row[c1] == '#': c1 += 1
            x, y, L = (c + c1)/2*PXL, (len(rows) - 0.5 - r)*PXL, (c1 - c)*PXL
            p = tuple(o[k] + x*ex[k] + y*ey[k] + (STAMP_D/2 - 0.05)*n[k] for k in range(3))
            out.append(tbm.createBox(adsk.core.OrientedBoundingBox3D.create(P(*p), adsk.core.Vector3D.create(*ex), adsk.core.Vector3D.create(*ey), L*MM, PXL*MM, (STAMP_D + 0.1)*MM)))
            c = c1
    return out
def cats(): return doodle(CATS['sitting'], F(102.5, 121, WSO - T), U_, V_) + doodle(CATS['asleep'], F(120.8, 121, WSO - T), U_, V_)      # together 39 mm wide, in the middle of the clear part of the back
def cats_volume(): return sum(row.count('#') for rows in CATS.values() for row in rows)*PXL*PXL*STAMP_D/1000
TURTLE = """
........######........
......##########......
.....###.####.###.....
....####.####.####.###
...######.##.#####.#.#
..####################
##.###############.##.
....###.......###.....
....###.......###.....""".split()
def turtle(): return doodle(TURTLE, (161.5 - 11*PXL, 66.3 + 4.5*PXL, ZB - T), (1, 0, 0), (0, -1, 0))      # 19.8 x 8.1 mm in the middle of the underside of the access cover's roof, its feet towards the cover's back
def turtle_volume(): return sum(row.count('#') for row in TURTLE)*PXL*PXL*STAMP_D/1000
def stamp_volume(text): return sum(row.count('1') for ch in text for row in FONT[ch].split())*PXL*PXL*STAMP_D/1000      # cm3 the mark should remove
CODE = {'Frame plate': 'P1', 'Base tray': 'P2', 'Plinth': 'P3', 'Front bezel': 'P4', 'Rear shell': 'P5', 'Access cover': 'P6'}      # each part's code: the number its print file starts with
def mark_text(part): return ['MK3 R%d' % REV[part], CODE[part]]
def marks(part):
    # where each part's mark goes, always on an inside face: (corner, reading direction, up, where the part's code goes). The code follows the
    # revision on the same line where there is the length for it, and sits on a second line under or over it where there is not.
    nU = neg(U_)
    o, ex, ey, where = {'Frame plate': (F(118, 108, 8.0), nU, V_, 'under'),             # the back of the plate, in the clear middle above the cable-tie anchors
          'Front bezel': (F(3.4, 44, WB), V_, U_, 'after'),               # the back of the left border, reading up it: the plate covers it
          'Base tray': ((139.8, 44, 3.0), (0, 1, 0), (-1, 0, 0), 'over'),   # the floor, between the battery pad and the right bank of vents, reading front to back
          'Plinth': ((30, 68.5, ZP - T), (1, 0, 0), (0, -1, 0), 'after'),    # the underside of its roof
          'Rear shell': (F(50, 120, WSO - T), U_, V_, 'under'),           # the inside of its back
          'Access cover': (F(137.2, 73.5, WBT - T), U_, V_, 'after')}[part]   # the inside of the hump's back
    t, code = mark_text(part)
    dx, dy = {'after': (6*(len(t) + 1)*PXL, 0), 'under': (0, -8.5*PXL), 'over': (0, 8.5*PXL)}[where]
    return stamp(t, o, ex, ey) + stamp(code, tuple(o[k] + dx*ex[k] + dy*ey[k] for k in range(3)), ex, ey)
def marked(part, body):
    egg, egg_v = {'Rear shell': (cats, cats_volume), 'Access cover': (turtle, turtle_volume)}.get(part, (lambda: [], lambda: 0))      # the rear shell carries the cats as well, and the access cover a turtle
    v0 = body.volume; b = SUB(body, marks(part) + egg()); cut, want = v0 - b.volume, sum(stamp_volume(t) for t in mark_text(part)) + egg_v()
    print('%-12s mark %s R%d: %.4f of %.4f cm3 cut%s' % (part, CODE[part], REV[part], cut, want, '' if abs(cut - want) < 0.0005 else '   <-- NOT ON A CLEAR FLAT FACE'))
    return b
def mounts():
    add, cut, tadd, tcut = [], [], [], []      # what stands on the frame plate, and what stands on the base tray
    # Display driver HAT: four posts off the plate to the HAT's corner holes. The two on the right take M2.5 screws from behind; the two under the adapter are locating pins.
    for u, v, screw in ((161.5, 73.57, False), (161.5, 15.56, False), (210.72, 73.57, True), (210.72, 15.56, True)):
        r = 3.25 if screw else 2.75                    # the screwed pair are 6.5 mm across, for more plastic round the thread the M2.5 screw cuts; 7 mm would touch the HAT's micro-USB socket
        add.append(wcyl(u, v, 7.5, 28.5, r)); add += braces(u, v, r, 12.0, 5.0)
        if screw: cut += [wcyl(u, v, 19, 28.6, R25), lead(F(u, v, 28.5), nW_, R25)]
        else: add.append(wcyl(u, v, 28.5, 30.6, 1.15))
    # Power and amp protoboard: four corner posts, M2 from behind (an M2 fits the board's holes)
    for u in PBU:
        for v in PBV: add.append(wcyl(u, v, 7.5, 18, 3.0)); cut += [wcyl(u, v, 11, 18.1, R2), lead(F(u, v, 18), nW_, R2)]
    # E-paper flex adapter: two rails, each with a row of holes for M2.5 through the board's 3.1 mm holes. The holes stop 1.85 mm short of the plate's front.
    for u in FAU:
        add.append(obox(u - 3.0, FAH[0] - 3.25, 7.5, u + 3.0, FAH[-1] + 3.25, FAZ))
        for v in FAH: cut += [wcyl(u, v, 7.9, FAZ + 0.1, R25), lead(F(u, v, FAZ), nW_, R25)]
    # Key module: two triangular fins under the NeoKey, between the keys. At the back of each a boss takes an M2.5 screw down through the board. At the front, where the
    # board's hole is only 1.5 mm off the plate and a boss can only ever be half of one, a plain block off the plate holds the board up and a peg locates it: no screw.
    for u0 in (161.85, 199.95):
        fin = SUB(obox(u0 - 2.5, 147, 7.5, u0 + 2.5, FINTOP, 29.5), [halfspace(F(u0, 147, 8), vadd(W_, V_, -1.0))])
        add.append(fin)
        add.append(cyl(F(u0, 170, 26.05), F(u0, 177.9, 26.05), BOSSR))      # boss up to the board, clear of the sockets and parts under it
        add += [obox(u0 - 2.5, 170, 7.5, u0 + 2.5, 177.9, 12.0), cyl(F(u0, 177.5, 9.54), F(u0, 179.3, 9.54), 1.15)]      # front rest and its peg
        # a buttress each side of the fin, square to it, so a sideways knock on the keys doesn't lever the fin off the plate
        for du in (1, -1):
            e = 2.5 + 10.0
            g = obox(min(u0, u0 + du*e), 165, 7.5, max(u0, u0 + du*e), 167, 8.0 + 16.0)
            add.append(SUB(g, [halfspace(F(u0 + du*e, 166, 8.6), tuple(15.4*du*a + 10.0*w for a, w in zip(U_, W_)))]))
        cut += [cyl(F(u0, 169, 26.05), F(u0, 178.2, 26.05), R25), lead(F(u0, 177.9, 26.05), tuple(-x for x in V_), R25)]
    # Encoder: a bracket the threaded bushing bolts through, with a rib above and below
    ky, kz = F(0, KV, KW)[1:]
    add += [obox(238.1, 133, 6.05, 241.3, 167, 27.5), obox(232.2, 131, 7.5, 241.3, 133.2, 22), obox(232.2, 166.8, 7.5, 241.3, 169, 22)]      # the ribs stop short of the board: its two STEMMA QT sockets open up and down, and the plugs need the room
    cut.append(cyl((237.5, ky, kz), (241.6, ky, kz), 3.65))
    # Speaker: a baffle wall standing on the floor and tied into the plate, M4 through the two lugs
    tadd.append(INTER(box(15, 16.4, 3, 18, YR - 5.5, 61), obox(-10, -60, 8.4, 260, 300, 200)))
    # a short duct from the baffle to the side wall so the sound goes straight out of the grille; open below so the shell's front post can slide past
    duct = U([INTER(cyl((3.0, 49, 33.3), (15, 49, 33.3), 25.5), slab(T + G)), cyl((PI + T + G, 49, 33.3), (15, 49, 33.3), 25.5)])      # long inside the frame, short inside the narrower plinth
    tadd.append(SUB(duct, [cyl((2, 49, 33.3), (16, 49, 33.3), 23.5), box(0, 0, 0, 20, 120, 23.5)]))
    tcut.append(cyl((14, 49, 33.3), (19, 49, 33.3), 23.0))
    for y, z in ((70.21, 54.51), (27.79, 12.09)): tcut.append(cyl((14, y, z), (19, y, z), 2.2))
    tcut.append(NUT_RELIEF()); tcut += TIP_RELIEF()
    # Battery: a raised pad with low walls and two tunnels for cable ties
    bx = lambda x0, y0, z0, x1, y1, z1: box(x0 + BX, y0, z0, x1 + BX, y1, z1)
    tadd += [bx(78.7, 44.7, 3, 134.9, 73.4, 7),
            bx(85, 44.7, 7, 99, 46.7, 21), bx(115, 44.7, 7, 129, 46.7, 21), bx(85, 71.3, 7, 99, 73.3, 21), bx(115, 71.3, 7, 129, 73.3, 21),
            bx(78.7, 52, 7, 80.7, 66, 21), bx(133.3, 52, 7, 134.9, 66, 21)]
    tcut += [bx(91, 44, 4.2, 97, 74, 5.8), bx(117, 44, 4.2, 123, 74, 5.8)]
    # USB-C inlet: a pedestal behind the battery that carries the breakout, with the port surround as part of the chassis
    tadd += [box(109 + PX, 73.3, 3, 134.6 + PX, YR, 10), box(109 + PX, 73.3, 10, 134.6 + PX, YR, 42), box(109 + PX, YR - T, 42, 134.6 + PX, YR, 58)]
    # Power switch: the same pedestal carried along to the left, with a 6.3 mm hole for the toggle's bushing. It stays on the chassis when the plinth comes off.
    tadd += [box(88 + PX, 73.3, 3, 109 + PX, YR, 10), box(88 + PX, 73.3, 10, 109 + PX, YR, 42), box(88 + PX, YR - T, 42, 109.1 + PX, YR, 58)]
    tcut += [cyl((SWX, YR - T - 1, SWZ), (SWX, YR + 1, SWZ), 3.15)]
    ux, uz, uw, uh = USB
    r = 1.5; x0, x1, z0, z1, y0, y1 = ux - uw/2, ux + uw/2, uz - uh/2, uz + uh/2, YR - T - 1, YR + 1
    tcut += [box(111.2 + PX, YR - T - 0.1, 42, 132.8 + PX, YR - 0.4, 46.0)]       # relief for the board's edge and its two screw heads
    tcut += [box(x0 + r, y0, z0, x1 - r, y1, z1), box(x0, y0, z0 + r, x1, y1, z1 - r)] + [cyl((x, y0, z), (x, y1, z), r) for x in (x0 + r, x1 - r) for z in (z0 + r, z1 - r)]
    for x in (114.36 + PX, 129.6 + PX): tcut += [cyl((x, YR - 3.06, 33), (x, YR - 3.06, 42.1), R25), lead((x, YR - 3.06, 42.0), (0, 0, -1), R25)]
    # Cable-tie anchors on the back of the plate, above the boards
    for u in (70, 122, 175): add.append(obox(u - 5, 93, 7.5, u + 5, 97, 12.5)); cut.append(obox(u - 3, 92, 8.0, u + 3, 98, 10.5))
    # Rear shell: four tall posts off the plate that reach the inside of the shell's back; M3 through the back, threading into the post ends
    for u, v, r in BACK: add.append(wcyl(u, v, 7.5, WSO - T - BOSS, r)); add += braces(u, v, r, 14.0, 5.0); cut += [wcyl(u, v, 23, WSO - T + 0.1, R3), lead(F(u, v, WSO - T - BOSS), nW_, R3)]      # they stop at the bosses inside the shell's back that take the recessed heads
    # Floor vents against trapped moisture: two banks of 2.5 mm slots, clear of the battery pad, and the four stick-on feet hold the base 2 mm up so air can get under it
    for x0, n in ((24, 8), (145, 12)):
        for i in range(n):
            xs = x0 + 7*i; y0 = max([22.0] + [y + 7 for x, y in HS if abs(xs + 1.25 - x) < 7])      # a slot beside a heel screw starts behind it
            tcut.append(box(xs, y0, -3, xs + 2.5, 43 if x0 == 24 and xs + 2.5 > 77.7 + BX else 62, 3.5))      # and one the battery pad would cover stops in front of it
    return add, cut, tadd, tcut
def gusset(x, ytip, ztop):
    # a triangular rib from a heel on the tray floor up the back of the plate, with a boss at the heel for a screw from below
    ytop = 7.75 + (ztop - 10)*S/C                                                             # the plate's back face at that height
    rib = INTER(box(x - 2, 5, 3, x + 2, ytip, ztop), obox(-10, -60, 6.05, 260, 300, 80))
    rib = SUB(rib, [halfspace((x, ytip, 10.0), (0, ztop - 10.0, ytip - ytop - 3))])
    return [rib, cyl((x, ytip - 4.2, 3), (x, ytip - 4.2, 12), 4.2)]
def plate():
    # black: the leaning plate everything hangs off. Prints flat on its front face with every post pointing straight up.
    p = INTER(obox(2.7, -40, 6.05, 241.3, 193, 8.0), box(-10, -60, 10, 260, 200, 300))
    # the foot: a beam the full width that sits on the tray floor behind its front wall and runs 10 mm up the back of the plate, so the two are one piece
    foot = U([INTER(box(PI + T + 0.3, 5, 3, 244 - PI - T - 0.3, 16, 20), obox(-10, -60, 6.05, 260, 300, 80))] + [b for g in GUS for b in gusset(*g)])
    foot = U([foot] + [INTER(box(x0, 5, 10 + G + T + 0.35, x1, 16, 23), obox(-10, -60, 6.05, 260, 300, 80)) for x0, x1 in ((2.9, PI + T + 0.4), (244 - PI - T - 0.4, 241.1))])      # the beam carried out over the shell's floor, for its corner screws
    foot = SUB(foot, [box(0, 0, 0, 244, 8.8, 10)])                                            # the tray's front wall, which locates it
    add, cut, _, _ = mounts()
    m = U([p, foot] + add)
    cuts = [box(0, 0, 0, PI + T + 0.3, 60, 10 + G + T + 0.3), box(244 - PI - T - 0.3, 0, 0, 244, 60, 10 + G + T + 0.3)]      # bottom corners, clear of the rear shell's floor where it overhangs the tray
    cuts += [obox(140, 171, 5, 222, 200, 9), obox(224.5, 134.8, 5, 238.1, 165.2, 9), obox(FSL[0], FSL[1], 5, FSL[2], FSL[3], 9), obox(FCW[0], FCW[1], 5, FCW[2], FCW[3], 9), obox(FCP[0], FCP[1], 5, FCP[2], FCP[3], 6.05 + FCD), obox(-5, 186, 5, 9, 200, 9), obox(235, 186, 5, 250, 200, 9)]
    cuts += [wcyl(u, v, 5, 9, 1.7) for u, v in BZ] + [cyl((x, y, 12), (x, y, 22.5), R3) for x, y in CS] + [lead((x, y, 10 + G + CB + SEAT + 0.3), (0, 0, 1), R3) for x, y in CS] + [cyl((x, y, 12), (x, y, 10 + G + CB + SEAT + 0.3), 4.8) for x, y in CS]      # with room for the shell's bosses under them
    cuts += [cyl((x, y, 2.9), (x, y, 15.8), R3) for x, y in FS] + [cyl((x, y, 2.9), (x, y, 11.8), R3) for x, y in HS] + [lead((x, y, CB + SEAT + 0.15), (0, 0, 1), R3) for x, y in FS + HS]      # the foot takes M3 x 12, the heels M3 x 8
    cuts += [cyl((x, y, 2.9), (x, y, CB + SEAT + 0.15), 4.5) for x, y in FS + HS]      # pockets over the tray's bosses
    return SUB(m, cuts + cut)
def tray():
    # black: the flat base. Prints as it stands.
    t = SUB(box(PI, 6, 0, 244 - PI, YR, 10), [box(PI + T, 8.5, 3, 244 - PI - T, YR - T, 11)])      # as deep as the plinth, so the plinth's rear wall sits on the tray's with no overhang
    posts = [cyl((x, y, 3), (x, y, 10), 4.0) for x, y in POSTS]
    _, _, tadd, tcut = mounts()
    bosses = [cyl((x, y, 2.5), (x, y, CB + SEAT), 4.2) for x, y in FS + HS]      # the floor is 3 mm, so it is built up around each screw to take the whole head
    m = U([t] + posts + bosses + tadd)
    cuts = [cyl((x, y, -1), (x, y, 11), 1.7) for x, y in POSTS] + [cyl((x, y, -1), (x, y, CB), CBR) for x, y in POSTS]
    cuts += [cyl((x, y, -1), (x, y, 6), 1.7) for x, y in FS + HS] + [cyl((x, y, -1), (x, y, CB), CBR) for x, y in FS + HS]
    return SUB(m, cuts + tcut)
def halfspace(p, n):
    L = math.sqrt(sum(v*v for v in n)); n = [v/L for v in n]
    t = (1, 0, 0) if abs(n[0]) < 0.9 else (0, 1, 0)
    w = (n[1]*t[2] - n[2]*t[1], n[2]*t[0] - n[0]*t[2], n[0]*t[1] - n[1]*t[0])
    Lw = math.sqrt(sum(v*v for v in w)); w = [v/Lw for v in w]
    c = [p[i] + n[i]*600 for i in range(3)]
    return tbm.createBox(adsk.core.OrientedBoundingBox3D.create(P(*c), adsk.core.Vector3D.create(*n), adsk.core.Vector3D.create(*w), 1200*MM, 1200*MM, 1200*MM))
WSO, ZP, ZB, WBT, BU0, BU1, EC = 35.5, 64.0, 80.0, 61.0, 128.5, 194.5, 1.5   # slab back plane, plinth top, buttress top and back plane, buttress sides, edge chamfer
BLACK = (38, 39, 42)
SWX, SWZ = 98.5 + PX, 50.0                      # power toggle: bushing axis, through the rear of the chassis beside the USB-C inlet
def cham(e, n1, n2, d=None):
    # removal volume for a chamfer d wide along the edge through e, between faces with outward normals n1 and n2
    d = EC if d is None else d
    return halfspace(tuple(e[i] - d*n1[i] for i in range(3)), tuple(n1[i] + n2[i] for i in range(3)))
XL, XR, ZU, YB = (-1, 0, 0), (1, 0, 0), (0, 0, 1), (0, 1, 0)
def chamo(e, n1, n2, d, off):
    # as cham(), for a surface offset inwards by off
    n = tuple(n1[i] + n2[i] for i in range(3)); L = math.sqrt(sum(x*x for x in n))
    return halfspace(tuple(e[i] - d*n1[i] - off*n[i]/L for i in range(3)), n)
def above_plinth(): return box(-10, -80, ZP, 260, 300, 320)
def side_chamfers(off):
    # down the whole height: with the plinth narrower than the frame, the frame's back corners show all the way to the bottom
    back = F(0, 0, WSO)
    return [chamo((0, back[1], back[2]), XL, W_, CH, off), chamo((244, back[1], back[2]), XR, W_, CH, off)]
def slab(off):
    # the leaning frame: parallel front and back, the full width, with a deep chamfer round the back edge so it reads thinner than it is
    s = INTER(box(off, -80, 10 + (-1 if off else G), 244 - off, 200, 320), obox(-50, -300, WB + (-1 if off else G), 300, 196 - off, WSO - off))
    top = F(0, 196, 0)
    return SUB(s, [chamo((0, top[1], top[2]), XL, V_, EC, off), chamo((244, top[1], top[2]), XR, V_, EC, off), chamo(F(0, 196, WSO), V_, W_, CH, off)] + side_chamfers(off))
def grille(side, inset=0.0):
    # removal slots through a side wall over the speaker cone. They lean with the frame, and the bar between two of them sits on the seam between shell and plinth.
    # The frame's wall is cut deep enough to get through the chamfer at its back corner; the plinth's wall is the inset one.
    x0, x1 = (inset - 1, inset + T + (0.5 if inset else 6.0)) if side < 0 else (244 - inset - T - (0.5 if inset else 6.0), 245 - inset)
    out = []
    for wc in (24.5, 30.5, 40.5, 46.5, 52.5, 58.5):      # the bar on the seam is 7.5 mm, so each colour keeps 3.75 mm beside it
        h = math.sqrt(max(0.0, 22.0**2 - (wc - 41.8)**2))
        if h >= 6: out.append(INTER(obox(-5, 33.3 - h, wc - 1.25, 250, 33.3 + h, wc + 1.25), box(x0, -80, 13, x1, 200, 60)))
    return out
def buttress(off):
    # the block on the back of the frame that covers the display driver stack
    s = INTER(box(BU0 + off, -80, 10 - (1 if off else 0), BU1 - off, 200, ZB - off), obox(-50, -300, WB - (1 if off else 0), 300, 500, WBT - off))
    if off: return s
    yb = (WBT + (ZB - 12)*S)/C; back = F(0, 0, WBT)
    s = SUB(s, [cham((0, yb, ZB), ZU, W_), cham((BU0, 0, ZB), XL, ZU), cham((BU1, 0, ZB), XR, ZU), cham((BU0, back[1], back[2]), XL, W_), cham((BU1, back[1], back[2]), XR, W_)])
    return INTER(s, box(-10, -80, ZP, 260, 200, 320))
def plinth_solid(off):
    s = INTER(box(PI + off, -80, 10 + (-1 if off else G), 244 - PI - off, YR - off, ZP - off), obox(-50, -300, (WB - 1) if off else WSO + G, 300, 500, 400))
    if off: return s
    return SUB(s, [cham((0, YR, ZP), YB, ZU), cham((PI, 0, ZP), XL, ZU), cham((244 - PI, 0, ZP), XR, ZU), cham((PI, YR, 0), XL, YB), cham((244 - PI, YR, 0), XR, YB)])
def cavity(): return U([slab(T), buttress(T), plinth_solid(T)])
def post(x, y):
    return [cyl((x, y, 10 + G), (x, y, 22), 4.0), box(x - 1.5, y, 10 + G, x + 1.5, YR - T + 0.5, 22)]          # tied back to the plinth's rear wall
def shelves():
    back = F(0, 0, WSO); band = lambda: box(0.6, -80, ZP - T, 243.4, 300, ZP)
    inside = lambda: obox(-50, -300, WB + 2, 300, 500, WSO)
    return [INTER(INTER(chamo((x, back[1], back[2]), n, W_, CH, 0), band()), inside()) for x, n in ((0, XL), (244, XR))]
def shell():
    # beige: the back and sides of the frame, with an opening where the access cover sits. Slides off backwards once the plinth is off.
    s = SUB(slab(0), [cavity()])
    # the frame overhangs the tray and plinth on each side, so its underside is closed there with a floor
    s = U([s] + [INTER(slab(0), box(x0, -80, 10 + G, x1, 300, 10 + G + T)) for x0, x1 in ((0, PI + T), (244 - PI - T, 244))])
    # bosses inside, behind every recessed screw head: on the back around the six back screws, and on the floor around the two corner screws
    s = U([s] + [wcyl(u, v, WSO - T - BOSS, WSO - T + 0.5, 5.0) for u, v, r in BACK] + [INTER(cyl((x, y, 10 + G + T - 0.5), (x, y, 10 + G + CB + SEAT), 4.5), obox(-10, -60, 8.3, 260, 300, 80)) for x, y in CS])
    cuts = TIP_RELIEF() + [cyl((x, y, 9), (x, y, 16.5), 1.7) for x, y in CS] + [cyl((x, y, 9), (x, y, 10 + G + CB), CBR) for x, y in CS] + [obox(142.6, 190, WB - 1, 219.9, 200, 26.6), NUT_RELIEF()] + [wcyl(u, v, WSO - T - BOSS - 1, WSO + 1, 1.7) for u, v, r in BACK] + [wcyl(u, v, WSO - CB, WSO + 1, CBR) for u, v, r in BACK]                       # key channel, open to the front
    # slats on the top left, mirroring the key block. Across the top they are 1 mm grooves; down the back they go right through, as the high outlet for the floor vents
    cuts += [SUB(U([obox(25 + 4.5*i, 166, 12, 27 + 4.5*i, 300, 100) for i in range(17)]), [slab(1.0)])]
    cuts += [obox(25 + 4.5*i, 166, WSO - T - 1, 27 + 4.5*i, 187, WSO + 1) for i in range(17)]
    cuts += grille(-1) + grille(1)
    ky, kz = F(0, KV, KW)[1:]
    cuts += [cyl((235, ky, kz), (246, ky, kz), 6.25), INTER(box(235, ky - 70, kz - 6.25, 246, ky, kz + 6.25), obox(-50, -300, WB - 1, 300, 500, 400))]   # level slot for the encoder bushing and its nut, open to the front
    return SUB(s, cuts)
# The access cover's latches. On R1 and R2 two tabs hung forward from the inside of the cover's back, where no finger could reach them. From R3 the front
# part of each side wall is itself the spring: a slit under the roof frees a strip of the wall, 1.5 mm thick and joined to the cover only at the back, and
# the hook that goes through the opening in the rear shell's back wall (w = WSO - T to WSO) is on the inside of the strip's front end. Squeezing the two
# sides of the cover near the front pushes both hooks clear of the opening's edges, and the cover lifts straight back. Each hook's back face is a 45 degree
# ramp that crosses the back of the wall, so the wall's inner corner sits on it and pulls the cover forward onto the shell with no play; without a squeeze
# each strip must bend 1.2 mm, about 11 N of pull in PLA. A pad behind the barb stands 0.2 mm into the edge of the opening, in case the corner prints round.
# From R4 the prong in front of the cover's face is the full height of the opening, so the cover cannot ride up off the plinth, and is splayed 3 degrees
# outwards about its root, which puts its barb about 0.3 mm further past the edge. The cover prints top down: the block joining hook and strip grows at 45 degrees from the
# strip, each strip's first layer is a bridge from its root to a pip across the front of the slit (cut or snapped once the print is off the bed), and the
# two prongs, which start 2.75 mm above the bed outside the cover, need support.
TW0, TWP, TWR, TB = 30.6, 32.35, 33.85, 1.5     # the hook's front end, the barb's peak, where its back ramp runs out (w), and how far the barb stands proud of the hook
TPW, TPH = (33.0, 35.0), 0.5                    # the side pad: from the back of the wall to 0.5 mm short of its face (w), and how proud of the hook it stands
TS, TSL, TZ, TSG, TSF = 1.5, 57.0, 77.2, 0.6, 0.3   # the sprung strip: thickness, where its root is (w), its top (z), the slit above it, and how far its foot is held off the plinth's roof
THL, TPIP = 38.5, (0.6, 0.8)                    # how far back the block joining hook and strip runs (w), and the pip across the slit (u, w)
THT, TSP, TLI = 77.25, 3.0, 0.4                           # the prong's top (z): the opening's top edge is at ZB - T = 77.5, so the cover cannot ride up; and how far the prong is splayed outwards, in degrees; and how far inside the prong's face the lead-in starts
def cover():
    # beige: the hump over the Pico's USB-C port, as a separate cover. It slides forward on the plinth roof and two hooks click behind the edges of the opening in the rear shell; squeezing its sides lets them go.
    c = INTER(SUB(buttress(0), [cavity()]), obox(-50, -300, WSO + G, 300, 500, 400))
    add = []; cut = []; wf = WSO + G
    sp = lambda a, b: (min(a, b), max(a, b))
    for sgn, uin, ztop, zcap in ((-1, BU0 + T, TZ, None), (1, BU1 - T, TZ, 75.5)):      # inside the shell the right prong stops short of the HAT's 13-pin header
        zbox = lambda: box(-10, -80, 68.5, 260, 200, ztop)
        uo = uin + sgn*T; us = uo - sgn*TS                                   # the side wall's outer face, and the strip's inner face
        u0 = uin - sgn*0.3                                                   # hook face next to the opening's edge
        (a0, a1), (t0, t1) = sp(uo + sgn, uin), sp(us, uin)
        cut.append(INTER(obox(a0, -300, wf - 1, a1, 500, TSL), box(-10, -80, TZ, 260, 200, TZ + TSG)))               # the slit
        cut.append(INTER(obox(t0, -300, wf - 1, t1, 500, TSL), box(-10, -80, 60, 260, 200, TZ + TSG)))               # the strip, thinned from inside
        cut.append(INTER(obox(a0, -300, wf - 1, a1, 500, TSL), box(-10, -80, 60, 260, 200, ZP + TSF)))               # its foot, off the roof
        p0, p1 = sp(us, us + sgn*TPIP[0])
        add.append(INTER(obox(p0, -300, wf, p1, 500, wf + TPIP[1]), box(-10, -80, TZ - 0.05, 260, 200, TZ + TSG + 0.05)))
        span = lambda d: sp(u0, u0 + sgn*d)                                  # from the hook face d outwards, towards the edge
        fbox = lambda: box(-10, -80, 68.5, 260, 200, THT)                    # the prong, in front of the cover's face, is the full height of the opening
        h0, h1 = sp(us + sgn*0.2, u0 - sgn*2.0); g0, g1 = sp(u0, u0 - sgn*2.0)
        # printed top down, so the block inside the cover may only grow 45 degrees inwards from the strip; the prong in front of it stands on support from the bed
        add.append(SUB(INTER(obox(h0, -300, wf, h1, 500, THL), zbox()), [halfspace((us, 0, ztop), (-sgn, 0, 1))]))
        add.append(INTER(obox(g0, -300, wf - 0.5, g1, 500, wf), fbox()))
        (b0, b1), a, r = span(TB), F(0, 0, TW0), F(0, 0, TWR)
        barb = INTER(obox(b0, -300, TW0, b1, 500, TWR), fbox())
        k = (TB + TLI)/(TWP - TW0)                                           # lead-in slope: it starts inside the prong's face, so the splayed tip still enters the opening
        prong = U([SUB(INTER(obox(g0, -300, TW0, g1, 500, wf), fbox()), [halfspace((u0 - sgn*TLI, a[1], a[2]), (sgn, -k*W_[1], -k*W_[2]))]),
                   SUB(barb, [halfspace((u0 - sgn*TLI, a[1], a[2]), (sgn, -k*W_[1], -k*W_[2])), halfspace((u0, r[1], r[2]), (sgn, W_[1], W_[2]))]),
                   INTER(obox(*((lambda q: (q[0], -300, TPW[0], q[1], 500, TPW[1]))(span(TPH)))), fbox())])
        m = adsk.core.Matrix3D.create(); m.setToRotation(sgn*math.radians(TSP), adsk.core.Vector3D.create(*V_), P(*F(u0 - sgn*2.0, 62.0, wf)))
        if zcap: prong = SUB(prong, [INTER(obox(-50, -300, TW0 - 2, 300, 500, WSO - T - 0.1), box(-10, -80, zcap, 260, 200, 90))])
        tbm.transform(prong, m); add.append(prong)                           # splayed outwards about the line where it leaves the cover's face
    return U([SUB(c, cut)] + add)
def switch_bodies():
    x0, x1, z0, z1, yw = SWX - 6.3, SWX + 6.3, SWZ - 6.6, SWZ + 6.6, YR - T
    red, metal = (200, 38, 44), (192, 192, 197)
    lever = U([cyl((SWX, yw + 8, SWZ), (SWX, YR + 17, SWZ + 2.6), 1.5), sphere((SWX, YR + 17, SWZ + 2.6), 1.5)])
    pins = U([box(x - 0.4, yw - 13.5, z - 1.0, x + 0.4, yw - 9.5, z + 1.0) for x in (SWX - 2.4, SWX + 2.4) for z in (SWZ - 4.7, SWZ, SWZ + 4.7)])
    return [('Body', box(x0, yw - 9.5, z0, x1, yw - 2.0, z1), red), ('Frame', box(x0, yw - 2.0, z0, x1, yw, z1), metal),
            ('Bushing', cyl((SWX, yw, SWZ), (SWX, yw + 9, SWZ), 3.0), metal), ('Nut', cyl((SWX, YR, SWZ), (SWX, YR + 2, SWZ), 4.6), metal),
            ('Lever', lever, metal), ('Terminals', pins, metal)]
def screw(p, d, length, r, rh, hh):
    # p: the point under the head, d: unit direction the thread runs in
    return U([cyl(vadd(p, d, -hh), p, rh), cyl(p, vadd(p, d, length), r)])
def screws():
    zinc = (186, 189, 195); out = []; nW = neg(W_)
    def add(kind, bodies): out.extend(('%s screw %d' % (kind, i + 1), b, zinc) for i, b in enumerate(bodies))
    add('Bezel', [screw(F(u, v, 8.0), nW, 7.0, 1.5, 2.8, 1.8) for u, v in BZ])                       # M3 x 8 through the plate into the bezel
    add('Shell', [screw((x, y, CB), (0, 0, 1), 16.0, 1.5, 2.75, 3.0) for x, y in POSTS])             # M3 x 16 up through the base into the plinth posts
    add('HAT', [screw(F(u, v, 30.1), nW, 8.0, 1.0, 2.25, 1.6) for u, v in ((210.72, 73.57), (210.72, 15.56))])   # M2.5 x 8
    add('Board', [screw(F(u, v, 19.6), nW, 6.0, 0.75, 1.9, 1.3) for u in PBU for v in PBV])   # M2 x 6, power and amp board
    add('Adapter', [screw(F(u, v, FAZ + 1.6), nW, 6.0, 1.0, 2.25, 1.6) for u in FAU for v in FAV])   # M2.5 x 6, flex adapter
    add('Key', [screw(F(u0, 179.5, 26.05), neg(V_), 8.0, 1.0, 2.25, 1.6) for u0 in (161.85, 199.95)])   # M2.5 x 8 down through the NeoKey's two back holes
    add('Speaker', [U([screw((21.1, y, z), (-1, 0, 0), 10.0, 2.0, 3.5, 2.4), cyl((19.5, y, z), (21.1, y, z), 4.5), cyl((12.0, y, z), (15.0, y, z), 3.8)]) for y, z in ((70.21, 54.51), (27.79, 12.09))])   # M4 x 10 with two washers under the head and a nut behind the baffle
    add('Back', [screw(F(u, v, WSO - CB), nW, 8.0, 1.5, 2.75, 3.0) for u, v, r in BACK])                      # M3 x 8 through the back of the rear shell into the chassis posts
    add('Corner', [screw((x, y, 10 + G + CB), (0, 0, 1), 8.0, 1.5, 2.75, 3.0) for x, y in CS])               # M3 x 8 up through the shell's floor into the frame plate's corners
    add('USB', [screw((x, YR - 3.06, 43.6), (0, 0, -1), 8.0, 1.0, 2.25, 1.6) for x in (114.36 + PX, 129.6 + PX)])
    add('Frame', [screw((x, y, CB), (0, 0, 1), 12.0, 1.5, 2.75, 3.0) for x, y in FS] + [screw((x, y, CB), (0, 0, 1), 8.0, 1.5, 2.75, 3.0) for x, y in HS])                # M3 x 12 up through the tray into the frame plate's foot, M3 x 8 into the gusset heels    # M2.5 x 8 down through the breakout
    return out
def feet():
    # bought, not printed: self-adhesive rubber pads on the flat underside of the tray, clear of every screw head
    return [('Foot %d' % (i + 1), cyl((x, y, -2), (x, y, 0), 6.0), (28, 28, 30)) for i, (x, y) in enumerate(FEET)]
def plinth():
    # black: the box behind the foot of the frame, over the battery, speaker and USB-C inlet. Slides off backwards.
    s = SUB(plinth_solid(0), [cavity()])
    rear = [p for p in POSTS if p[1] > 40]
    s = U([s] + [b for x, y in rear for b in post(x, y)])
    cuts = [cyl((x, y, 9), (x, y, 20.5), R3) for x, y in rear] + [lead((x, y, 10 + G), (0, 0, 1), R3) for x, y in rear]                             # deep enough for M3 x 16
    cuts += [box(87.7 + PX, YR - T - 1, 9, 134.9 + PX, YR + 1, 58.3)]                                   # notch around the chassis's USB-C and power-switch pedestal
    cuts += TIP_RELIEF() + grille(-1, PI) + grille(1, PI)                                                   # the speaker fires out of the left one; the right one matches it
    return SUB(s, cuts)
def run(_context: str):
    case = doc_named('Bedside case'); case.activate()
    design = adsk.fusion.Design.cast(app.activeProduct); rc = design.rootComponent
    g = lambda n: [o for o in rc.occurrences if o.name.startswith(n)]
    global USB
    e = g('5880 Rotary')[0]; l, h = local_bbox(e, False); ec = [(a + b)/2/MM for a, b in zip(l, h)]
    ENC_REF = (ec[0] - 12.7, ec[1] - 12.7, ec[2] + 13)
    put(g('5880 Rotary')[0], (V_, W_, neg(U_)), ENC_REF, F(227, 137.3, 6))
    put(g('4090 USB C')[0], ((1,0,0), (0,1,0), (0,0,1)), ('min', 'max', 'min'), (111.8 + PX, YR - 0.5, 42))
    con = [k for k in g('4090 USB C')[0].childOccurrences if 'USB_C' in k.name][0].boundingBox
    USB = ((con.minPoint.x + con.maxPoint.x)/2/MM, con.maxPoint.z/MM - 1.63, 9.44, 3.76)      # shell is 8.94 x 3.26 with 1.25 mm corners
    bat = g('LiPo')[0]; m = bat.transform2; tr = m.translation
    tr.x += (81.0 + BX)*MM - bat.boundingBox.minPoint.x; m.translation = tr; bat.transform2 = m
    # the panel sits with the front of its glass on the pocket's lip
    pan = g('E Ink')[0]; gl = [b for b in pan.component.bRepBodies if b.name == 'Glass'][0]
    m = pan.transform2; tr = m.translation
    w_now = min((v.geometry.y/MM)*C - (v.geometry.z/MM - 12)*S for v in [x for x in gl.createForAssemblyContext(pan).vertices])
    d = (GLASS_W - w_now)*MM
    tr.y += d*W_[1]; tr.z += d*W_[2]; m.translation = tr; pan.transform2 = m
    print('panel moved %.2f mm along W' % (d/MM))
    if design.snapshots.hasPendingSnapshot: design.snapshots.add()
    for n in ('Front bezel', 'Mid chassis', 'Base tray', 'Frame plate', 'Rear shell', 'Plinth', 'Accent stripe', 'Access cover', 'Power switch', 'Screws', 'Rubber feet', 'Panel foam tape', 'Waveshare 10.3', 'Keycaps', 'Knob', 'Floppy slot insert'):
        for o in g(n): o.deleteMe()
    made = {}
    COL = {'Front bezel': BEIGE, 'Base tray': BLACK, 'Frame plate': BLACK, 'Rear shell': BEIGE, 'Plinth': BLACK, 'Access cover': BEIGE}
    for n, fn in (('Front bezel', bezel), ('Base tray', tray), ('Frame plate', plate), ('Rear shell', shell), ('Plinth', plinth), ('Access cover', cover)):
        o = rc.occurrences.addNewComponent(adsk.core.Matrix3D.create()); o.component.name = n
        add_bodies(o.component, [(n, marked(n, fn()), COL[n])], design); made[n] = o.component
        b = o.component.bRepBodies.item(0)
        print('%-12s volume %.0f cm3, %d lump(s)' % (n, b.volume, b.lumps.count))
    def uvw(p):
        y, z = p.y/MM, p.z/MM - 12
        return (p.x/MM, y*S + z*C, y*C - z*S, y, p.z/MM)          # u, v, w, Y, Z
    def pick(body, pred):
        col = adsk.core.ObjectCollection.create()
        for e in body.edges:
            if e.startVertex is None or e.endVertex is None: continue
            a, b = uvw(e.startVertex.geometry), uvw(e.endVertex.geometry)
            if pred(a, b): col.add(e)
        return col
    def near(x, t, tol=0.02): return abs(x - t) < tol
    def side(u): return near(u, 0) or near(u, 244)
    def fillet(comp, edges, r, label):
        if edges.count == 0: print(label, ': no edges'); return
        try:
            fi = comp.features.filletFeatures.createInput()
            fi.isRollingBallCorner = True
            fi.edgeSetInputs.addConstantRadiusEdgeSet(edges, adsk.core.ValueInput.createByReal(r*MM), True)
            comp.features.filletFeatures.add(fi); print(label, ': r', r, 'on', edges.count, 'edges')
        except Exception as ex: print(label, 'FAILED:', str(ex)[:120])
    def chamfer(comp, edges, d, label):
        if edges.count == 0: print(label, ': no edges'); return
        try:
            ci = comp.features.chamferFeatures.createInput2()
            ci.chamferEdgeSets.addEqualDistanceChamferEdgeSet(edges, adsk.core.ValueInput.createByReal(d*MM), False)
            comp.features.chamferFeatures.add(ci); print(label, ':', d, 'mm on', edges.count, 'edges')
        except Exception as ex: print(label, 'FAILED:', str(ex)[:120])
    def cut_tools(comp, tools, label):
        try:
            n0 = comp.bRepBodies.count
            bf = comp.features.baseFeatures.add(); bf.startEdit()
            for t in tools: comp.bRepBodies.add(t, bf)
            bf.finishEdit()
            tc = adsk.core.ObjectCollection.create()
            for i in range(n0, comp.bRepBodies.count): tc.add(comp.bRepBodies.item(i))
            cin = comp.features.combineFeatures.createInput(comp.bRepBodies.item(0), tc); cin.operation = adsk.fusion.FeatureOperations.CutFeatureOperation
            comp.features.combineFeatures.add(cin); print(label, 'cut,', len(tools), 'tools')
        except Exception as ex: print(label, 'FAILED:', str(ex)[:120])
    # --- front bezel ---
    comp = made['Front bezel']; body = lambda: comp.bRepBodies.item(0)
    chamfer(comp, pick(body(), lambda a, b: near(a[2], WF) and near(b[2], WF) and all(15 < p[0] < 229 and 24 < p[1] < 186 for p in (a, b))), 2.0, 'screen bevel')
    chamfer(comp, pick(body(), lambda a, b: near(a[0], b[0]) and all(near(p[1], 196) and side(p[0]) for p in (a, b))), EC, 'bezel top corners')
    chamfer(comp, pick(body(), lambda a, b: near(a[2], WF) and near(b[2], WF) and not all(11 < p[0] < 233 and 20 < p[1] < 190 for p in (a, b))), EC, 'bezel front edge')
    # --- foot corners ---
    comp = made['Base tray']; mb = lambda: comp.bRepBodies.item(0)
    chamfer(comp, pick(mb(), lambda a, b: all((near(p[0], PI) or near(p[0], 244 - PI)) and near(p[3], 6) for p in (a, b)) and abs(a[4] - b[4]) > 5), 3.0, 'foot corners, front')
    chamfer(comp, pick(mb(), lambda a, b: all((near(p[0], PI) or near(p[0], 244 - PI)) and near(p[3], YR) for p in (a, b)) and abs(a[4] - b[4]) > 5), EC, 'foot corners, rear')      # the same as the plinth's above them
    for n in made:
        for b in made[n].bRepBodies: b.appearance = appearance(design, 'c%d_%d_%d' % COL[n], COL[n])
    o = rc.occurrences.addNewComponent(adsk.core.Matrix3D.create()); o.component.name = 'Power switch'
    add_bodies(o.component, switch_bodies(), design)
    o = rc.occurrences.addNewComponent(adsk.core.Matrix3D.create()); o.component.name = 'Screws'
    add_bodies(o.component, screws(), design)
    o = rc.occurrences.addNewComponent(adsk.core.Matrix3D.create()); o.component.name = 'Waveshare 10.3 e-Paper adapter'
    add_bodies(o.component, adapter_bodies(), design)
    o = rc.occurrences.addNewComponent(adsk.core.Matrix3D.create()); o.component.name = 'Panel foam tape'
    add_bodies(o.component, foam_bodies(), design)
    o = rc.occurrences.addNewComponent(adsk.core.Matrix3D.create()); o.component.name = 'Keycaps'
    add_bodies(o.component, [('Key %d' % (i + 1), keycap(u, KC_MARKS[i]), (226, 120, 60) if i == 3 else (150, 150, 150)) for i, u in enumerate(KCU)], design)
    o = rc.occurrences.addNewComponent(adsk.core.Matrix3D.create()); o.component.name = 'Knob'
    add_bodies(o.component, [('Knob', knob(), (150, 150, 150))], design)
    o = rc.occurrences.addNewComponent(adsk.core.Matrix3D.create()); o.component.name = 'Rubber feet'
    add_bodies(o.component, feet(), design)
    print('save', case.save('MK III case: part codes beside the revision marks, deeper groove in the bezel for the fold of the flex'))
