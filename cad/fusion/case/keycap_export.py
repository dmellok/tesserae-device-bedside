# Exports the printed keycap, top face down as it prints, and a small test piece with three stem sockets of different tightness.
# Run after case_mk3.py in the same script, so the keycap's dimensions come from there.
KOUT = '/Users/kayden/Developer/PlatformIO/tesserae-device-bedside/cad/'
STEM_TEST = [(4.15, 1.25, 4.35), (4.25, 1.35, 4.45), (4.35, 1.45, 4.55)]      # tight, as modelled, loose: marked with one, two and three notches
def stem_test():
    zp = KC_TOP - KC_SEAT
    base = box(-17, -5, 0, 17, 5, KC_ROOF)
    posts, cuts = [], []
    for i, (a, t, b) in enumerate(STEM_TEST):
        x = -11 + 11*i
        posts.append(cyl((x, 0, KC_ROOF - 0.1), (x, 0, zp), KC_POST))
        cuts += [box(x - a/2, -t/2, zp - KC_SOCK, x + a/2, t/2, zp + 0.1), box(x - t/2, -b/2, zp - KC_SOCK, x + t/2, b/2, zp + 0.1)]
        cuts += [box(x - 1.5 + 1.5*k - 0.4, -5.1, -0.1, x - 1.5 + 1.5*k + 0.4, -4.0, KC_ROOF + 0.1) for k in range(i + 1)]
    return SUB(U([base] + posts), cuts)
def run(_context: str):
    case = doc_named('Bedside case'); case.activate()
    design = adsk.fusion.Design.cast(app.activeProduct); rc = design.rootComponent
    occ = rc.occurrences.addNewComponent(adsk.core.Matrix3D.create()); comp = occ.component; comp.name = 'Keycap export'
    bf = comp.features.baseFeatures.add(); bf.startEdit()
    items = [('print/keycap', keycap_flat()), ('test-fits/15-keycap-stem-fit', stem_test())]
    for name, b in items: comp.bRepBodies.add(b, bf)
    bf.finishEdit()
    em = design.exportManager
    for i, (name, _) in enumerate(items):
        body = comp.bRepBodies.item(i); bb = body.boundingBox
        opt = em.createSTLExportOptions(body, KOUT + name + '.stl')
        opt.meshRefinement = adsk.fusion.MeshRefinementSettings.MeshRefinementHigh
        print('%-30s %.1f x %.1f x %.1f mm  %.2f cm3  lumps %d  stl %s' % (name, (bb.maxPoint.x - bb.minPoint.x)/MM, (bb.maxPoint.y - bb.minPoint.y)/MM, (bb.maxPoint.z - bb.minPoint.z)/MM, body.volume, body.lumps.count, em.execute(opt)))
    occ.deleteMe()
