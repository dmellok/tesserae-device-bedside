# Case generator scripts

These build the printed case in the Fusion design "Bedside case". They are fragments, not add-ins: a full script is the
files below joined in order, then run through Fusion's scripting interface.

    import math; USB = None
    common.py  geom.py  frame.py  case_mk1.py   (MK I: the chamfered wedge, kept as hidden "MK I ..." components)
    common.py  geom.py  frame.py  case_mk2.py   (MK II: leaning slab, slatted hump and black plinth; the visible case)
    common.py  export.py                        (writes cad/viewer/data; set ONLY = 'mk1' to write the MK I case parts)

Both versions fit the same component layout, so either can be rebuilt without moving anything.
