# Foam tape jig for the MK III frame plate: run after case_mk3.py in the same script. Writes one STL, a flat frame that lies on the front of the plate
# with lips hooked over the plate's left, right and top edges. A slot runs through the frame wherever a strip of foam goes: lay the tape in the slot,
# sticky side down, and cut it at each end of the slot, where a groove across the frame guides the knife. The number sunk beside each slot is the
# length of that strip in mm, for cutting the tape beforehand instead. Lift the frame straight off afterwards. The slots come from FOAM in case_mk3.py,
# so the jig follows the model.
JIG_OUT = '/Users/kayden/Developer/PlatformIO/tesserae-device-bedside/cad/print/'
JT, JLIP, JLW, JC = 2.0, 1.5, 2.0, 0.2          # body thickness (the foam is 2 mm), how far the lips reach down the plate's 1.95 mm edge, their width, and the clearance to the edge
JS, JG = 0.4, (1.0, 1.2, 3.0)                   # clearance each side of the tape in a slot; the knife grooves: width, depth, and how far they run past the slot on each side
PE_U, PE_V, PW = (2.7, 241.3), 193.0, 6.05      # the plate's left and right edges, its top edge, and its front face (w)
JB = (24.0, 220.0, 14.0, 26.0, 171.0)           # the frame's bands: inner edge of the left and of the right one, the bottom band (v), and the lower edge of the top one
def tape_jig():
    u0, u1, v1 = PE_U[0] - JC - JLW, PE_U[1] + JC + JLW, PE_V + JC + JLW
    w0, w1 = PW - JT, PW + JLIP
    b = U([obox(u0, JB[2], w0, JB[0], v1, PW), obox(JB[1], JB[2], w0, u1, v1, PW), obox(u0, JB[2], w0, u1, JB[3], PW), obox(u0, JB[4], w0, u1, v1, PW),
           obox(u0, JB[2], w0, PE_U[0] - JC, v1, w1), obox(PE_U[1] + JC, JB[2], w0, u1, v1, w1), obox(u0, PE_V + JC, w0, u1, v1, w1)])      # four bands, then the lips over the left, right and top edges
    cut = []
    for fu0, fv0, fu1, fv1 in FOAM:
        up = (fv1 - fv0) > (fu1 - fu0)                                    # a strip that runs up the plate, or across it
        if up:
            cut.append(obox(fu0 - JS, fv0, w0 - 1, fu1 + JS, fv1, PW + 1))
            cut += [obox(fu0 - JG[2], v - JG[0]/2, w0 - 1, fu1 + JG[2], v + JG[0]/2, w0 + JG[1]) for v in (fv0, fv1)]
        else:
            cut.append(obox(fu0, fv0 - JS, w0 - 1, fu1, fv1 + JS, PW + 1))
            cut += [obox(u - JG[0]/2, fv0 - JG[2], w0 - 1, u + JG[0]/2, fv1 + JG[2], w0 + JG[1]) for u in (fu0, fu1)]
    # the length of each strip, sunk into the front face beside its slot; the three beside the upright slots read up the frame
    o = lambda u, v: F(u, v, w0)
    nU = neg(U_)
    cut += stamp('%d' % round(FOAM[0][3] - FOAM[0][1]), o(10.8, 100), V_, nU) + stamp('%d' % round(FOAM[1][3] - FOAM[1][1]), o(239.3, 75), V_, nU)
    cut += stamp('%d' % round(FOAM[2][3] - FOAM[2][1]), o(239.3, 171.5), V_, nU) + stamp('%d' % round(FOAM[3][2] - FOAM[3][0]), o(72, 173.0), U_, V_)
    return SUB(b, cut)
def run(_context: str):
    case = doc_named('Bedside case'); case.activate()
    design = adsk.fusion.Design.cast(app.activeProduct); rc = design.rootComponent
    plate_b = [o for o in rc.occurrences if o.name.startswith('Frame plate')][0].component.bRepBodies.item(0)
    occ = rc.occurrences.addNewComponent(adsk.core.Matrix3D.create()); comp = occ.component; comp.name = 'Tape jig (temporary)'
    bf = comp.features.baseFeatures.add(); bf.startEdit()
    b = tape_jig()
    c = tbm.copy(b); tbm.booleanOperation(c, tbm.copy(plate_b), adsk.fusion.BooleanTypes.IntersectionBooleanType)
    clash = c.volume*1000 if c.lumps.count else 0.0
    m = adsk.core.Matrix3D.create()                                       # face down on the bed, lips up
    m.setToAlignCoordinateSystems(P(*F(0, 0, PW - JT)), adsk.core.Vector3D.create(*neg(U_)), adsk.core.Vector3D.create(*V_), adsk.core.Vector3D.create(*W_),
                                  adsk.core.Point3D.create(0, 0, 0), adsk.core.Vector3D.create(1, 0, 0), adsk.core.Vector3D.create(0, 1, 0), adsk.core.Vector3D.create(0, 0, 1))
    tbm.transform(b, m); bb = b.boundingBox
    size = [(bb.maxPoint.asArray()[k] - bb.minPoint.asArray()[k])/MM for k in range(3)]
    comp.bRepBodies.add(b, bf); bf.finishEdit()
    body = comp.bRepBodies.item(0); em = design.exportManager
    opt = em.createSTLExportOptions(body, JIG_OUT + 'jig-foam-tape-frame.stl'); opt.meshRefinement = adsk.fusion.MeshRefinementSettings.MeshRefinementHigh
    ok = em.execute(opt)
    print('jig-foam-tape-frame  %.1f x %.1f x %.1f mm  %.1f cm3  lumps %d  overlap with the plate %.2f mm3  bed z %.2f  stl %s' % (size[0], size[1], size[2], body.volume, body.lumps.count, clash, bb.minPoint.z/MM, ok))
    occ.deleteMe()
