# Case generator scripts

These build the printed case in the Fusion design "Bedside case". They are fragments, not add-ins: a full script is the
files below joined in order, then run through Fusion's scripting interface.

    import math; USB = None
    common.py  geom.py  frame.py  case_mk1.py   (MK I: the chamfered wedge, kept as hidden "MK I ..." components)
    common.py  geom.py  frame.py  case_mk2.py   (MK II: leaning slab, slatted hump and black plinth, kept as hidden "MK II ..." components)
    common.py  geom.py  frame.py  case_mk21.py  (MK II.I: MK II with a split base tray and frame plate, speaker grille, kept as hidden "MK II.I ..." components)
    common.py  geom.py  frame.py  case_mk3.py   (MK III: MK II.I with the plinth and tray 9 mm narrower each side so the frame floats; the visible case)
    common.py  geom.py  wiring.py               (every cable, as the top-level component "Wiring"; colours from docs/power-amp-board.html)
    common.py  geom.py  frame.py  coupons_mk3.py (cuts the 18 MK III test-fit pieces out of the case and writes their STLs to cad/test-fits; hide the "Test fit coupons" component again before exporting)
    common.py  export.py                        (writes cad/viewer/data; set ONLY = 'mk1', 'mk2' or 'mk21' to write a kept version's case parts)

After Fusion has been restarted, a fresh export gives the components coarser meshes and some different colours. `merge_case.py` takes only the case's own parts from a new export and keeps the components from the previous one.

MK III has no heat-set inserts: its M3 screws thread into 2.5 mm holes. The older versions still have 4 mm insert holes.

All four versions fit the same component layout, except that MK II.I and MK III have the battery 12 mm nearer the speaker (the viewer puts it back for the older two).
