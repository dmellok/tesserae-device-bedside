// Tesserae bedside: front bezel, panel stack and ribbed back plate (mock-up).
//
// Units are mm. z = 0 is the front face of the bezel and +z runs towards the
// back of the device, so every printed part is already in print orientation:
// the bezel face-down, the back plate skin-down.
//
// Stack, front to back:
//   bezel face (window, 45 deg chamfer) -> foam gasket on the ledge -> glass
//   -> foam sheet over the whole back -> back plate skin.
// The back plate lands on the pocket ring, a hard stop, so the screws can be
// tightened fully and the foam is always squeezed by exactly `squeeze`.
//
// Export one part at a time:
//   openscad -D 'part="bezel"' -o bezel.stl bezel.scad
//   openscad -D 'part="plate"' -o plate.stl bezel.scad
// Views: part = "assembly" | "exploded" | "section"

part = "exploded"; // [assembly, exploded, section, bezel, plate, gasket, foam, glass]
explode = 30;

/* [Panel: ED103TC2, E Ink / Waveshare spec] */
glass_w = 216.7;
glass_h = 174.4;
glass_t = 0.78;
aa_w = 209.66;
aa_h = 157.25;
// Borders from the glass edge to the active area. ASSUMED, measure your panel:
// the spec gives the outline and active area but not where one sits in the
// other. The FPC edge (bottom here) carries the wide border.
aa_left = (glass_w - aa_w) / 2;
aa_bottom = 13.65;
// The flex: width and centre offset from the window centre. MEASURE.
fpc_w = 50;
fpc_off = 0;
fpc_len = 30;

/* [Fit] */
clear = 0.4;          // glass to pocket wall, per side
window_margin = 0.75; // window larger than the active area, per side

/* [Bezel] */
face_t = 3.0;      // front skin
ring_t = 1.6;      // pocket wall
skirt_wall = 2.0;
skirt_depth = 25;  // behind the plate, room for the electronics
corner_r = 6;

/* [Stack] */
gasket_t = 0.5;  // adhesive foam on the ledge, uncompressed
foam_t = 1.5;    // foam sheet behind the glass, uncompressed
squeeze = 0.4;   // total compression of gasket + foam once screwed down

/* [Screws: M3 heat-set inserts] */
boss_d = 8;
insert_d = 4.0;
insert_depth = 6;
screw_clear = 3.4;
boss_off = 5;    // boss centre outside the pocket ring
boss_gap = 0.3;  // boss top short of the plate cap, so the ring is the stop

/* [Back plate] */
skin_t = 1.2;
rib_t = 1.2;
rib_d = 7;
rib_pitch = 25;
rim_w = 6;      // solid band over the clamp line
cap_t = 2;      // screw-head land on top of each tube
tube_wall = 1.6;
slot_len = 6;   // flex slot, along y
slot_from = 2.5; // flex runs flat this far past the glass before it bends

$fn = 48;

// ---- derived --------------------------------------------------------------
aa_top = glass_h - aa_h - aa_bottom;
ring_h = gasket_t + glass_t + foam_t - squeeze;
z_stop = face_t + ring_h;
plate_h = skin_t + rib_d;
boss_top = z_stop + plate_h - cap_t - boss_gap;
tube_bore = boss_d + 0.6;
tube_od = tube_bore + 2 * tube_wall;

// boxes are [x0, y0, w, h]; the window centre is the origin
function grow(b, d) = [b[0] - d, b[1] - d, b[2] + 2 * d, b[3] + 2 * d];
glass_b = [-aa_w / 2 - aa_left, -aa_h / 2 - aa_bottom, glass_w, glass_h];
aa_b = [-aa_w / 2, -aa_h / 2, aa_w, aa_h];
window_b = grow(aa_b, window_margin);
pocket_b = grow(glass_b, clear);
ring_b = grow(pocket_b, ring_t);
boss_ring = grow(ring_b, boss_off);
plate_b = grow(boss_ring, tube_od / 2);
// outer outline centred on the window so the borders look even from the front
need_x = max(-plate_b[0], plate_b[0] + plate_b[2]) + 0.5 + skirt_wall;
need_y = max(-plate_b[1], plate_b[1] + plate_b[3]) + 0.5 + skirt_wall;
outer_b = [-need_x, -need_y, 2 * need_x, 2 * need_y];
inner_b = grow(outer_b, -skirt_wall);

// slot for the flex, past the bottom edge of the glass
slot_b = [fpc_off - (fpc_w + 6) / 2, glass_b[1] - slot_from - slot_len,
          fpc_w + 6, slot_len];

bx0 = boss_ring[0]; by0 = boss_ring[1];
bx1 = bx0 + boss_ring[2]; by1 = by0 + boss_ring[3];
bosses = [
  [bx0, by0], [bx1, by0], [bx0, by1], [bx1, by1],  // corners
  [(bx0 + bx1) / 2, by1],                          // top middle
  [fpc_off - fpc_w / 2 - 16, by0],                 // either side of the flex
  [fpc_off + fpc_w / 2 + 16, by0],
];

echo(str("bezel outline ", outer_b[2], " x ", outer_b[3], " mm, depth ",
         z_stop + skirt_depth, " mm"));
echo(str("glass border: left/right ", aa_left, ", top ", aa_top,
         ", bottom ", aa_bottom));
echo(str("pocket ring ", ring_h, " mm tall; plate lands at z = ", z_stop));

// ---- helpers --------------------------------------------------------------
module rect(b) translate([b[0], b[1]]) square([b[2], b[3]]);
module rrect(b, r) translate([b[0] + r, b[1] + r])
  offset(r = r) square([b[2] - 2 * r, b[3] - 2 * r]);
module slab(b, z, h) translate([0, 0, z]) linear_extrude(h) rect(b);

// ---- bezel ----------------------------------------------------------------
module bezel() {
  difference() {
    union() {
      linear_extrude(face_t) rrect(outer_b, corner_r);
      linear_extrude(z_stop + skirt_depth) difference() {
        rrect(outer_b, corner_r);
        rrect(inner_b, corner_r - skirt_wall);
      }
      translate([0, 0, face_t]) linear_extrude(ring_h) difference() {
        rect(ring_b);
        rect(pocket_b);
      }
      for (p = bosses) translate(p) cylinder(d = boss_d, h = boss_top);
    }
    // window, chamfered 45 deg towards the front so the edge casts no shadow
    hull() {
      slab(window_b, face_t, 0.02);
      slab(grow(window_b, face_t + 0.02), -0.02, 0.02);
    }
    // corner reliefs so the sharp glass corners never bear on the wall
    for (x = [pocket_b[0], pocket_b[0] + pocket_b[2]],
         y = [pocket_b[1], pocket_b[1] + pocket_b[3]])
      translate([x, y, face_t - 0.01]) cylinder(d = 2, h = ring_h + 1);
    // the flex leaves through a gap in the ring
    slab([fpc_off - (fpc_w + 4) / 2, ring_b[1] - 1, fpc_w + 4, ring_t + 2],
         face_t - 0.01, ring_h + 1);
    for (p = bosses)
      translate([p[0], p[1], boss_top - insert_depth])
        cylinder(d = insert_d, h = insert_depth + 1);
  }
}

// ---- back plate (local z = 0 is the skin face that touches the foam) -------
module plate() {
  difference() {
    union() {
      linear_extrude(skin_t) rect(plate_b);
      // rib grid
      intersection() {
        linear_extrude(plate_h) rect(plate_b);
        union() {
          for (x = [plate_b[0] : rib_pitch : plate_b[0] + plate_b[2]])
            translate([x - rib_t / 2, plate_b[1], 0])
              cube([rib_t, plate_b[3], plate_h]);
          for (y = [plate_b[1] : rib_pitch : plate_b[1] + plate_b[3]])
            translate([plate_b[0], y - rib_t / 2, 0])
              cube([plate_b[2], rib_t, plate_h]);
        }
      }
      // edge ribs and a solid band over the clamp line
      linear_extrude(plate_h) difference() {
        rect(plate_b);
        rect(grow(plate_b, -rib_t));
      }
      linear_extrude(plate_h) difference() {
        rect(grow(ring_b, 1));
        rect(grow(pocket_b, -rim_w));
      }
      // ribs round the flex slot
      linear_extrude(plate_h) difference() {
        rect(grow(slot_b, rib_t + 1));
        rect(slot_b);
      }
      for (p = bosses) translate(p) cylinder(d = tube_od, h = plate_h);
    }
    for (p = bosses) translate(p) {
      translate([0, 0, -0.01]) cylinder(d = tube_bore, h = plate_h - cap_t);
      cylinder(d = screw_clear, h = plate_h + 1);
    }
    slab(slot_b, -0.01, plate_h + 1);
  }
}

// ---- soft parts and the panel ---------------------------------------------
module gasket(t = gasket_t) linear_extrude(t) difference() {
  rect(grow(glass_b, -0.3));
  rect(grow(window_b, 0.3));
}

module foam(t = foam_t) linear_extrude(t) rect(grow(glass_b, -0.5));

module glass() {
  color("#ecebe6") linear_extrude(glass_t) rect(glass_b);
  color("#bdbab2") translate([0, 0, -0.01]) linear_extrude(0.02) rect(aa_b);
  // flex: flat past the glass edge, then folded up through the plate slot
  fy = glass_b[1] - slot_from - slot_len / 2;
  color("#c98a2b") {
    translate([fpc_off - fpc_w / 2, fy, 0.2])
      cube([fpc_w, glass_b[1] - fy + 4, 0.15]);
    translate([fpc_off - fpc_w / 2, fy, 0.2]) cube([fpc_w, 0.15, fpc_len]);
  }
}

// ---- views ----------------------------------------------------------------
module stack(e) {
  sq_g = squeeze * gasket_t / (gasket_t + foam_t);
  sq_f = squeeze - sq_g;
  gt = e > 0 ? gasket_t : gasket_t - sq_g;
  ft = e > 0 ? foam_t : foam_t - sq_f;
  color("#34373c") bezel();
  color("#d8a03a") translate([0, 0, face_t + e]) gasket(gt);
  translate([0, 0, face_t + gt + 2 * e]) glass();
  color("#3b78a8") translate([0, 0, face_t + gt + glass_t + 3 * e]) foam(ft);
  color("#7d8794") translate([0, 0, z_stop + 4 * e]) plate();
}

if (part == "assembly") stack(0);
else if (part == "exploded") stack(explode);
else if (part == "section") difference() {
  stack(0);
  // cut through the flex slot, keep x < fpc_off
  translate([fpc_off, -500, -50]) cube([1000, 1000, 200]);
}
else if (part == "bezel") bezel();
else if (part == "plate") plate();
else if (part == "gasket") gasket();
else if (part == "foam") foam();
else if (part == "glass") glass();
