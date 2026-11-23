OUT = '/Users/kayden/Developer/PlatformIO/tesserae-device-bedside/cad/test-fits/'
def obox(u0, v0, w0, u1, v1, w1):
    c = F((u0+u1)/2, (v0+v1)/2, (w0+w1)/2)
    return tbm.createBox(adsk.core.OrientedBoundingBox3D.create(P(*c), adsk.core.Vector3D.create(*U_), adsk.core.Vector3D.create(*V_), abs(u1-u0)*MM, abs(v1-v0)*MM, abs(w1-w0)*MM))
def align(origin, fx, fy, fz):
    m = adsk.core.Matrix3D.create()
    m.setToAlignCoordinateSystems(P(*origin), adsk.core.Vector3D.create(*fx), adsk.core.Vector3D.create(*fy), adsk.core.Vector3D.create(*fz),
                                  adsk.core.Point3D.create(0, 0, 0), adsk.core.Vector3D.create(1, 0, 0), adsk.core.Vector3D.create(0, 1, 0), adsk.core.Vector3D.create(0, 0, 1))
    return m
X_, Y_, Z_ = (1, 0, 0), (0, 1, 0), (0, 0, 1)
PLATE = lambda: align(F(0, 0, 6), neg(U_), V_, W_)                 # chassis plate flat on the bed, posts up
def run(_context: str):
    case = doc_named('Bedside case'); case.activate()
    design = adsk.fusion.Design.cast(app.activeProduct); rc = design.rootComponent
    src = {n: [o for o in rc.occurrences if o.name.startswith(n)][0].component.bRepBodies.item(0) for n in ('Front bezel', 'Frame plate', 'Base tray', 'Rear shell', 'Plinth', 'Access cover')}
    FLAT = align((0, 0, 0), X_, Y_, Z_)                              # as it stands
    BACK = align(F(0, 0, 35.5), V_, U_, neg(W_))                     # rear shell on its back face
    ROOF = align((0, 0, 64), X_, neg(Y_), neg(Z_))                   # plinth on its roof
    spec = [  # name, source, region, lightening cuts, orientation for printing
        ('01-driver-hat-and-flex-adapter-posts', 'Frame plate', obox(154, 8, 6, 218, 81, 31), [obox(172, 40, 5, 203, 68, 9)], PLATE()),
        ('02-power-board-posts', 'Frame plate', obox(54, 8, 6, 132, 66, 19), [obox(67, 21, 5, 119, 53, 9)], PLATE()),
        ('03-key-module-fins', 'Frame plate', obox(147, 144, 6, 215, 179, 31), [], PLATE()),
        ('04-encoder-bracket', 'Frame plate', obox(223, 129, 6, 242, 171, 34), [], PLATE()),
        ('05-speaker-baffle', 'Base tray', box(14.9, 5, 2.9, 18.1, 79, 62.5), [], align((15, 0, 0), Y_, Z_, X_)),
        ('06-battery-cradle', 'Base tray', box(65, 43, 0, 124.5, 73.2, 22), [], FLAT),
        ('07-usb-c-port-and-switch', 'Base tray', box(81.5, 73.4, 30, 129, 84.5, 58.5), [], align((0, 0, 30), X_, Y_, Z_)),
        ('08-bezel-corner-glass-pocket', 'Front bezel', obox(-1, 140, -4, 56, 197, 7), [], align(F(0, 0, -3), neg(U_), V_, W_)),
        ('09-key-channel', 'Rear shell', obox(134, 188, 5, 228, 197, 36), [], align(F(0, 196, 0), neg(U_), W_, neg(V_))),
        ('10-knob-slot', 'Rear shell', obox(240, 132, 5, 245, 168, 36), [], align(F(244, 132, 5), V_, W_, neg(U_))),
        ('11a-foot-joint-frame-plate', 'Frame plate', box(120, 4, 2.5, 162, 40, 50), [], PLATE()),              # two gussets, one foot screw and two heel screws
        ('11b-foot-joint-base-tray', 'Base tray', box(120, 4, -3, 162, 40, 10.5), [], FLAT),
        ('12a-plinth-corner-and-post', 'Plinth', box(200, 71, 9, 236, 85, 30), [], ROOF),
        ('12b-plinth-corner-base-tray', 'Base tray', box(200, 71, -3, 236, 85, 10.5), [], FLAT),
        ('13a-access-cover', 'Access cover', box(100, 0, 50, 220, 120, 100), [], align((0, 0, 80), X_, neg(Y_), neg(Z_))),
        ('13b-access-cover-opening', 'Rear shell', obox(118, 52, 26, 205, 86, 36), [], BACK),
        ('14a-shell-corner-floor', 'Rear shell', INTER(box(-1, 0, 9, 22, 60, 34), obox(-5, -60, 5, 30, 80, 36)), [], BACK),
        ('14b-frame-plate-corner', 'Frame plate', box(2, 4, 2.5, 34, 40, 34), [], PLATE()),                      # the corner boss and the first gusset
    ]
    for o in [o for o in rc.occurrences if o.name.startswith('Test fit coupons')]: o.deleteMe()
    occ = rc.occurrences.addNewComponent(adsk.core.Matrix3D.create()); comp = occ.component; comp.name = 'Test fit coupons'
    bf = comp.features.baseFeatures.add(); bf.startEdit()
    x = 0.0; info = []
    for name, s, region, cuts, m in spec:
        b = tbm.copy(src[s])
        tbm.booleanOperation(b, region, adsk.fusion.BooleanTypes.IntersectionBooleanType)
        for cu in cuts: tbm.booleanOperation(b, cu, adsk.fusion.BooleanTypes.DifferenceBooleanType)
        tbm.transform(b, m)
        bb = b.boundingBox
        t = adsk.core.Matrix3D.create(); t.translation = adsk.core.Vector3D.create(x - bb.minPoint.x, -300*MM - bb.minPoint.y, -bb.minPoint.z)
        tbm.transform(b, t)
        size = [(bb.maxPoint.x - bb.minPoint.x)/MM, (bb.maxPoint.y - bb.minPoint.y)/MM, (bb.maxPoint.z - bb.minPoint.z)/MM]
        x += size[0]*MM + 1.0
        comp.bRepBodies.add(b, bf); info.append((name, size, b.volume))
    bf.finishEdit()
    em = design.exportManager
    for i, (name, size, vol) in enumerate(info):
        body = comp.bRepBodies.item(i); body.name = name
        body.appearance = appearance(design, 'c222_211_186', (222, 211, 186))
        opt = em.createSTLExportOptions(body, OUT + name + '.stl')
        opt.meshRefinement = adsk.fusion.MeshRefinementSettings.MeshRefinementHigh
        ok = em.execute(opt)
        print('%-40s %5.1f x %5.1f x %5.1f mm  %5.1f cm3  lumps %d  stl %s' % (name, size[0], size[1], size[2], vol, body.lumps.count, ok))
    print('row width mm', round(x/MM))
