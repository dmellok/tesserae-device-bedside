# Print files

## Keycaps

One file per keycap, for a Cherry MX switch, top face down as it prints. No supports. Each cap is longer in front of its post than behind it; the long
side faces the screen. The marks are sunk 0.5 mm into the top so the keys can be told apart by touch.

| File | Mark |
|---|---|
| `keycap-1-dot.stl`, `keycap-2-dots.stl`, `keycap-3-dots.stl` | The three grey keys, left to right from the front |
| `keycap-bar.stl` | The orange menu key, on the right |
| `keycap-4-dots.stl` | For the menu key, if you'd rather have dots on all four |
| `keycap-plain.stl` | No mark |

The viewer's "Key marks" option shows each arrangement. Print `../test-fits/15-keycap-stem-fit.stl` first: it has three stem sockets, marked with
one, two and three notches (tight, as modelled, loose). Push each onto a switch and say which one holds firmly without forcing; the keycaps use
the two-notch size.

### All of them on one plate

`keycaps-set-grey.stl` and `keycaps-set-orange.stl` are the same plate of 30 caps, one file for each filament: every mark above, and 24 caps with a
picture sunk into the top in place of a mark. The plate is 125 x 112 mm, about 27 cm3 of plastic, and needs no supports. Each cap is a separate
piece, so in the slicer the plate can be split into objects and any cap left out. Print the grey one for the three grey keys and the orange one for
the menu key, and fit whichever caps suit.

The pictures are Phosphor Bold icons, the set the screen draws with, 0.5 mm deep with lines 1.3 mm wide. In the slicer, seen from above, the
plate reads like this (the caps lie top down, so the pictures are on the underside and mirrored):

| Row | Left to right |
|---|---|
| 1 | plain, 1 dot, 2 dots, 3 dots, 4 dots, bar |
| 2 | caret up, caret down, check, back arrow, list (menu), house |
| 3 | minus, plus, play/pause, speaker, speaker muted, music notes |
| 4 | lightbulb, rain cloud, snowflake, fan, thermometer, power |
| 5 | moon, sun, alarm clock, bell, timer, gear |

`../renders/keycap-sets.png` shows the tops as they look on the keys. The smallest details, such as the glint in the lightbulb and the dot beside
the thermometer, are under 1 mm and may fill in.

`../fusion/case/keycap_plates.py` makes both files from the single keycap files and `tools/Phosphor-Bold.ttf`. It runs in plain Python, not in
Fusion, and needs fonttools, manifold3d and numpy; the rows and icons are a list at its top.

## Knob

`knob.stl` is the knob for the rotary encoder: 32 mm across, 12 mm long, straight knurl. It prints outer face down with no supports and pushes onto
the encoder's 6 mm D-shaft. The socket is 6.1 mm with its flat 0.05 mm clear of the shaft's; if it is loose or will not go on, say which.

## Case parts

The six case parts, each turned to the way it prints and numbered in the order to print them: black first, then beige.

| File | Part | Prints |
|---|---|---|
| `1-frame-plate-black-r4.stl` | Frame plate | Front face down |
| `2-base-tray-black-r1.stl` | Base tray | As it stands |
| `3-plinth-black-r2.stl` | Plinth | Roof down |
| `4-front-bezel-beige-r2.stl` | Front bezel | Front face down |
| `5-rear-shell-beige-r1.stl` | Rear shell | Back down |
| `6-access-cover-beige-r9.stl` | Access cover | Top down |

`../fusion/case/print_export.py` makes them (run after `case_mk3.py` in the same script).

Every case part has `MK3 R<n>` sunk into a face that is hidden once the case is together, and the same `r<n>` at the end of its file name.
Beside or under it is the part's code, `P1` to `P6`: the number its file name starts with. The number
goes up whenever that part changes in a way that affects how it fits or works, so a print can be matched to its file.

| Part | Where its mark is |
|---|---|
| Frame plate | Back face, in the middle, above the cable-tie anchors |
| Front bezel | Back of the left border |
| Base tray | Floor, right of the battery pad |
| Plinth | Underside of the roof, on the left |
| Rear shell | Inside of the back, left of centre |
| Access cover | Inside of the hump's back |

Front bezel revisions:

- R1: the first print.
- R2: the groove under the glass's bottom edge, where the panel's wide flex folds over, is 1 mm taller: it reaches 2.4 mm below the glass, not 1.4.

Frame plate revisions:

- R1 (no mark): the first print. The flex adapter's posts are too far from the flex slot for the panel's tail to reach.
- R2: the flex adapter sits on two rails just above the slot, 2 mm closer to the plate, and can be moved up in 3.5 mm steps. The gusset beside the
  speaker is shorter, so it clears the speaker's basket by 5 mm, not 0.7 mm. Its slot is too low: it sits behind the wide strip of the panel's
  flex, which cannot pass through it.
- R3: the slot is 9 mm higher and 11 mm tall, where the narrow tail leaves the wide strip, and the adapter's rails are 11 mm higher to match,
  with four places for the board. The wide strip stays between the glass and the plate, in a pocket 0.6 mm deep in the plate's front, and
  its capacitors sit in a window right through the plate. All placed from a photo of a panel, so print
  `../test-fits/16-flex-slot-fixture.stl` (slot and rails only) or `17-flex-relief-fixture.stl` (the relief as well), screw it to the bezel and try it on the panel before printing the whole plate.
- R4: the four holes in the power and amp board's posts are 1.8 mm, not 1.7, which was too tight for an M2 screw to cut its thread in. On an
  R3 plate, open those holes with a 1.8 mm drill instead of reprinting. `../test-fits/02-power-board-posts.stl` has the new holes.

Access cover revisions:

- R1: the first print. Loose: the barbs on its two tabs ended in front of the rear shell's wall and stood only 0.7 mm past the edges of the
  opening, so the cover rattled and fell off at a touch.
- R2: never printed. Stronger barbs on the same two tabs, which pulled off but still could not be reached to release them.
- R3: the tabs are gone. A slit under the roof frees the front of each side wall as a sprung strip, 1.5 mm thick and joined only at the back,
  with the hook on the inside of its front end. Squeeze the two sides of the cover near the front and both hooks leave the edges of the opening;
  the cover then lifts straight back. Each hook's back face is a 45 degree ramp that crosses the back of the shell's wall, so the wall's inner
  corner sits on it and pulls the cover onto the shell with no play. The hook stands 1.2 mm past the edge; unsqueezed, the cover takes about
  11 N to pull off in PLA. Printed top down with no support, the hooks tapering at 45 degrees. Still slightly loose, and it rode up off the
  plinth, because the tapered hooks left room under the top of the opening.
- R4: full-height prongs, splayed 3 degrees and printed on support. They would not clip behind the wall at all.
- R5, R6: never printed. Screwed down to the plinth through lugs, then flat tabs, on its sides, with sprung tabs inside as on R1.
- R7: R3 again, made tighter, with no screws. The sprung strips in the side walls are 1.8 mm thick instead of 1.5 mm, which makes them 1.7 times
  as stiff: about 19 N to pull the cover off unsqueezed, in PLA, against 11 N. The hooks' back ramps sit 0.6 mm onto the corner of the shell's
  wall instead of 0.55 mm. Two small rigid tongues under the front of the roof, one inside each hook and clear of the Pico's USB-C socket, reach
  into the top of the opening 0.4 mm under its edge, so the cover cannot ride up off the plinth. Squeeze the two sides near the front and lift
  it straight back, as on R3. Prints top down. Each strip's first layer is a 22 mm bridge to a pip across the front of its slit: cut or snap the
  two pips after printing, or the sides will not squeeze. The hooks still tapered at 45 degrees so as to print without support.
- R8: R7 with the prongs in front of the cover's face square and nearly the full height of the opening (0.5 mm under its top edge, with a small
  chamfer on the tip), for more contact with the opening's edges; inside the shell the right prong stops at z 75.5, short of the HAT's 13-pin
  header. Prints top down, on support under the two prongs and the two tongues, which start about 3 mm above the bed in front of the cover. The
  two pips across the slits still need cutting or snapping. `../test-fits/13a-access-cover.stl` is the same cover; print
  `13b-access-cover-opening.stl` to try its grip without the rear shell. As built, the hooks ran into the display driver HAT just inside
  the opening.
- R9: no hooks, strips, slits or tongues: nothing on the cover reaches through the opening. A low shoulder down each side stands on the
  plinth's roof and takes an M3 x 8 down into a boss under the roof, its head sunk in a recess like the other screws on the outside, so it needs
  plinth R2. The hump has thirteen grooves over its top and down its back, 2 mm wide at 4.5 mm like the slats on the rear shell and 1 mm deep;
  they are for looks and do not go through. Prints top down, on support under the two shoulders, which are 10.8 mm above the bed. (R9 was first
  drawn plain, with a flat tab on each side and the screw heads standing on it; the shoulders and grooves change its looks, not its fit, so the
  mark is still R9.) The cover no longer gives access to anything: the Pico sits facing down and its USB-C port cannot be reached
  with the case together, so flash it before assembly. `../test-fits/13a-access-cover.stl` is the same cover and
  `13b-access-cover-seat.stl` is the strip of the plinth's roof it screws to.

Plinth revisions:

- R1: the first print.
- R2: two bosses under the roof, one either side of the hump, for the access cover's screws.

The rear shell also has two pixel cats with whiskers, one sitting and one asleep, sunk 0.4 mm into the middle of the inside of its back. They are
hidden once the case is together. The access cover has a pixel turtle, 20 mm long, sunk 0.4 mm into the underside of its roof. Decoration like this
does not change a part's revision.

## Foam tape jig

`jig-foam-tape-frame.stl` is a flat frame (243 x 181 x 3.5 mm, 28 cm3, any colour; print it flat as exported, lips up, no support) for sticking
the panel's foam tape to the front of the frame plate. Lay the plate front face up and drop the frame onto it, numbers up, with its lips hooked over
the plate's left, right and top edges. There is a slot through the frame wherever a strip of foam goes: one down the left, two down the right
either side of the encoder's opening, and one across the top. Lay the tape in each slot, sticky side down, and cut it at each end of the slot,
where a groove across the frame guides the knife. The number sunk beside each slot is that strip's length in mm (155, 102, 21 and 118), for
cutting the tape beforehand instead. Lift the frame straight off afterwards.
`../fusion/case/tape_jig.py` makes it (run after `case_mk3.py` in the same script); the slots come from `FOAM` in `case_mk3.py`.

Regenerate the knob and keycaps with `../fusion/case/keycap_export.py` (run after `case_mk3.py` in the same script).
