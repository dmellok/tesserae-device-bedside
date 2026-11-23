# Print files, MK III case

Six STL files, numbered in the order to print them. Each is already turned to its recommended orientation and sits on Z = 0, so it can be dropped
straight onto the bed. Why each one goes that way up, and where it needs support, is on the printing page: https://turtle.gg/bedside/printing

| File | Colour | On the bed | Height |
|---|---|---|---|
| `1-frame-plate-black.stl` | Black | 239 x 199 mm | 31 mm |
| `2-base-tray-black.stl` | Black | 232 x 78 mm | 61 mm |
| `3-plinth-black.stl` | Black | 226 x 48 mm | 54 mm |
| `4-front-bezel-beige.stl` | Beige | 244 x 199 mm | 9 mm |
| `5-rear-shell-beige.stl` | Beige | 244 x 196 mm | 29 mm |
| `6-access-cover-beige.stl` | Beige | 66 x 34 mm | 16 mm |

The order keeps each colour together, so the filament changes once, and lets each part be checked against the one before it: the tray screws to
the frame plate's foot, the plinth slides over the tray, and the rear shell closes onto the bezel.

Print the small pieces in `../test-fits/` before any of these: they check the screw holes and board fits for a fraction of the plastic.

Regenerate with `../fusion/case/print_export.py` after any change to the case. None of these has been sliced or printed yet.
