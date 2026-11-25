# Foam tape jig for the MK III frame plate: run after case_mk3.py in the same script. Writes two STLs, a left-hand and a right-hand corner gauge.
# Each is an L that lies on the front of the plate in a top corner, with a lip that hooks over the plate's side edge and another over its top edge.
# Its inside edges are where the foam goes: the long arm is the outer edge of the strip down that side, the short band is the top edge of the strip
# across the top (and the top end of the side strips), and a small nub is where the top strip starts. Slid down the side edge, the long arm also
# spaces the lower strips. The positions come from FOAM in case_mk3.py, so the jig follows the model.
JIG_OUT = '/Users/kayden/Developer/PlatformIO/tesserae-device-bedside/cad/print/'
JT, JLIP, JLW, JC = 3.0, 1.5, 2.0, 0.2          # body thickness, how far the lips reach down the plate's 1.95 mm edge, their width, and the clearance to the edge
JARM, JBAND = 80.0, 90.0                        # how far the arm runs down the side, and the band along the top
PE_U, PE_V, PW = 2.7, 193.0, 6.05               # the plate's left edge, its top edge, and its front face (w)
def tape_jig(right):
    fu, fv, tu = FOAM[0][0], FOAM[0][3], FOAM[3][0]                      # outer edge of the left strip, top of the strips, where the top strip starts
    u_out, v_out = PE_U - JC - JLW, PE_V + JC + JLW
    w0, w1 = PW - JT, PW + JLIP
    parts = [obox(u_out, fv - JARM, w0, fu, v_out, PW),                  # the arm down the side
             obox(u_out, fv, w0, JBAND, v_out, PW),                      # the band along the top
             obox(FOAM[0][2], fv - 3.0, w0, tu, fv, PW),                 # the nub between the side strip and the start of the top strip
             obox(u_out, fv - JARM, w0, PE_U - JC, v_out, w1),           # lip over the side edge
             obox(u_out, PE_V + JC, w0, JBAND, v_out, w1)]               # lip over the top edge
    b = U(parts)
    if right:                                                             # the plate is the same either side of u = 122
        m = adsk.core.Matrix3D.create(); o = F(122, 0, 0)
        m.setToAlignCoordinateSystems(P(*o), adsk.core.Vector3D.create(*U_), adsk.core.Vector3D.create(*V_), adsk.core.Vector3D.create(*W_),
                                      P(*o), adsk.core.Vector3D.create(*neg(U_)), adsk.core.Vector3D.create(*V_), adsk.core.Vector3D.create(*W_))
        tbm.transform(b, m)
    return b
def run(_context: str):
    case = doc_named('Bedside case'); case.activate()
    design = adsk.fusion.Design.cast(app.activeProduct); rc = design.rootComponent
    plate_b = [o for o in rc.occurrences if o.name.startswith('Frame plate')][0].component.bRepBodies.item(0)
    occ = rc.occurrences.addNewComponent(adsk.core.Matrix3D.create()); comp = occ.component; comp.name = 'Tape jig (temporary)'
    bf = comp.features.baseFeatures.add(); bf.startEdit(); info = []
    for name, right in (('jig-foam-tape-left', False), ('jig-foam-tape-right', True)):
        b = tape_jig(right)
        c = tbm.copy(b); tbm.booleanOperation(c, tbm.copy(plate_b), adsk.fusion.BooleanTypes.IntersectionBooleanType)
        clash = c.volume*1000 if c.lumps.count else 0.0
        m = adsk.core.Matrix3D.create()                                   # face down on the bed, lips up
        m.setToAlignCoordinateSystems(P(*F(0, 0, PW - JT)), adsk.core.Vector3D.create(*neg(U_)), adsk.core.Vector3D.create(*V_), adsk.core.Vector3D.create(*W_),
                                      adsk.core.Point3D.create(0, 0, 0), adsk.core.Vector3D.create(1, 0, 0), adsk.core.Vector3D.create(0, 1, 0), adsk.core.Vector3D.create(0, 0, 1))
        tbm.transform(b, m); bb = b.boundingBox
        size = [(bb.maxPoint.asArray()[k] - bb.minPoint.asArray()[k])/MM for k in range(3)]
        comp.bRepBodies.add(b, bf); info.append((name, size, b.volume, clash, bb.minPoint.z/MM))
    bf.finishEdit()
    em = design.exportManager
    for i, (name, size, vol, clash, zmin) in enumerate(info):
        body = comp.bRepBodies.item(i)
        opt = em.createSTLExportOptions(body, JIG_OUT + name + '.stl'); opt.meshRefinement = adsk.fusion.MeshRefinementSettings.MeshRefinementHigh
        ok = em.execute(opt)
        print('%-22s %5.1f x %5.1f x %4.1f mm  %4.1f cm3  lumps %d  overlap with the plate %.2f mm3  bed z %.2f  stl %s' % (name, size[0], size[1], size[2], vol, body.lumps.count, clash, zmin, ok))
    occ.deleteMe()
