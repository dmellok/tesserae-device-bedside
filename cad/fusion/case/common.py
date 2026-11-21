import adsk.core, adsk.fusion, math
app = adsk.core.Application.get()
MM = 0.1
def P(x, y, z=0.0): return adsk.core.Point3D.create(x*MM, y*MM, z*MM)
def V(x, y, z=0.0): return adsk.core.Vector3D.create(x*MM, y*MM, z*MM)
def appearance(design, name, rgb):
    a = design.appearances.itemByName(name)
    if a: return a
    lib = app.materialLibraries.itemByName('Fusion Appearance Library')
    src = None
    for pref in ('Plastic - Matte (Black)', 'Plastic - Glossy (Black)', 'Plastic'):
        for i in range(lib.appearances.count):
            ap = lib.appearances.item(i)
            if ap.name.startswith(pref): src = ap; break
        if src: break
    a = design.appearances.addByCopy(src, name)
    for i in range(a.appearanceProperties.count):
        p = a.appearanceProperties.item(i)
        if isinstance(p, adsk.core.ColorProperty) and p.value is not None:
            p.value = adsk.core.Color.create(rgb[0], rgb[1], rgb[2], 255); break
    return a
def doc_named(prefix):
    for d in [app.documents.item(i) for i in range(app.documents.count)]:
        if d.name.startswith(prefix): return d
def folder():
    p = [app.data.dataProjects.item(i) for i in range(app.data.dataProjects.count) if app.data.dataProjects.item(i).name == 'Default Project'][0]
    return p, p.rootFolder.dataFolders.itemByName('Bedside components')
