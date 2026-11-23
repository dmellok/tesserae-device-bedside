S, C = math.sin(math.radians(15)), math.cos(math.radians(15))
U_, V_, W_ = (1, 0, 0), (0, S, C), (0, C, -S)          # frame axes in world: across, up the frame, into the frame
def neg(a): return (-a[0], -a[1], -a[2])
def F(u, v, w): return (u, w*C + v*S, 12 + v*C - w*S)   # frame point -> world, mm
def union_bbox(occ):
    lo = [1e9]*3; hi = [-1e9]*3
    for c in occ.childOccurrences:
        b = c.boundingBox
        if b.maxPoint.x - b.minPoint.x <= 0: continue
        lo = [min(lo[0], b.minPoint.x), min(lo[1], b.minPoint.y), min(lo[2], b.minPoint.z)]; hi = [max(hi[0], b.maxPoint.x), max(hi[1], b.maxPoint.y), max(hi[2], b.maxPoint.z)]
    return lo, hi
def local_bbox(occ, children):
    if children: lo, hi = union_bbox(occ)
    else:
        b = occ.boundingBox; lo = [b.minPoint.x, b.minPoint.y, b.minPoint.z]; hi = [b.maxPoint.x, b.maxPoint.y, b.maxPoint.z]
    inv = occ.transform2.copy(); inv.invert()
    l = [1e9]*3; h = [-1e9]*3
    for x in (lo[0], hi[0]):
        for y in (lo[1], hi[1]):
            for z in (lo[2], hi[2]):
                p = adsk.core.Point3D.create(x, y, z); p.transformBy(inv)
                l = [min(l[0], p.x), min(l[1], p.y), min(l[2], p.z)]; h = [max(h[0], p.x), max(h[1], p.y), max(h[2], p.z)]
    return l, h
def put(occ, axes, ref, target, children=False):
    # axes: world images of the part's own x, y, z. ref: a point in the part's own coordinates (mm), or 'min'/'max' per axis
    if isinstance(ref[0], str):
        l, h = local_bbox(occ, children)
        ref = [(l[i] if ref[i] == 'min' else h[i]) / MM for i in range(3)]
    M = adsk.core.Matrix3D.create()
    M.setWithCoordinateSystem(adsk.core.Point3D.create(0, 0, 0), adsk.core.Vector3D.create(*axes[0]), adsk.core.Vector3D.create(*axes[1]), adsk.core.Vector3D.create(*axes[2]))
    p = P(*ref); p.transformBy(M)
    M.translation = adsk.core.Vector3D.create(target[0]*MM - p.x, target[1]*MM - p.y, target[2]*MM - p.z)
    occ.transform2 = M
