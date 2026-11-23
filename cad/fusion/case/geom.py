tbm = adsk.fusion.TemporaryBRepManager.get()
def box(x0, y0, z0, x1, y1, z1):
    obb = adsk.core.OrientedBoundingBox3D.create(P((x0+x1)/2, (y0+y1)/2, (z0+z1)/2), adsk.core.Vector3D.create(1,0,0), adsk.core.Vector3D.create(0,1,0),
                                                 abs(x1-x0)*MM, abs(y1-y0)*MM, abs(z1-z0)*MM)
    return tbm.createBox(obb)
def cyl(a, b, r): return tbm.createCylinderOrCone(P(*a), r*MM, P(*b), r*MM)
def cone(a, ra, b, rb): return tbm.createCylinderOrCone(P(*a), ra*MM, P(*b), rb*MM)
def sphere(c, r): return tbm.createSphere(P(*c), r*MM)
def U(bs):
    t = bs[0]
    for b in bs[1:]: tbm.booleanOperation(t, b, adsk.fusion.BooleanTypes.UnionBooleanType)
    return t
def SUB(t, tools):
    for b in tools: tbm.booleanOperation(t, b, adsk.fusion.BooleanTypes.DifferenceBooleanType)
    return t
def INTER(t, b):
    tbm.booleanOperation(t, b, adsk.fusion.BooleanTypes.IntersectionBooleanType); return t
def header_pins(xs, ys, z0, z1):
    return U([box(x-0.32, y-0.32, z0, x+0.32, y+0.32, z1) for x in xs for y in ys])
def add_bodies(comp, items, design):
    bf = comp.features.baseFeatures.add(); bf.startEdit()
    for n, b, rgb in items: comp.bRepBodies.add(b, bf)
    bf.finishEdit()
    bodies = [comp.bRepBodies.item(i) for i in range(comp.bRepBodies.count)][-len(items):]
    for (n, b, rgb), body in zip(items, bodies):
        body.name = n; body.appearance = appearance(design, 'c%d_%d_%d' % rgb, rgb)
def new_part_doc(fold, name, desc, items):
    doc = app.documents.add(adsk.core.DocumentTypes.FusionDesignDocumentType)
    design = adsk.fusion.Design.cast(app.activeProduct)
    design.designType = adsk.fusion.DesignTypes.ParametricDesignType
    add_bodies(design.rootComponent, items, design)
    bb = design.rootComponent.boundingBox
    print('%-34s bbox mm' % name, [round(v*10, 2) for v in (bb.minPoint.x, bb.minPoint.y, bb.minPoint.z, bb.maxPoint.x, bb.maxPoint.y, bb.maxPoint.z)], 'saveAs', doc.saveAs(name, fold, desc, ''))
