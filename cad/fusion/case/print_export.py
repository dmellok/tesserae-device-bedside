# Exports the six printed parts of the visible (MK III) case as STL files, each already turned to its recommended print orientation and resting on Z = 0,
# centred on the origin. Run it with case_mk3.py loaded first, for the revision numbers. The orientations are the ones on the printing page (cad/viewer/printing.html): a turn about X, which runs across the case.
OUT = '/Users/kayden/Developer/PlatformIO/tesserae-device-bedside/cad/print/'
# Numbered in the order to print them: the three black parts first, then the three beige ones, so the filament changes once. Within each colour the part
# everything else is checked against comes first: the frame plate, then the tray that screws to its foot, then the plinth that slides over the tray.
PRINT = [('1-frame-plate-black', 'Frame plate', 105), ('2-base-tray-black', 'Base tray', 0), ('3-plinth-black', 'Plinth', 180), ('3-plinth-with-lugs-black', 'Plinth with lugs', 0),      # the plinth with lugs cannot lie on its roof, so it prints as it stands
         ('4-front-bezel-beige', 'Front bezel', 105), ('5-rear-shell-beige', 'Rear shell', -75), ('6-access-cover-beige', 'Access cover', 180)]
def run(_context: str):
    case = doc_named('Bedside case'); case.activate()
    design = adsk.fusion.Design.cast(app.activeProduct); rc = design.rootComponent
    for o in [o for o in rc.occurrences if o.name.startswith('Print parts')]: o.deleteMe()
    occ = rc.occurrences.addNewComponent(adsk.core.Matrix3D.create()); comp = occ.component; comp.name = 'Print parts'
    bf = comp.features.baseFeatures.add(); bf.startEdit()
    info = []
    for name, part, turn in PRINT:
        name += '-r%d' % REV[part]                      # the revision sunk into the part (REV in case_mk3.py), so a file can be matched to a print
        src = [o for o in rc.occurrences if o.name.startswith(part + ':')][0].component.bRepBodies.item(0)
        b = tbm.copy(src)
        m = adsk.core.Matrix3D.create(); m.setToRotation(math.radians(turn), adsk.core.Vector3D.create(1, 0, 0), adsk.core.Point3D.create(0, 0, 0))
        tbm.transform(b, m)
        bb = b.boundingBox
        t = adsk.core.Matrix3D.create(); t.translation = adsk.core.Vector3D.create(-(bb.minPoint.x + bb.maxPoint.x)/2, -(bb.minPoint.y + bb.maxPoint.y)/2, -bb.minPoint.z)
        tbm.transform(b, t)
        comp.bRepBodies.add(b, bf); info.append((name, [(bb.maxPoint.x - bb.minPoint.x)/MM, (bb.maxPoint.y - bb.minPoint.y)/MM, (bb.maxPoint.z - bb.minPoint.z)/MM], b.volume))
    bf.finishEdit()
    em = design.exportManager
    for i, (name, size, vol) in enumerate(info):
        body = comp.bRepBodies.item(i); body.name = name
        opt = em.createSTLExportOptions(body, OUT + name + '.stl')
        opt.meshRefinement = adsk.fusion.MeshRefinementSettings.MeshRefinementHigh
        ok = em.execute(opt)
        print('%-24s %5.1f x %5.1f x %5.1f mm  %5.1f cm3  lumps %d  stl %s' % (name, size[0], size[1], size[2], vol, body.lumps.count, ok))
    occ.deleteMe()
