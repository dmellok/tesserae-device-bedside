# merge_case.py old.json old.bin new.json new.bin out.json out.bin [component name ...]
# Keeps every component from the old export and takes only the case's own parts (printed parts, power switch, screws, rubber feet) from the new one.
# Fusion's display meshes and some appearance colours come back different after the design is reopened, so a fresh export of the components would change how they look.
import json, sys, re
oj, ob, nj, nb, outj, outb = sys.argv[1:7]
EXTRA = sys.argv[7:]          # components that really did move, to take from the new export as well
SHELL = ['Front bezel', 'Mid chassis and base', 'Base tray', 'Frame plate', 'Rear shell', 'Plinth', 'Access cover', 'Accent stripe', 'Power switch', 'Rubber feet', 'Panel foam tape']
own = lambda n: n in SHELL or n in EXTRA or re.search('screws', n, re.I)
old = json.load(open(oj)); new = json.load(open(nj)); bo = open(ob, 'rb').read(); bn = open(nb, 'rb').read()
parts = []; blob = bytearray()
for src, data, keep in ((old, bo, lambda n: not own(n)), (new, bn, own)):
    for p in src['parts']:
        if not keep(p['name']): continue
        q = dict(p)
        for k, n in (('pos', p['verts']*12), ('nrm', p['verts']*12), ('idx', p['tris']*12)):
            q[k] = len(blob); blob += data[p[k]:p[k] + n]
        parts.append(q)
old['parts'] = parts
json.dump(old, open(outj, 'w')); open(outb, 'wb').write(blob)
print(len(parts), 'parts', len(blob), 'bytes')
