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

## Knob

`knob.stl` is the knob for the rotary encoder: 32 mm across, 12 mm long, straight knurl. It prints outer face down with no supports and pushes onto
the encoder's 6 mm D-shaft. The socket is 6.1 mm with its flat 0.05 mm clear of the shaft's; if it is loose or will not go on, say which.

## Case parts

The six case parts, each turned to the way it prints and numbered in the order to print them: black first, then beige.

| File | Part | Prints |
|---|---|---|
| `1-frame-plate-black-r3.stl` | Frame plate | Front face down |
| `2-base-tray-black-r1.stl` | Base tray | As it stands |
| `3-plinth-black-r1.stl` | Plinth | Roof down |
| `4-front-bezel-beige-r2.stl` | Front bezel | Front face down |
| `5-rear-shell-beige-r1.stl` | Rear shell | Back down |
| `6-access-cover-beige-r4.stl` | Access cover | Top down |

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
- R4: the prong in front of the cover's face is the full height of the opening (0.25 mm under its top edge), so the cover cannot ride up; inside
  the shell the right prong stops at z 75.5, short of the HAT's 13-pin header. Both prongs are splayed 3 degrees outwards, which puts the barb
  0.3 mm further past the edge and the wall's corner 0.85 mm onto the ramp instead of 0.55 mm. Prints top down. Each strip's first layer is still a
  22 mm bridge to a pip across the front of its slit: cut or snap the two pips after printing, or the sides will not squeeze. The two prongs now
  need support from the bed; they start 2.75 mm up, outside the cover. `../test-fits/13a-access-cover.stl` is the same cover; print
  `13b-access-cover-opening.stl` to try its grip without the rear shell.

The rear shell also has two pixel cats with whiskers, one sitting and one asleep, sunk 0.4 mm into the middle of the inside of its back. They are
hidden once the case is together. The access cover has a pixel turtle, 20 mm long, sunk 0.4 mm into the underside of its roof. Decoration like this
does not change a part's revision.

## Foam tape jig

`jig-foam-tape-left.stl` and `jig-foam-tape-right.stl` are two small corner gauges (90 x 88 x 4.5 mm, any colour, flat as exported with the lips
up, no support) for sticking the panel's foam tape to the front of the frame plate. Each lies on the plate in a top corner with its lips hooked over
the plate's side and top edges. Butt the foam against its inside edges: the long arm is the outer edge of the strip down that side, the band along
the top is the top edge of the strip across the top and the top end of the side strips, and the small nub on the left jig is where the top strip
starts. Slide the jig down the side edge to guide the rest of a long strip. The lower strip on the right starts 1 mm below the encoder's opening.
`../fusion/case/tape_jig.py` makes them (run after `case_mk3.py` in the same script); the positions come from `FOAM` in `case_mk3.py`.

Regenerate the knob and keycaps with `../fusion/case/keycap_export.py` (run after `case_mk3.py` in the same script).
