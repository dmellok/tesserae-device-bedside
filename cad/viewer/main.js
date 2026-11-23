import * as THREE from 'three';
import { OrbitControls } from './vendor/OrbitControls.js';

const track = (name, props) => { if (window.track) window.track(name, props); };

// The model is exported from Fusion in millimetres with Z up (tools: cad/viewer/data/bedside.json + .bin).
// Each part carries two offsets: e1 for the case explode, e2 for the component explode.

const canvas = document.getElementById('scene');
const renderer = new THREE.WebGLRenderer({ canvas, antialias: true, alpha: true });
renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
renderer.outputColorSpace = THREE.SRGBColorSpace;

const scene = new THREE.Scene();
const camera = new THREE.PerspectiveCamera(32, 1, 5, 6000);
camera.up.set(0, 0, 1);

const CENTRE = new THREE.Vector3(122, 42, 100);
const VIEWS = {
  Front: [330, -560, 190],
  Rear: [-520, 520, 250],
  Side: [700, 40, 120],
  Top: [0, -120, 760],
};
camera.position.set(...VIEWS.Front).add(CENTRE);

const controls = new OrbitControls(camera, canvas);
controls.target.copy(CENTRE);
controls.enableDamping = true;
controls.dampingFactor = 0.08;
controls.minDistance = 120;
controls.maxDistance = 2200;
controls.update();

scene.add(new THREE.HemisphereLight(0xffffff, 0x3a3f4d, 1.5));
const key = new THREE.DirectionalLight(0xffffff, 1.9); key.position.set(260, -520, 640); scene.add(key);
const fill = new THREE.DirectionalLight(0xbfd0ff, 0.75); fill.position.set(-420, 480, 240); scene.add(fill);

const model = new THREE.Group();
scene.add(model);
const parts = [];              // { mesh, e1, e2, name }
const state = { explode: 0, parts: 0, spin: 0, hidden: new Set(), design: 'mk2', bg: 'Auto' };
const shown = { explode: 0, parts: 0 };

// Four versions of the case share one set of components. MK I is the chamfered wedge, MK II the leaning slab on a black plinth,
// and MK II.I is MK II reworked for printing: split chassis, fit gaps, vented slats, a speaker grille, a braced frame plate and the battery moved clear of the GPIO plug.
// MK III is MK II.I with the plinth and base tray 9 mm narrower on each side, so the frame overhangs them and reads as floating,
// no heat-set inserts, and the USB-C and switch panel lined up with the hump.
const DESIGNS = { 'MK I': 'mk1', 'MK II': 'mk2', 'MK II.I': 'mk21', 'MK III': 'mk3' };
const SPLIT = new Set(['mk21', 'mk3']);          // the versions with a separate base tray and frame plate, and the battery nearer the speaker
const MK_HASH = { mk1: '1', mk2: '2', mk21: '2.1', mk3: '3' };
const CASE_PARTS = ['Front bezel', 'Rear shell', 'Plinth', 'Access cover'];
const NOT_MK1 = ['Plinth', 'Access cover'];          // case toggles that mean nothing on MK I
const isScrew = (name) => /screws/i.test(name);
// Case finishes. Beige is the two-tone case as modelled: beige frame, black chassis and plinth. Black gets a light backdrop so the shape still reads.
const SHELL = ['Front bezel', 'Mid chassis and base', 'Base tray', 'Frame plate', 'Rear shell', 'Plinth', 'Access cover'];
// All beige is the frame's beige on every printed part.
const FINISHES = { Beige: { colour: null, light: false }, 'All beige': { colour: '#ded3ba', light: false }, Black: { colour: '#1b1c1f', light: true } };
function finishColour(f, base) {
  if (f.colour) return new THREE.Color(f.colour);
  return base;
}
// Backdrops are plain CSS behind the transparent canvas (see html[data-bg] in index.html). Auto picks Night for the beige case and Studio for the black one.
const BACKDROPS = { Auto: null, Night: { light: false, theme: '#0b0d12' }, Studio: { light: true, theme: '#f4f5f7' }, Paper: { light: true, theme: '#fbf8f2' },
  Sky: { light: true, theme: '#bfe0f6' }, Blueprint: { light: false, theme: '#16407c' }, Dusk: { light: false, theme: '#4a2459' } };
let lightBackdrop = false;
function applyBackdrop() {
  const name = state.bg === 'Auto' ? ((FINISHES[state.finish] || FINISHES.Beige).light ? 'Studio' : 'Night') : state.bg;
  const b = BACKDROPS[name];
  lightBackdrop = b.light;
  document.documentElement.dataset.bg = name.toLowerCase();
  const under = $('bdA'), over = $('bdB');           // two stacked layers, so a change of backdrop fades instead of snapping
  if (over.dataset.bg !== name.toLowerCase()) {
    under.dataset.bg = over.dataset.bg || name.toLowerCase();
    over.style.transition = 'none'; over.style.opacity = 0; void over.offsetWidth;
    over.dataset.bg = name.toLowerCase();
    over.style.transition = matchMedia('(prefers-reduced-motion: reduce)').matches ? 'none' : 'opacity .9s ease'; over.style.opacity = 1;
  }
  document.documentElement.classList.toggle('light', b.light);
  document.querySelector('meta[name=theme-color]').content = b.theme;
  paintWire();
}
function setBackdrop(name, save = true) {
  state.bg = Object.keys(BACKDROPS).find((k) => k.toLowerCase() === String(name || '').toLowerCase()) || 'Auto';
  for (const b of $('backdrop').children) b.setAttribute('aria-pressed', String(b.textContent === state.bg));
  if (save) try { localStorage.setItem('bedside-backdrop', state.bg); } catch (e) { /* storage blocked */ }
  applyBackdrop();
}
function setFinish(name, save = true) {
  const f = FINISHES[name] || FINISHES.Beige;
  state.finish = FINISHES[name] ? name : 'Beige';
  for (const p of parts) {
    if (p.screen || !SHELL.includes(p.name)) continue;
    p.mesh.material.color.copy(finishColour(f, p.mesh.userData.base));
    p.mesh.material.roughness = f.colour ? 0.62 : 0.72;
  }
  applyBackdrop();
  for (const b of $('finish').children) b.setAttribute('aria-pressed', String(b.textContent === state.finish));
  if (save) try { localStorage.setItem('bedside-finish', state.finish); } catch (e) { /* storage blocked */ }
  if (hovered) { const h = hovered; hovered = null; setHover(h); }
}

async function load() {
  // The .json holds offsets into the .bin, so a browser must never pair an old copy of one with a new copy of the other: change DATA_V with every export.
  const DATA_V = '20261124b';
  const get = (stem) => [fetch(`./data/${stem}.json?v=${DATA_V}`).then((r) => r.json()), fetch(`./data/${stem}.bin?v=${DATA_V}`).then((r) => r.arrayBuffer())];
  const [meta, bin, meta1, bin1, meta2, bin2, meta21, bin21] = await Promise.all([...get('bedside'), ...get('mk1'), ...get('mk2'), ...get('mk21')]);
  // bedside.* holds the components plus the newest case; mk1.*, mk2.* and mk21.* hold the earlier cases. Each case part says which versions it belongs to.
  const own = (name) => SHELL.includes(name) || name === 'Power switch' || name === 'Rubber feet' || name === 'Panel foam tape' || isScrew(name);
  const wires = (name) => (name === 'Power switch wires' ? ['mk2', 'mk21', 'mk3'] : null);      // MK I has no switch
  addParts(meta1, bin1, () => ['mk1']);
  addParts(meta2, bin2, (name) => (isScrew(name) && !name.startsWith('Back screws') ? ['mk1', 'mk2'] : ['mk2']));    // MK I borrows MK II's screws
  addParts(meta21, bin21, () => ['mk21']);
  addParts(meta, bin, (name) => (own(name) ? ['mk3'] : wires(name)));
  await addScreen(meta);
  document.getElementById('loading').remove();
}
// These sets differ in length between versions, so the viewer names them without one.
const SCREW_NAMES = { 'Base screws, M3 x 12': 'Base screws, M3', 'Speaker screws and nuts, M4 x 8': 'Speaker screws and nuts, M4', 'Frame screws, M3 x 8': 'Frame screws, M3' };
function addParts(meta, bin, variantOf) {
  for (const p of meta.parts) {
    p.name = SCREW_NAMES[p.name] || p.name;
    const geo = new THREE.BufferGeometry();
    geo.setAttribute('position', new THREE.BufferAttribute(new Float32Array(bin, p.pos, p.verts * 3), 3));
    geo.setAttribute('normal', new THREE.BufferAttribute(new Float32Array(bin, p.nrm, p.verts * 3), 3));
    geo.setIndex(new THREE.BufferAttribute(new Uint32Array(bin, p.idx, p.tris * 3), 1));
    const colour = new THREE.Color().setRGB(p.color[0] / 255, p.color[1] / 255, p.color[2] / 255, THREE.SRGBColorSpace);
    const metal = /gold|pin|knob|shield|usb|steel|screw/i.test(p.name) ? 0.35 : 0.04;
    const mat = new THREE.MeshStandardMaterial({ color: colour, roughness: 0.72, metalness: metal, transparent: p.opacity < 1, opacity: p.opacity, depthWrite: p.opacity >= 1 });
    const mesh = new THREE.Mesh(geo, mat);
    mesh.userData.name = p.name;
    mesh.userData.base = colour.clone();
    model.add(mesh);
    parts.push({ mesh, name: p.name, e1: new THREE.Vector3(...p.e1), e2: new THREE.Vector3(...p.e2), variants: variantOf(p.name), op: p.opacity });
  }
}
// ---------- key marks ----------
// The keycaps have marks sunk into their tops so the keys can be told apart by touch. The exported caps carry one arrangement; here each cap gets a lid in
// its own colour over the top, with the chosen arrangement drawn on it, so the options can be compared. Per key, left to right from the front.
const MARKS = { 'Dots and bar': [1, 2, 3, 'bar'], 'Dots': [1, 2, 3, 4], 'Menu only': [0, 0, 0, 'bar'], 'Plain': [0, 0, 0, 0] };
const KEYS_U = [152.325, 171.375, 190.425, 209.475], KEY_V = 198.5, KEY_W = [6.7, 25.8], KEY_HALF = 8.75;      // from the case model: centres across, top face, front and back, half width
const keyLids = [];             // { group, parent }
function buildKeyMarks() {
  for (const name of Object.keys(MARKS)) {              // the buttons, made here because this runs once the page's helpers exist
    const b = document.createElement('button');
    b.type = 'button'; b.textContent = name; b.setAttribute('aria-pressed', 'false');
    b.addEventListener('click', () => { setMarks(name); track('bedside_key_marks', { marks: name }); });
    $('marks').appendChild(b);
  }
  const S15 = Math.sin(Math.PI / 12), C15 = Math.cos(Math.PI / 12);
  const basis = new THREE.Matrix4().makeBasis(new THREE.Vector3(1, 0, 0), new THREE.Vector3(0, C15, -S15), new THREE.Vector3(0, S15, C15));     // across, into the frame, up the frame
  const caps = parts.filter((p) => p.name === 'Keycaps');
  KEYS_U.forEach((u, i) => {
    const wm = (KEY_W[0] + KEY_W[1]) / 2, v = KEY_V + 0.03;
    const centre = new THREE.Vector3(u, wm * C15 + v * S15, 12 + v * C15 - wm * S15);
    // the cap this key belongs to: the mesh whose box holds its centre
    const parent = caps.find((p) => { p.mesh.geometry.computeBoundingBox(); const b = p.mesh.geometry.boundingBox; return i === 3 ? b.min.x > 195 : b.min.x < 195; }) || caps[0];
    if (!parent) return;
    const g = new THREE.Group(); g.position.copy(centre); g.setRotationFromMatrix(basis);
    const lid = new THREE.Mesh(new THREE.PlaneGeometry(KEY_HALF * 2 - 0.1, KEY_W[1] - KEY_W[0] - 0.1), new THREE.MeshStandardMaterial({ color: parent.mesh.material.color.clone(), roughness: 0.72, metalness: 0.04 }));
    g.add(lid); g.userData.marks = new THREE.Group(); g.userData.marks.position.z = 0.02; g.add(g.userData.marks);
    g.userData.ink = new THREE.MeshStandardMaterial({ color: parent.mesh.material.color.clone().multiplyScalar(0.42), roughness: 0.9, metalness: 0 });
    parent.mesh.add(g); keyLids.push({ group: g, parent, i });
  });
}
function setMarks(name, save = true) {
  state.marks = MARKS[name] ? name : 'Dots and bar';
  for (const { group, i } of keyLids) {
    const m = MARKS[state.marks][i], holder = group.userData.marks;
    holder.clear();
    if (m === 'bar') holder.add(new THREE.Mesh(new THREE.PlaneGeometry(8, 2), group.userData.ink));
    else for (let k = 0; k < m; k++) { const d = new THREE.Mesh(new THREE.CircleGeometry(1, 24), group.userData.ink); d.position.x = 4 * (k - (m - 1) / 2); holder.add(d); }
  }
  for (const b of $('marks').children) b.setAttribute('aria-pressed', String(b.textContent === state.marks));
  if (save) try { localStorage.setItem('bedside-marks', state.marks); } catch (e) { /* storage blocked */ }
}
async function addScreen(meta) {
  // The dashboard image, laid over the panel's active area
  const s = meta.screen;
  const quad = new THREE.BufferGeometry();
  quad.setAttribute('position', new THREE.BufferAttribute(new Float32Array(s.corners.flat()), 3));
  quad.setAttribute('uv', new THREE.BufferAttribute(new Float32Array([0, 0, 1, 0, 1, 1, 0, 1]), 2));
  quad.setIndex([0, 1, 2, 0, 2, 3]);
  const tex = await new THREE.TextureLoader().loadAsync('./' + s.image);
  tex.colorSpace = THREE.SRGBColorSpace;
  tex.anisotropy = renderer.capabilities.getMaxAnisotropy();
  const screen = new THREE.Mesh(quad, new THREE.MeshBasicMaterial({ map: tex, side: THREE.DoubleSide, toneMapped: false, color: 0xe4e4e0 }));
  screen.userData.name = 'E Ink ED103TC2 panel';
  model.add(screen);
  parts.push({ mesh: screen, name: screen.userData.name, e1: new THREE.Vector3(...s.e1), e2: new THREE.Vector3(...s.e2), screen: true, op: 1 });
}

// ---------- controls ----------
const $ = (id) => document.getElementById(id);
function bindRange(id, keyName) {
  const el = $(id);
  let sent = false;
  el.addEventListener('input', () => {
    state[keyName] = parseFloat(el.value);
    if (!sent && state[keyName] > 0) { sent = true; track('bedside_slider', { slider: keyName }); }
  });
  return el;
}
const ranges = { explode: bindRange('explode', 'explode'), parts: bindRange('parts', 'parts'), spin: bindRange('spin', 'spin') };

for (const name of Object.keys(FINISHES)) {
  const b = document.createElement('button');
  b.type = 'button'; b.textContent = name; b.setAttribute('aria-pressed', String(name === 'Beige'));
  b.addEventListener('click', () => { setFinish(name); track('bedside_case_colour', { colour: name }); });
  $('finish').appendChild(b);
}
for (const name of Object.keys(BACKDROPS)) {
  const b = document.createElement('button');
  b.type = 'button'; b.textContent = name; b.setAttribute('aria-pressed', String(name === 'Auto'));
  b.addEventListener('click', () => { setBackdrop(name); track('bedside_backdrop', { backdrop: name }); });
  $('backdrop').appendChild(b);
}
for (const [label, id] of Object.entries(DESIGNS)) {
  const b = document.createElement('button');
  b.type = 'button'; b.textContent = label;
  b.addEventListener('click', () => { setDesign(id); track('bedside_version', { version: label }); });
  $('design').appendChild(b);
}
function setDesign(id) {
  state.design = Object.values(DESIGNS).includes(id) ? id : 'mk2';
  for (const b of $('design').children) b.setAttribute('aria-pressed', String(DESIGNS[b.textContent] === state.design));
  for (const b of $('show').children) if (NOT_MK1.includes(b.textContent)) b.style.display = state.design === 'mk1' ? 'none' : '';   // MK I has no plinth or access cover
  if (!/[#&]embed/.test(location.hash)) try { localStorage.setItem('bedside-version', state.design); } catch (e) { /* storage blocked */ }      // the copy inside the assembly guide doesn't change what the viewer remembers
  applyVisibility();
  pointerMoved = true;
  if (hovered) { const h = hovered; hovered = null; setHover(h); }
}
for (const name of CASE_PARTS) {
  const b = document.createElement('button');
  b.type = 'button'; b.textContent = name; b.setAttribute('aria-pressed', 'true');
  b.addEventListener('click', () => {
    const on = b.getAttribute('aria-pressed') !== 'true';
    b.setAttribute('aria-pressed', String(on));
    if (on) state.hidden.delete(name); else state.hidden.add(name);
    track('bedside_case_part', { part: name, shown: on });
    applyVisibility();
  });
  $('show').appendChild(b);
}

// Wireframe: the three printed case parts drawn as their edges over a faint fill, so the inside shows through.
let wire = false;
const HOT = new THREE.Color('#e2783c');
function paintWire() {
  const f = FINISHES[state.finish] || FINISHES.Beige;
  for (const p of parts) {
    if (!p.lines) continue;
    const c = p.lines.material.color.copy(finishColour(f, p.mesh.userData.base));
    const lum = 0.3 * c.r + 0.59 * c.g + 0.11 * c.b;                 // keep the lines readable against the backdrop
    if (lightBackdrop && lum > 0.25) c.multiplyScalar(0.5); else if (!lightBackdrop && lum < 0.1) c.set('#9aa0ad');
    if (hovered === p.name) c.copy(HOT);
  }
}
let wireShown = 0;                          // 0 solid .. 1 wireframe; eased in the frame loop so the case fades between the two
function setWire(on) {
  wire = on;
  for (const p of parts) {
    if (p.screen || !SHELL.includes(p.name) || p.lines) continue;
    // built on first use: only the creases, not every triangle
    p.lines = new THREE.LineSegments(new THREE.EdgesGeometry(p.mesh.geometry, 28), new THREE.LineBasicMaterial({ transparent: true, opacity: 0 }));
    p.lines.raycast = () => {}; p.lines.visible = false;
    p.mesh.add(p.lines);
  }
  paintWire();
  $('wireToggle').setAttribute('aria-pressed', String(on));
  pointerMoved = true;
}
function applyWire(dt) {
  const target = wire ? 1 : 0;
  if (wireShown === target) return;
  wireShown += (target - wireShown) * (1 - Math.exp(-dt * 2.6));
  if (Math.abs(target - wireShown) < 0.004) wireShown = target;
  for (const p of parts) {
    if (!p.lines) continue;
    const m = p.mesh.material, see = wireShown > 0;
    if (m.transparent !== see) { m.transparent = see; m.needsUpdate = true; }
    m.opacity = 1 - 0.93 * wireShown; m.depthWrite = wireShown < 0.5;
    p.lines.visible = see; p.lines.material.opacity = 0.9 * wireShown;
  }
}
$('wireToggle').addEventListener('click', () => { setWire(!wire); track('bedside_wireframe', { on: wire }); });

// The panel folds down to a round button. Phones start folded and fold again when the model is touched.
const panel = $('controls');
const phone = matchMedia('(max-width:640px)');
function setOpen(open) {
  if (!open) panel.scrollTop = 0;         // left scrolled, the folded button would sit out of view
  panel.classList.toggle('open', open);
  $('fab').setAttribute('aria-expanded', String(open));
}
$('fab').addEventListener('click', () => { setOpen(true); track('bedside_controls', { open: true }); });
$('closeControls').addEventListener('click', () => { setOpen(false); track('bedside_controls', { open: false }); });
canvas.addEventListener('pointerdown', () => { if (phone.matches) setOpen(false); });

// With the electronics hidden, what is left is what gets printed or screwed together: the case, its own screws, the keycaps and the knob.
const CASE_SCREWS = /^(Bezel|Base|Back|Frame|Shell corner) screws/;
const isHardware = (p) => !p.screen && (SHELL.includes(p.name) || p.name === 'Accent stripe' || p.name === 'Keycaps' || p.name === 'Knob' || p.name === 'Rubber feet' || p.name === 'Panel foam tape' || CASE_SCREWS.test(p.name));
let elec = true;
function setElec(on) {
  elec = on;
  $('elecToggle').setAttribute('aria-pressed', String(on));
  applyVisibility();
}
$('elecToggle').addEventListener('click', () => { setElec(!elec); track('bedside_electronics', { on: elec }); });

function applyVisibility() {
  for (const p of parts) {
    const hide = (!elec && !isHardware(p)) || state.hidden.has(p.name) || (p.variants && !p.variants.includes(state.design)) || (p.name === 'Accent stripe' && state.hidden.has('Front bezel'));
    p.off = hide; p.mesh.visible = !hide;
  }
  if (build) planBuild();
}

let flight = null;
for (const [name, pos] of Object.entries(VIEWS)) {
  const b = document.createElement('button');
  b.type = 'button'; b.textContent = name;
  b.addEventListener('click', () => {
    flight = { from: camera.position.clone(), to: new THREE.Vector3(...pos).multiplyScalar(lastFit).add(CENTRE), t: 0 };
    state.spin = 0; ranges.spin.value = 0;
    track('bedside_view', { view: name });
  });
  $('views').appendChild(b);
}
$('reset').addEventListener('click', () => {
  stopDemo(false); endBuild(); endFlash();
  track('bedside_reset');
  state.explode = state.parts = state.spin = 0;
  for (const k of Object.keys(ranges)) ranges[k].value = 0;
  state.hidden.clear();
  setFinish('Beige');
  setLabels(false);
  setCats(CATS_DEFAULT);
  setMarks('Dots and bar');
  setWire(false);
  elec = true; $('elecToggle').setAttribute('aria-pressed', 'true');
  setDesign('mk2');
  setBackdrop('Auto');
  for (const b of $('show').children) b.setAttribute('aria-pressed', 'true');
  applyVisibility();
  controls.target.copy(CENTRE);
  flight = { from: camera.position.clone(), to: new THREE.Vector3(...VIEWS.Front).add(CENTRE), t: 0 };
});

// ---------- share ----------
// Everything on screen goes into the link's hash: version, colours, sliders, hidden parts, wireframe, labels, screw sizes and the camera.
const SLUGS = { 'Front bezel': 'bezel', 'Rear shell': 'shell', Plinth: 'plinth', 'Access cover': 'cover' };
function shareURL() {
  const q = new URLSearchParams();
  q.set('mk', MK_HASH[state.design]);
  q.set('case', state.finish.toLowerCase());
  if (state.marks !== 'Dots and bar') q.set('keys', state.marks.toLowerCase().replace(/ /g, '-'));
  q.set('bg', state.bg.toLowerCase());
  if (state.explode > 0) q.set('x', state.explode.toFixed(2));
  if (state.parts > 0) q.set('c', state.parts.toFixed(2));
  if (state.spin > 0) q.set('spin', state.spin.toFixed(1));
  if (state.hidden.size) q.set('hide', [...state.hidden].map((n) => SLUGS[n]).filter(Boolean).join('_'));
  if (wire) q.set('wire', '1');
  if (!elec) q.set('elec', '0');
  if (labelsOn) { q.set('labels', '1'); if ([...cats].sort().join() !== [...CATS_DEFAULT].sort().join()) q.set('lab', [...cats].join('_').toLowerCase()); }
  if (sizesOn) q.set('sizes', '1');
  const eye = camera.position.clone().sub(controls.target).divideScalar(lastFit).add(controls.target);   // without the explode pull-back, which is re-applied on load
  q.set('cam', [...eye.toArray(), ...controls.target.toArray()].map(Math.round).join('_'));
  return location.origin + location.pathname + '#' + q.toString();
}
function applyURL(q) {
  for (const [key, id] of [['x', 'explode'], ['c', 'parts'], ['spin', 'spin']]) {
    const v = parseFloat(q.get(key)), el = ranges[id];
    if (Number.isFinite(v)) { state[id] = Math.min(parseFloat(el.max), Math.max(0, v)); el.value = state[id]; }
  }
  if (q.has('lab')) setCats(CATS.filter((c) => q.get('lab').split('_').includes(c.toLowerCase())));
  for (const slug of (q.get('hide') || '').split('_')) {
    const name = Object.keys(SLUGS).find((n) => SLUGS[n] === slug);
    if (!name) continue;
    state.hidden.add(name);
    for (const b of $('show').children) if (b.textContent === name) b.setAttribute('aria-pressed', 'false');
  }
  const cam = (q.get('cam') || '').split('_').map(Number);
  if (cam.length === 6 && cam.every(Number.isFinite)) {
    camera.position.set(cam[0], cam[1], cam[2]); controls.target.set(cam[3], cam[4], cam[5]); controls.update();
  }
}
$('share').addEventListener('click', async () => {
  const url = shareURL(), btn = $('share');
  history.replaceState(null, '', url);
  let ok = false;
  try { await navigator.clipboard.writeText(url); ok = true; } catch (e) { /* clipboard blocked: the link is still in the address bar */ }
  btn.textContent = ok ? 'Link copied' : 'Link is in the address bar';
  setTimeout(() => { btn.textContent = 'Copy link'; }, 2000);
  track('bedside_share');
});

// ---------- demo ----------
// A scripted run: the beige case turning slowly on the light backdrop, a fade to wireframe as the backdrop turns to blueprint, the case and its components
// coming apart and closing again, and a finish back in beige with the panel facing the viewer.
// Anything the viewer does to the model stops it. #demo=1 in the link plays it once on load, #demo=loop keeps it going.
let demo = null;
const FACING = [70, -650, 150];           // nearly square-on to the panel
const ramp = (t, a, b) => { const x = Math.min(1, Math.max(0, (t - a) / (b - a))); return x * x * (3 - 2 * x); };
function setSlider(id, v) { state[id] = v; ranges[id].value = v; }
function startDemo(loop) {
  endBuild(); endFlash();
  demo = { t: 0, loop, step: 0, before: { bg: state.bg, finish: state.finish, wire, labels: labelsOn, spin: state.spin, open: panel.classList.contains('open') } };
  setLabels(false); setWire(false); wireShown = 0.001;                     // snap back to solid before the run starts
  setFinish('Beige', false); setBackdrop('Studio', false);
  setSlider('explode', 0); setSlider('parts', 0); setSlider('spin', 0.5);    // slow enough that the explode is seen from the front quarter
  state.hidden.clear(); for (const b of $('show').children) b.setAttribute('aria-pressed', 'true'); applyVisibility();
  controls.target.copy(CENTRE);
  flight = { from: camera.position.clone(), to: new THREE.Vector3(...VIEWS.Front).multiplyScalar(lastFit).add(CENTRE), t: 0 };
  setOpen(false);
  $('demo').textContent = 'Stop demo'; $('demo').setAttribute('aria-pressed', 'true');
}
function stopDemo(restore) {
  if (!demo) return;
  const b = demo.before; demo = null;
  $('demo').textContent = 'Play demo'; $('demo').setAttribute('aria-pressed', 'false');
  if (!restore) return;                                                   // stopped by hand: leave things as they are
  setSlider('explode', 0); setSlider('parts', 0); setSlider('spin', 0);
  setWire(false); setLabels(b.labels);                                   // ends as it began: solid beige on the light backdrop, panel facing the viewer
  if (b.open) setOpen(true);
}
function stepDemo(dt) {
  const d = demo; d.t += dt;
  const at = (n, time, fn) => { if (d.step === n && d.t >= time) { d.step++; fn(); } };
  at(0, 3.0, () => setWire(true));
  at(1, 3.6, () => setBackdrop('Blueprint', false));
  if (d.t > 6) { setSlider('explode', ramp(d.t, 6, 9.5) - ramp(d.t, 17, 20)); setSlider('parts', ramp(d.t, 9.5, 12.5) - ramp(d.t, 15.5, 18)); }
  at(2, 17.5, () => {                                                     // come round to face the panel for the finish
    setSlider('spin', 0);
    flight = { from: camera.position.clone(), to: new THREE.Vector3(...FACING).multiplyScalar(lastFit).add(CENTRE), t: 0, rate: 0.45 };
  });
  at(3, 20.2, () => { setWire(false); setBackdrop('Studio', false); });
  if (d.t >= 23) { if (d.loop) startDemo(true); else stopDemo(true); }
}
$('demo').addEventListener('click', () => { if (demo) stopDemo(false); else { startDemo(false); track('bedside_demo'); } });
canvas.addEventListener('pointerdown', () => stopDemo(false));
canvas.addEventListener('wheel', () => stopDemo(false), { passive: true });
for (const id of ['explode', 'parts', 'spin']) $(id).addEventListener('pointerdown', () => stopDemo(false));
for (const id of ['explode', 'parts']) $(id).addEventListener('pointerdown', () => endBuild());

// ---------- build ----------
// The device going together one module at a time, in the order it is assembled, or coming apart in reverse. Each module flies in from the side it is
// fitted from and fades up as it lands; its screws follow. The camera comes round to face that side first, and the next module waits for it.
// #build=1 in the link plays it on load, #build=loop builds and strips it over and over.
const WN = [0, Math.cos(Math.PI / 12), -Math.sin(Math.PI / 12)], VN = [0, Math.sin(Math.PI / 12), Math.cos(Math.PI / 12)];   // into the frame, and up it
const along = (v, k) => v.map((x) => x * k);
const BUILD = [
  ['Chassis frame', ['Frame plate', 'Mid chassis and base'], [0, 0, 150]],
  ['Foam tape', ['Panel foam tape'], along(WN, -120)],
  ['E-ink panel', ['E Ink ED103TC2 panel'], along(WN, -160)],
  ['Front bezel', ['Front bezel', 'Accent stripe', 'Bezel screws, M3 x 8'], along(WN, -190)],
  ['Flex adapter', ['E-paper flex adapter', 'Flex adapter screws, M2.5 x 6'], along(WN, 130)],
  ['Display driver HAT', ['Waveshare IT8951 driver HAT', 'Driver HAT screws, M2.5 x 8'], along(WN, 150)],
  ['Pico and GPIO splitter', ['GPIO splitter', 'Hard Stuff Pico to Pi HAT', 'Pico sockets and headers', 'Pimoroni Pico Plus 2 W'], along(WN, 170)],
  ['Power and amp board', ['Power and amp protoboard', 'Adafruit PowerBoost 1000C', 'Adafruit MAX98357A amplifier', 'Headers, resistors, capacitors and diode', 'Power board screws, M2 x 6'], along(WN, 150)],
  ['Key module', ['Adafruit NeoKey 1x4 QT', 'Cherry MX Brown switches', 'Key module screws, M2.5 x 8'], along(VN, 120)],
  ['Rotary encoder', ['Adafruit I2C rotary encoder'], [-70, 60, 0]],
  ['Base tray', ['Base tray', 'Frame screws, M3'], [0, 0, -130]],
  ['Speaker', ['Visaton FRS 5 X speaker', 'Speaker screws and nuts, M4'], [90, 40, 40]],
  ['Battery', ['LiPo battery, 4400 mAh'], [0, 70, 110]],
  ['USB-C inlet and power switch', ['USB-C power inlet', 'USB-C inlet screws, M2.5 x 8', 'Power switch'], [0, 130, 40]],
  ['Wiring', ['Wires to the GPIO header', 'USB-C inlet wires', 'Power switch wires', 'Speaker leads', 'Battery lead', 'STEMMA QT cables'], [0, 60, 30]],
  ['Rear shell', ['Rear shell', 'Back screws, M3 x 8', 'Shell corner screws, M3 x 8'], [0, 210, 30]],
  ['Keycaps and knob', ['Keycaps', 'Knob'], along(VN, 110)],
  ['Plinth', ['Plinth', 'Base screws, M3'], [0, 230, 0]],
  ['Access cover', ['Access cover'], [0, 130, 110]],
  ['Rubber feet', ['Rubber feet'], [0, 0, -90]],
];
const BUILD_FROM = { Knob: [110, 0, 0] };                 // parts that arrive from somewhere other than their module
const STEP = 0.85, DUR = 1.1, SCREW_LAG = 0.6;          // seconds: the least between modules, for one to land, and before its screws follow
const PAUSE = 0.3, SETTLE = 0.2;                         // a beat after a module is home, and another once the camera has come round, before the next one moves
let build = null, buildSpread = 0;
function planBuild() {
  // which modules this version of the case has, and where each of their parts flies in from
  const mods = [], screwed = [];
  for (const p of parts) p.mod = -1;
  for (const [title, names, from] of BUILD) {
    const mine = parts.filter((p) => names.includes(p.name) && !p.off);
    if (!mine.length) continue;
    for (const p of mine) {
      p.mod = mods.length;
      const own = p.e1.clone().add(p.e2);
      p.from = new THREE.Vector3(...(BUILD_FROM[p.name] || from));
      if (isScrew(p.name) && own.lengthSq() > 1) p.from = own.setLength(70);       // screws go in along their own axis
    }
    mods.push(title); screwed.push(mine.some((p) => isScrew(p.name)));
  }
  // where the camera stands for each module, as [azimuth, elevation], and when each module starts: one thing at a time, so a module and its screws
  // are home before the camera turns, and the camera is still before the next module moves
  const at = [], view = [], turn = [];
  let az = FRONT_AZ;
  for (const title of mods) {
    const [x, y, z] = BUILD.find((b) => b[0] === title)[2], h = Math.hypot(x, y), n = Math.hypot(h, z);
    if (h > 0.5 * n) {                                    // fitted from a side: stand a quarter-turn off square to it, on whichever hand is nearer
      const side = Math.atan2(y, x), off = Math.atan2(Math.sin(az - side), Math.cos(az - side));
      if (Math.abs(off) > SIDE_KEEP) az = side + Math.sign(off || 1) * SIDE_OFF + Math.round((az - side - Math.sign(off || 1) * SIDE_OFF) / (2 * Math.PI)) * 2 * Math.PI;
    }                                                     // fitted from above or below: stay where it is, and look down or up at it
    const el = (16 + (z > 0 ? 24 : 40) * z / n) * Math.PI / 180;
    const i = view.length, far = i ? Math.hypot(az - view[i - 1][0], el - view[i - 1][1]) : 0;
    turn.push(far < 0.02 ? 0 : 0.35 + far / TURN_RATE);
    at.push(i ? at[i - 1] + DUR + (screwed[i - 1] ? SCREW_LAG : 0) + PAUSE + (turn[i] ? turn[i] + SETTLE : 0) : 0);
    view.push([az, el]);
  }
  build.mods = mods; build.at = at; build.view = view; build.turn = turn; build.screwed = screwed;
  build.total = at[at.length - 1] + SCREW_LAG + DUR;
}
const FRONT_AZ = Math.atan2(-560, 330), SIDE_OFF = 28 * Math.PI / 180, SIDE_KEEP = 40 * Math.PI / 180, TURN_RATE = 130 * Math.PI / 180;   // radians a second
function buildView(T) {
  // the camera's place at time T: at a module's angle while it goes on, turning to the next one's just before that starts
  const b = build;
  let i = 0;
  while (i + 1 < b.at.length && b.at[i + 1] <= T) i++;
  if (i + 1 >= b.at.length || !b.turn[i + 1]) return b.view[i];
  let u = Math.min(1, Math.max(0, (T - (b.at[i + 1] - SETTLE - b.turn[i + 1])) / b.turn[i + 1]));
  u = u * u * (3 - 2 * u);
  return [b.view[i][0] + (b.view[i + 1][0] - b.view[i][0]) * u, b.view[i][1] + (b.view[i + 1][1] - b.view[i][1]) * u];
}
function startBuild(dir, loop) {
  endFlash();
  const apart = build && build.apart;                    // already stripped to the chassis: build from there
  const before = build ? build.before : { spin: state.spin, labels: labelsOn };
  stopDemo(false);
  build = { dir, loop, speed: 1, hold: 0, keep: dir < 0 || apart, apart: false, before };
  setLabels(false); if (wire) { setWire(false); wireShown = 0.001; }
  setSlider('explode', 0); setSlider('parts', 0); setSlider('spin', 0);            // the camera is led round to each module instead
  if (phone.matches) setOpen(false);
  planBuild();
  build.T = dir < 0 ? build.total : (build.keep ? (build.at[1] || STEP) - 0.3 : -0.5);
  flight = null;
  paintBuild();
}
function endBuild() {
  // put every part back as it was
  if (!build) return;
  const b = build.before; build = null;
  for (const p of parts) {
    const m = p.mesh.material;
    if (p.faded) { m.opacity = p.op; m.transparent = p.op < 1; m.depthWrite = p.op >= 1; m.needsUpdate = true; p.faded = false; }
    p.mesh.visible = !p.off;
  }
  setSlider('spin', b.spin); setLabels(b.labels);
  paintBuild();
}
function paintBuild() {
  const run = build && build.dir !== 0 && build.only == null;
  $('build').textContent = run && build.dir > 0 ? 'Finish' : 'Build';
  $('apart').textContent = run && build.dir < 0 ? 'Finish' : 'Take apart';
  $('build').setAttribute('aria-pressed', String(!!(run && build.dir > 0))); $('apart').setAttribute('aria-pressed', String(!!(run && build.dir < 0)));
  if (!run && !(build && build.only != null) && !flash) $('buildCap').classList.remove('on');
}
function stepBuild(dt) {
  const b = build;
  if (b.dir === 0 && b.only != null) return;              // a single step, finished and held
  if (b.dir === 0) {                                      // resting between runs of the loop
    if (b.loop && (b.hold -= dt) <= 0) { if (b.apart) startBuild(1, true); else startBuild(-1, true); }
    return;
  }
  if (b.only != null) {                                   // one step on its own: play it to its end and hold there
    b.T += b.dir * dt;
    if ((b.dir > 0 && b.T >= b.stop) || (b.dir < 0 && b.T <= b.stop)) { b.T = b.stop; b.dir = 0; }
    return;
  }
  b.T += b.dir * dt * b.speed;
  let i = 0;
  while (i + 1 < b.at.length && b.at[i + 1] <= b.T) i++;
  const cap = $('buildCap');
  cap.textContent = `${b.mods[i]}  ·  ${i + 1} of ${b.mods.length}`; cap.classList.add('on');
  if (b.dir > 0 && b.T >= b.total) {
    if (b.loop) { b.dir = 0; b.hold = 2.5; b.T = b.total; paintBuild(); } else { endBuild(); flight = { from: camera.position.clone(), to: new THREE.Vector3(...FACING).multiplyScalar(lastFit).add(CENTRE), t: 0, rate: 0.6 }; }
  } else if (b.dir < 0 && b.T <= (b.at[1] || STEP)) {
    b.T = b.at[1] || STEP; b.dir = 0; b.apart = true; b.hold = 1.5; setSlider('spin', b.before.spin); paintBuild();
  }
}
function placeBuild(p) {
  // how far in this part is: 0 away and unseen, 1 home
  let t = 1;
  if (p.mod > 0 || (p.mod === 0 && !build.keep)) t = Math.min(1, Math.max(0, (build.T - build.at[p.mod] - (isScrew(p.name) ? SCREW_LAG : 0)) / DUR));
  const m = p.mesh.material;
  if (t >= 1) {
    if (p.faded) { m.opacity = p.op; m.transparent = p.op < 1; m.depthWrite = p.op >= 1; m.needsUpdate = true; p.faded = false; }
    p.mesh.visible = !p.off;
    return;
  }
  const fade = Math.min(1, t * 2.4), s = 1 - Math.pow(1 - t, 3);
  if (!m.transparent) { m.transparent = true; m.needsUpdate = true; }
  m.opacity = p.op * fade; m.depthWrite = p.op >= 1 && fade > 0.7; p.faded = true;
  p.mesh.visible = !p.off && t > 0.002;
  p.mesh.position.addScaledVector(p.from, 1 - s);
}
$('build').addEventListener('click', () => {
  if (build && build.only == null && build.dir > 0) { build.speed = 6; build.loop = false; return; }
  startBuild(1, false); track('bedside_build', { way: 'build' });
});
$('apart').addEventListener('click', () => {
  if (build && build.only == null && build.dir < 0) { build.speed = 6; build.loop = false; return; }
  if (build && build.apart) return;
  startBuild(-1, false); track('bedside_build', { way: 'apart' });
});

// ---------- single steps ----------
// The assembly guide (assembly.html) links to one step at a time: #step=<module> plays that module going on and holds with everything after it
// still off; with way=apart it plays the module coming off instead. The module is named by its title in BUILD, in lower case with dashes.
const slug = (s) => s.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '');
function showStep(id, apart) {
  endFlash(); stopDemo(false);
  const before = build ? build.before : { spin: state.spin, labels: labelsOn };
  build = { dir: apart ? -1 : 1, loop: false, speed: 1, hold: 0, keep: true, apart: false, before, only: 0 };
  setLabels(false); if (wire) { setWire(false); wireShown = 0.001; }
  setSlider('explode', 0); setSlider('parts', 0); setSlider('spin', 0);
  planBuild();
  const n = build.mods.findIndex((m) => slug(m) === id);
  if (n < 0) { endBuild(); return; }
  const home = build.at[n] + DUR + (build.screwed[n] ? SCREW_LAG : 0);      // when this module and its screws are home; the next one has not started
  build.only = n; build.keep = n > 0;
  build.T = apart ? home + 0.5 : build.at[n] - 0.3;       // a beat before it moves
  build.stop = apart ? build.at[n] : home;
  flight = null;
  const cap = $('buildCap');
  cap.textContent = apart ? `Remove  ·  ${build.mods[n]}` : `Fit  ·  ${build.mods[n]}  ·  ${n + 1} of ${build.mods.length}`; cap.classList.add('on');
  paintBuild();
}

// ---------- flashing ----------
// #flash=1: the finished device from behind, the access cover coming off and a USB-C cable going into the Pico's port underneath it.
const PORT = new THREE.Vector3(174.4, 58.43, 75.2), UP_FRAME = new THREE.Vector3(...VN);     // the mouth of the Pico's USB-C socket, and the way it faces: up the leaning frame
const COVER_OFF = new THREE.Vector3(0, 75, 34);
let flash = null, cable = null;
function makeCable() {
  // drawn here, not exported from the CAD model: a USB-C plug and a length of cable, with +Y pointing away from the port
  const g = new THREE.Group();
  const metal = new THREE.MeshStandardMaterial({ color: 0xc9ccd2, roughness: 0.4, metalness: 0.6 }), rubber = new THREE.MeshStandardMaterial({ color: 0x2a2b2f, roughness: 0.8, metalness: 0.02 });
  const add = (geo, mat, y) => { const m = new THREE.Mesh(geo, mat); m.position.y = y; g.add(m); };
  add(new THREE.BoxGeometry(8.3, 6.5, 2.5), metal, -2.25);                 // the plug's shell, 5.5 mm of it inside the socket
  add(new THREE.BoxGeometry(12, 22, 6.5), rubber, 12);                     // overmould
  add(new THREE.CylinderGeometry(1.8, 1.8, 150, 16), rubber, 98);          // cable
  g.setRotationFromMatrix(new THREE.Matrix4().makeBasis(new THREE.Vector3(1, 0, 0), UP_FRAME, new THREE.Vector3(1, 0, 0).cross(UP_FRAME)));
  g.visible = false; scene.add(g);
  return g;
}
function startFlash() {
  stopDemo(false); endBuild();
  if (!cable) cable = makeCable();
  setLabels(false); if (wire) { setWire(false); wireShown = 0.001; }
  setSlider('explode', 0); setSlider('parts', 0); setSlider('spin', 0);
  flash = { t: -0.6 };
  flight = { from: camera.position.clone(), to: PORT.clone().add(new THREE.Vector3(-95, 215, 140).multiplyScalar(lastFit)), aimFrom: controls.target.clone(), aim: PORT.clone(), t: 0, rate: 0.9 };
  const cap = $('buildCap'); cap.textContent = 'Flashing  ·  cover off, USB-C cable into the Pico'; cap.classList.add('on');
}
function endFlash() {
  if (!flash) return;
  flash = null; cable.visible = false;
  flight = { from: camera.position.clone(), to: new THREE.Vector3(...VIEWS.Rear).multiplyScalar(lastFit).add(CENTRE), aimFrom: controls.target.clone(), aim: CENTRE.clone(), t: 0 };
  paintBuild();
}
const smooth = (t) => { t = Math.min(1, Math.max(0, t)); return t * t * (3 - 2 * t); };
function placeFlash(p) {
  if (p.name === 'Access cover') p.mesh.position.addScaledVector(COVER_OFF, smooth(flash.t / 1.0));
}
function stepFlash(dt) {
  flash.t = Math.min(4, flash.t + dt);
  const out = 1 - smooth((flash.t - 1.3) / 1.2);
  cable.visible = flash.t > 1.2;
  cable.position.copy(PORT).addScaledVector(UP_FRAME, 55 * out);
}
function applyHashStep(q) {
  if (q.has('step')) showStep(q.get('step'), q.get('way') === 'apart');
  else if (q.has('flash')) startFlash();
  else return false;
  return true;
}
window.addEventListener('hashchange', () => {
  // the assembly guide drives an embedded copy of this page by changing its hash
  const q = new URLSearchParams(location.hash.slice(1));
  const mk = Object.keys(MK_HASH).find((k) => MK_HASH[k] === q.get('mk'));
  if (mk && mk !== state.design) { setDesign(mk); applyVisibility(); }
  if (!applyHashStep(q)) { endBuild(); endFlash(); }
});

// What comes away with the base tray of MK II.I and MK III when the case is exploded: the parts fixed to it, and the screws that come up through it.
const ON_TRAY = new Set(['Base tray', 'Visaton FRS 5 X speaker', 'Speaker screws and nuts, M4', 'LiPo battery, 4400 mAh', 'USB-C power inlet', 'USB-C inlet screws, M2.5 x 8', 'Power switch',
  'Base screws, M3', 'Frame screws, M3', 'Rubber feet']);
const TRAY_DROP = 70;
const PANEL_PARTS = new Set(['USB-C power inlet', 'USB-C inlet wires', 'Power switch wires']);

// ---------- hover ----------
const ray = new THREE.Raycaster();
const pointer = new THREE.Vector2(2, 2);
let hovered = null, pointerDown = false, pointerMoved = true;
const tip = $('tip');
canvas.addEventListener('pointermove', (e) => {
  pointer.set((e.clientX / window.innerWidth) * 2 - 1, -(e.clientY / window.innerHeight) * 2 + 1);
  pointerMoved = true;
});
canvas.addEventListener('pointerdown', () => { pointerDown = true; if (build) build.free = true; });   // dragging during a build takes the camera over for the rest of it
window.addEventListener('pointerup', () => { pointerDown = false; });
canvas.addEventListener('pointerleave', () => { pointer.set(2, 2); pointerMoved = true; });

function setHover(name) {
  if (name === hovered) return;
  hovered = name;
  for (const p of parts) {
    if (p.screen) continue;
    const m = p.mesh.material;
    m.emissive.setHex(name && p.name === name ? 0x3a1d0c : 0x000000);
  }
  paintWire();
  if (!name) { tip.classList.remove('on'); return; }
  const group = parts.filter((p) => p.name === name && !p.screen && p.mesh.visible);
  const c = group.length ? '#' + group[0].mesh.material.color.getHexString() : '#ccc';
  tip.querySelector('.name').innerHTML = `<i style="background:${c}"></i>`;
  tip.querySelector('.name').append(name);
  tip.querySelector('.meta').textContent = DETAILS[name] || '';
  tip.classList.add('on');
}
const DETAILS = {
  'Front bezel': 'Printed case. Holds the glass from the front.',
  'Mid chassis and base': 'Printed case. Everything mounts to this.',
  'Frame plate': 'Printed case, in black. The boards, keys and encoder mount to this. A beam along its foot and five gussets up its back screw to the base tray.',
  'Base tray': 'Printed case, in black. Carries the battery, speaker, USB-C inlet and power switch.',
  'Frame screws, M3': 'Nine, up through the base tray: four into the foot of the frame plate and five into the heels of its gussets. All M3 x 8 on MK II.I; on MK III the four in the foot are M3 x 12.',
  'Rear shell': 'Printed case. Slides off the back.',
  'Accent stripe': 'Orange insert, matching the menu key.',
  'Access cover': 'Snaps on over the Pico\'s USB-C port, for reflashing by cable.',
  'Bezel screws, M3 x 8': 'Six, through the chassis plate into the bezel.',
  'Base screws, M3': 'Up through the base into the shell and plinth posts. M3 x 12, or on MK III two M3 x 16 into the plinth posts.',
  'Shell corner screws, M3 x 8': 'Two, up through the floor of the rear shell where it overhangs the base, into the bottom corners of the frame plate.',
  'Back screws, M3 x 8': 'Through the back of the rear shell into chassis posts.',
  'Driver HAT screws, M2.5 x 8': 'Two; the other two corners sit on locating pins.',
  'Power board screws, M2 x 6': 'Four, one per corner.',
  'Flex adapter screws, M2.5 x 6': 'Four, one per corner, into whichever holes in the rails let the flex reach.',
  'Key module screws, M2.5 x 8': 'Two, down through the back holes of the NeoKey into the fins. The front holes sit on pegs.',
  'Speaker screws and nuts, M4': 'Two, through the lugs and the baffle. M4 x 8, or on MK III M4 x 10 with two washers under each head.',
  'USB-C inlet screws, M2.5 x 8': 'Two, down into the pedestal.',
  'Panel foam tape': 'Single-sided EVA foam, 2 mm thick and 5 mm wide, stuck to the front of the frame plate by its blue side. It presses the edges of the glass against the bezel without gluing it.',
  'Rubber feet': 'Four self-adhesive pads, 12 mm across and 2 mm thick, stuck to the flat underside of the base tray. Bought, not printed.',
  'Power switch': 'Mini toggle, fixed to the chassis beside the USB-C inlet.',
  'Plinth': 'Printed case, in black. Covers the battery, speaker and USB-C inlet.',
  'E Ink ED103TC2 panel': '10.3 inch, 1872 × 1404, 16 greys.',
  'Waveshare IT8951 driver HAT': 'Drives the e-paper panel over SPI.',
  'GPIO splitter': 'Doubles the 40-pin header: one for the HAT, one for wiring.',
  'Pimoroni Pico Plus 2 W': 'RP2350B, 16 MB flash, 8 MB PSRAM, Wi-Fi.',
  'Hard Stuff Pico to Pi HAT': 'Adapts the Pico to the Raspberry Pi header.',
  'Pico sockets and headers': 'The Pico plugs in, so it can come out.',
  'Wires to the GPIO header': 'One zip-tied loom, soldered into the power board, with a 2x20 Dupont plug on the splitter: 5 V, ground, I2S audio and the three sense lines.',
  'USB-C inlet wires': 'Red and black, 5 V in from the socket.',
  'Power switch wires': 'White and black: closing the switch pulls EN low and turns the device off.',
  'Speaker leads': 'A twisted pair from the amp terminal block.',
  'Battery lead': 'JST-PH plug into the PowerBoost.',
  'STEMMA QT cables': 'I2C chain: Pico to the encoder, encoder to the keys.',
  'Power and amp protoboard': 'Hand-wired 5 × 7 cm board.',
  'Adafruit PowerBoost 1000C': 'Charges the battery and makes 5 V.',
  'Adafruit MAX98357A amplifier': 'I2S class-D amplifier, 3 W.',
  'Headers, resistors, capacitors and diode': 'Sense dividers, supply capacitor, Schottky diode.',
  'LiPo battery, 4400 mAh': 'Keeps the alarm going through a power cut.',
  'Visaton FRS 5 X speaker': '5 cm full-range driver, fires out the left side of the plinth.',
  'USB-C power inlet': 'Mains power, centre of the back.',
  'E-paper flex adapter': 'Joins the panel flex to the ribbon for the HAT. It sits on two rails just above the flex slot and can be moved up in 3.5 mm steps.',
  'Adafruit NeoKey 1x4 QT': 'Four key sockets with backlights, on I2C.',
  'Cherry MX Brown switches': 'Tactile, quiet enough for a bedroom.',
  'Keycaps': 'Printed, flat-topped, 17.5 x 19.1 mm, filling the slot in the top of the case, with marks sunk into the top to tell them apart by touch: three grey, one orange for the menu. Each pushes onto its switch\'s cross.',
  'Adafruit I2C rotary encoder': 'Detented, with a push switch.',
  'Knob': 'Printed, 32 mm across with a straight knurl. Pushes onto the encoder\'s 6 mm D-shaft. Volume, scrolling and select.',
};

// ---------- labels ----------
// One callout per part: a label in a column either side of the model, joined to the part by a leader line.
const OUTSIDE = new Set(['Base tray', 'Rubber feet', 'Front bezel', 'Rear shell', 'Plinth', 'Access cover', 'Power switch', 'Accent stripe', 'Mid chassis and base', 'E Ink ED103TC2 panel', 'Keycaps', 'Knob', 'USB-C power inlet']);
const ANCHORS_BY = { mk1: { 'Rear shell': [60, 84, 130] }, mk21: { 'Base tray': [200, 8, 4], 'Frame plate': [122, 14, 40] }, mk3: { 'Base tray': [200, 8, 4], 'Frame plate': [122, 14, 40] } };
const ANCHORS = { 'Front bezel': [30, 46.3, 196.3], 'Rear shell': [60, 68.4, 130], 'Plinth': [200, 84, 36], 'Mid chassis and base': [200, 8, 4], 'Keycaps': [152, 60, 197] };
const labels = [];
const SVGNS = 'http://www.w3.org/2000/svg';
let labelsOn = false, labelHover = null;
// Callouts can be filtered by kind. Wires and screws start switched off: that many more labels is a lot.
const BOARDS = new Set(['Waveshare IT8951 driver HAT', 'GPIO splitter', 'Pimoroni Pico Plus 2 W', 'Hard Stuff Pico to Pi HAT', 'Pico sockets and headers', 'Power and amp protoboard',
  'Adafruit PowerBoost 1000C', 'Adafruit MAX98357A amplifier', 'Headers, resistors, capacitors and diode', 'E-paper flex adapter', 'Adafruit NeoKey 1x4 QT', 'Adafruit I2C rotary encoder', 'USB-C power inlet']);
const WIRES = new Set(['Wires to the GPIO header', 'USB-C inlet wires', 'Power switch wires', 'Speaker leads', 'Battery lead', 'STEMMA QT cables']);
const CATS = ['Case', 'Boards', 'Parts', 'Wires', 'Screws'], CATS_DEFAULT = ['Case', 'Boards', 'Parts'];
const cats = new Set(CATS_DEFAULT);
function catOf(name) {
  if (/screws/i.test(name)) return 'Screws';
  if (WIRES.has(name)) return 'Wires';
  if (SHELL.includes(name) || name === 'Accent stripe') return 'Case';
  return BOARDS.has(name) ? 'Boards' : 'Parts';
}
function setCats(list) {
  cats.clear();
  for (const c of list) if (CATS.includes(c)) cats.add(c);
  for (const b of $('labelCats').children) b.setAttribute('aria-pressed', String(cats.has(b.textContent)));
}
for (const c of CATS) {
  const b = document.createElement('button');
  b.type = 'button'; b.textContent = c; b.setAttribute('aria-pressed', String(cats.has(c)));
  b.addEventListener('click', () => { setCats(cats.has(c) ? [...cats].filter((x) => x !== c) : [...cats, c]); track('bedside_label_kind', { kind: c, on: cats.has(c) }); });
  $('labelCats').appendChild(b);
}
function buildLabels() {
  const seen = new Map();
  for (const p of parts) {
    if (p.screen) continue;
    p.mesh.geometry.computeBoundingBox();
    const id = p.name + '|' + (p.variants || []).join();       // each case version gets its own callouts
    if (!seen.has(id)) seen.set(id, { box: new THREE.Box3(), part: p });
    seen.get(id).box.union(p.mesh.geometry.boundingBox);
  }
  for (const { box, part } of seen.values()) {
    const name = part.name;
    const el = document.createElement('div');
    el.className = 'lab'; el.textContent = name;
    el.addEventListener('pointerenter', () => { labelHover = name; setHover(name); });
    el.addEventListener('pointerleave', () => { labelHover = null; pointerMoved = true; });
    $('labels').appendChild(el);
    const path = document.createElementNS(SVGNS, 'path'), dot = document.createElementNS(SVGNS, 'circle');
    dot.setAttribute('r', '2.5');
    $('leaders').append(path, dot);
    const fixed = (part.variants && (ANCHORS_BY[part.variants[0]] || {})[name]) || ANCHORS[name];
    // a set of screws has no useful centre, so its callout points at the first screw
    const anchor = fixed ? new THREE.Vector3(...fixed) : /screws/i.test(name) ? new THREE.Vector3().fromBufferAttribute(part.mesh.geometry.attributes.position, 0) : box.getCenter(new THREE.Vector3());
    labels.push({ name, cat: catOf(name), el, path, dot, anchor, part, w: 0, h: 0, sx: 0, sy: 0, y: 0, alpha: 0 });
  }
}
function setLabels(on) {
  labelsOn = on;
  document.body.classList.toggle('labels', on);
  $('labelsToggle').setAttribute('aria-pressed', String(on));
}
$('labelsToggle').addEventListener('click', () => { setLabels(!labelsOn); track('bedside_labels', { on: labelsOn }); });

const proj = new THREE.Vector3();
function spread(col, yMin, yMax, step) {            // keep labels in anchor order, at least one step apart, inside the band
  col.sort((a, b) => a.ty - b.ty);
  let y = yMin;
  for (const l of col) { l.y = Math.max(l.ty, y); y = l.y + step; }
  y = yMax;
  for (let i = col.length - 1; i >= 0; i--) { col[i].y = Math.min(col[i].y, y); y = col[i].y - step; }
}
function screenBounds(W, H) {                        // the model's outline on screen, from the corners of every visible part's box
  const b = { x0: 1e9, x1: -1e9, y0: 1e9, y1: -1e9 };
  for (const p of parts) {
    const g = p.mesh.geometry.boundingBox;
    if (!p.mesh.visible || !g) continue;
    for (let i = 0; i < 8; i++) {
      proj.set(i & 1 ? g.max.x : g.min.x, i & 2 ? g.max.y : g.min.y, i & 4 ? g.max.z : g.min.z).add(p.mesh.position).project(camera);
      const x = (proj.x * 0.5 + 0.5) * W, y = (-proj.y * 0.5 + 0.5) * H;
      b.x0 = Math.min(b.x0, x); b.x1 = Math.max(b.x1, x); b.y0 = Math.min(b.y0, y); b.y1 = Math.max(b.y1, y);
    }
  }
  return b;
}
function layoutLabels(e1) {
  const W = window.innerWidth, H = window.innerHeight, phone = W <= 640;
  const open = state.hidden.has('Rear shell') && (state.design === 'mk1' || state.hidden.has('Plinth'));
  const inside = open ? 1 : Math.min(1, Math.max(0, (e1 - 0.18) / 0.3));
  const live = [];
  for (const l of labels) {
    proj.copy(l.anchor).add(l.part.mesh.position).project(camera);
    l.sx = (proj.x * 0.5 + 0.5) * W; l.sy = (-proj.y * 0.5 + 0.5) * H;
    const outside = OUTSIDE.has(l.name) || l.name.startsWith('Back screws');
    l.alpha = !cats.has(l.cat) || !l.part.mesh.visible || proj.z > 1 ? 0 : (outside ? 1 : inside);
    if (l.alpha > 0.02) { if (!l.w) { l.w = l.el.offsetWidth; l.h = l.el.offsetHeight; } live.push(l); }
    else { l.el.style.opacity = 0; l.path.style.opacity = 0; l.dot.style.opacity = 0; }
  }
  if (!live.length) return;
  live.sort((a, b) => a.sx - b.sx);
  const half = Math.ceil(live.length / 2), left = live.slice(0, half), right = live.slice(half);
  const step = phone ? 21 : 29, edge = 8, gap = phone ? 16 : 56;
  const wl = Math.max(...left.map((l) => l.w)), wr = Math.max(0, ...right.map((l) => l.w));
  // Each column stands just off the model's outline, not at the edge of the window, and only falls back to the edge when there is no room
  const b = screenBounds(W, H);
  let xl = Math.max(edge + wl, b.x0 - gap), xr = Math.min(W - edge - wr, b.x1 + gap);
  const yTop = phone ? 20 : 60, yBottom = H - (phone ? $('controls').offsetHeight + 26 : 28);
  const band = (n) => {                                // a band around the model's height, grown evenly if the labels need more
    const need = n * step, mid = (b.y0 + b.y1) / 2, halfBand = Math.max((b.y1 - b.y0) / 2 + 40, need / 2);
    return [Math.max(yTop, mid - halfBand), Math.min(yBottom, mid + halfBand)];
  };
  let [l0, l1] = band(left.length);
  const [r0, r1] = band(right.length);
  if (!phone) {                                        // keep the left column off the controls panel: above it if it fits, otherwise beside it
    const panelBox = $('controls').getBoundingClientRect();
    if (xl - wl < panelBox.right + 12 && l1 > panelBox.top - 14) {
      if (left.length * step <= panelBox.top - 14 - yTop) { l1 = panelBox.top - 14; l0 = Math.min(l0, l1 - left.length * step); }
      else xl = Math.max(xl, panelBox.right + 12 + wl);
    }
  }
  // aim each label a little above or below its part, so every leader gets a level run and then a slanted one
  const midY = (b.y0 + b.y1) / 2;
  for (const l of live) l.ty = l.sy + (l.sy < midY ? -34 : 34);
  spread(left, l0, l1, step);
  spread(right, r0, r1, step);
  for (const [col, isLeft] of [[left, true], [right, false]]) {
    for (const l of col) {
      // leader: level out of the label, then one 45 degree run into the part
      const start = isLeft ? xl + 4 : xr - 4, rise = Math.abs(l.sy - l.y);
      const elbow = isLeft ? Math.max(start + 14, l.sx - rise) : Math.min(start - 14, l.sx + rise);
      l.el.style.transform = `translate(${(isLeft ? xl - l.w : xr).toFixed(1)}px, ${(l.y - l.h / 2).toFixed(1)}px)`;
      l.el.style.opacity = l.alpha;
      l.el.classList.toggle('hot', hovered === l.name);
      l.path.setAttribute('d', `M${start.toFixed(1)} ${l.y.toFixed(1)}H${elbow.toFixed(1)}L${l.sx.toFixed(1)} ${l.sy.toFixed(1)}`);
      l.path.style.opacity = l.alpha * 0.85;
      l.dot.setAttribute('cx', l.sx.toFixed(1)); l.dot.setAttribute('cy', l.sy.toFixed(1));
      l.dot.style.opacity = l.alpha;
    }
  }
}

// ---------- screw sizes ----------
// A second kind of callout: a small tag on every single screw giving its size. A set of screws is one mesh, so the screws are found by splitting
// the mesh into its connected pieces; the size comes from the set's name, or for the sets that mix lengths, from the version and the screw's length.
const sizeTags = [];
let sizesOn = false;
function screwSize(part, long) {
  const m = / (M[\d.]+ x \d+)$/.exec(part.name);
  if (m) return m[1];
  const mk3 = (part.variants || []).includes('mk3');
  if (part.name.startsWith('Base screws')) return mk3 ? 'M3 x 16' : 'M3 x 12';
  if (part.name.startsWith('Speaker screws')) return mk3 ? 'M4 x 10' : 'M4 x 8';
  if (part.name.startsWith('Frame screws')) return mk3 && long > 15 ? 'M3 x 12' : 'M3 x 8';      // the four in the plate's foot are the long ones
  return (/M[\d.]+/.exec(part.name) || ['?'])[0];
}
function buildSizeTags() {
  for (const part of parts) {
    if (!isScrew(part.name)) continue;
    const pos = part.mesh.geometry.attributes.position, idx = part.mesh.geometry.index.array, n = pos.count;
    const parent = new Int32Array(n), first = new Map();
    const find = (i) => { while (parent[i] !== i) { parent[i] = parent[parent[i]]; i = parent[i]; } return i; };
    for (let i = 0; i < n; i++) {                       // corners that share a place count as one, so a screw's faces join up
      const k = Math.round(pos.getX(i) * 50) + '_' + Math.round(pos.getY(i) * 50) + '_' + Math.round(pos.getZ(i) * 50);
      if (first.has(k)) parent[i] = first.get(k); else { first.set(k, i); parent[i] = i; }
    }
    for (let t = 0; t < idx.length; t += 3) { const a = find(idx[t]); parent[find(idx[t + 1])] = a; parent[find(idx[t + 2])] = a; }
    const boxes = new Map(), v = new THREE.Vector3();
    for (let i = 0; i < n; i++) {
      const r = find(i);
      if (!boxes.has(r)) boxes.set(r, new THREE.Box3());
      boxes.get(r).expandByPoint(v.fromBufferAttribute(pos, i));
    }
    for (const box of boxes.values()) {
      const size = screwSize(part, box.getSize(v).length());
      const el = document.createElement('div');
      el.className = 'tag d' + /M([\d.]+)/.exec(size)[1].replace('.', '');
      el.textContent = size; el.title = part.name;
      $('sizes').appendChild(el);
      sizeTags.push({ el, part, anchor: box.getCenter(new THREE.Vector3()), on: false });
    }
  }
}
function setSizes(on) {
  sizesOn = on;
  document.body.classList.toggle('sizes', on);
  $('sizesToggle').setAttribute('aria-pressed', String(on));
}
$('sizesToggle').addEventListener('click', () => { setSizes(!sizesOn); track('bedside_screw_sizes', { on: sizesOn }); });
function layoutSizes() {
  const W = window.innerWidth, H = window.innerHeight;
  for (const t of sizeTags) {
    proj.copy(t.anchor).add(t.part.mesh.position).project(camera);
    const on = t.part.mesh.visible && proj.z < 1;
    if (on) t.el.style.transform = `translate(${((proj.x * 0.5 + 0.5) * W).toFixed(1)}px, ${((-proj.y * 0.5 + 0.5) * H).toFixed(1)}px) translate(-50%, -150%)`;
    if (on !== t.on) { t.on = on; t.el.style.opacity = on ? 1 : 0; }
  }
}

// ---------- frame loop ----------
// On a wide screen the open controls panel covers the left of the window, so the model is centred in what is left over
let viewShift = 0, shiftApplied = null;
function resize() {
  const w = window.innerWidth, h = window.innerHeight;
  renderer.setSize(w, h, false);
  camera.aspect = w / h;
  camera.fov = w < h ? 46 : 32;         // portrait phones need a wider view to fit the model
  camera.updateProjectionMatrix();
  shiftApplied = null;                 // the view offset is tied to the window size, so put it back on the next frame
}
window.addEventListener('resize', resize);
resize();

const clock = new THREE.Clock();
const tmp = new THREE.Vector3();
let lastFit = 1;
function frame() {
  const dt = Math.min(clock.getDelta(), 0.05);
  const k = 1 - Math.exp(-dt * 9);
  shown.explode += (state.explode - shown.explode) * k;
  shown.parts += (state.parts - shown.parts) * k;
  const e1 = shown.explode * shown.explode * (3 - 2 * shown.explode);     // ease the ends of each slider
  const e2 = shown.parts * shown.parts * (3 - 2 * shown.parts);
  if (build) stepBuild(dt);
  if (flash) stepFlash(dt);
  buildSpread += ((build && build.dir !== 0 ? 1 : 0) - buildSpread) * (1 - Math.exp(-dt * 2));
  for (const p of parts) {
    p.mesh.position.copy(p.e1).multiplyScalar(e1).addScaledVector(p.e2, e2);
    if (SPLIT.has(state.design) && ON_TRAY.has(p.name)) p.mesh.position.z -= TRAY_DROP * e1;      // from MK II.I on the base tray is its own part, so it drops away from the frame plate with everything fixed to it
    // MK III has the USB-C inlet and power switch 6.1 mm nearer the speaker, to line the panel up with the hump; the inlet and both sets of wires are exported there.
    if (state.design !== 'mk3' && PANEL_PARTS.has(p.name)) p.mesh.position.x += 6.1;
    // From MK II.I on the battery is 12 mm nearer the speaker, and it is exported there. The older versions put it back, and stretch its lead from the PowerBoost end (x 75) to match.
    if (p.name === 'LiPo battery, 4400 mAh' && !SPLIT.has(state.design)) p.mesh.position.x += 12;
    if (p.name === 'Battery lead') {
      const k = SPLIT.has(state.design) ? 1 : 1.255;
      p.mesh.scale.x = k; p.mesh.position.x += 75 * (1 - k);
    }
    if (build) placeBuild(p);
    if (flash) placeFlash(p);
  }
  for (const { group, parent } of keyLids) {             // the lids fade in and out with their caps
    const o = parent.mesh.material.opacity, solid = o >= 0.999;
    group.traverse((n) => { if (n.material) { n.material.opacity = o; n.material.transparent = !solid; n.material.depthWrite = solid; } });
  }
  // pull the camera back as the model spreads out, keeping whatever zoom the viewer chose
  const fit = 1 + 0.55 * e1 + 0.12 * e2 + 0.3 * buildSpread;
  if (Math.abs(fit - lastFit) > 1e-5) {
    const ratio = fit / lastFit;
    camera.position.sub(controls.target).multiplyScalar(ratio).add(controls.target);
    if (flight) { flight.from.sub(controls.target).multiplyScalar(ratio).add(controls.target); flight.to.sub(controls.target).multiplyScalar(ratio).add(controls.target); }
    lastFit = fit;
  }

  if (flight) {
    flight.t = Math.min(1, flight.t + dt * (flight.rate || 1.6));
    const s = flight.t * flight.t * (3 - 2 * flight.t);
    camera.position.lerpVectors(flight.from, flight.to, s);
    if (flight.aim) controls.target.lerpVectors(flight.aimFrom, flight.aim, s);
    if (flight.t >= 1) flight = null;
  } else if (build && !build.free && (build.dir !== 0 || build.only != null)) {
    // lead the camera round to the side the current module goes on from, at whatever distance the viewer chose
    const [az, el] = build.only != null ? build.view[build.only] : buildView(build.T), ease = 1 - Math.exp(-dt * 5);
    tmp.copy(camera.position).sub(controls.target);
    const r = tmp.length(), az0 = Math.atan2(tmp.y, tmp.x), el0 = Math.asin(tmp.z / r);
    const a = az0 + Math.atan2(Math.sin(az - az0), Math.cos(az - az0)) * ease, e = el0 + (el - el0) * ease;
    camera.position.set(Math.cos(e) * Math.cos(a), Math.cos(e) * Math.sin(a), Math.sin(e)).multiplyScalar(r).add(controls.target);
  } else if (state.spin > 0 && !pointerDown) {
    tmp.copy(camera.position).sub(controls.target).applyAxisAngle(camera.up, dt * state.spin * 0.12);
    camera.position.copy(controls.target).add(tmp);
  }
  controls.update();

  const wantShift = !phone.matches && panel.classList.contains('open') ? 154 : 0;
  viewShift += (wantShift - viewShift) * k;
  if (shiftApplied === null || Math.abs(viewShift - shiftApplied) > 0.4) {
    shiftApplied = Math.abs(wantShift - viewShift) < 0.4 ? wantShift : viewShift;
    camera.setViewOffset(window.innerWidth, window.innerHeight, -shiftApplied, 0, window.innerWidth, window.innerHeight);
    camera.updateProjectionMatrix();
  }

  applyWire(dt);
  if (demo) stepDemo(dt);

  if (labelsOn) layoutLabels(e1);
  if (sizesOn) layoutSizes();

  if (pointerMoved && !pointerDown && !labelHover) {
    pointerMoved = false;
    ray.setFromCamera(pointer, camera);
    const hit = ray.intersectObjects(model.children.filter((m) => m.visible && !(wire && SHELL.includes(m.userData.name))), false)[0];
    setHover(hit ? hit.object.userData.name : null);
  }
  renderer.render(scene, camera);
  requestAnimationFrame(frame);
}

load().then(() => {
  const q = new URLSearchParams(location.hash.slice(1));
  const stored = (k) => { try { return localStorage.getItem(k); } catch (e) { return null; } };
  const mk = Object.keys(MK_HASH).find((k) => MK_HASH[k] === q.get('mk'));
  setDesign(mk || stored('bedside-version'));
  const fin = q.get('case');
  setFinish(fin ? fin[0].toUpperCase() + fin.slice(1).toLowerCase() : (stored('bedside-finish') || 'Beige'));
  setBackdrop(q.get('bg') || stored('bedside-backdrop'));
  buildKeyMarks();
  setMarks(Object.keys(MARKS).find((n) => n.toLowerCase().replace(/ /g, '-') === q.get('keys')) || stored('bedside-marks'), false);
  buildLabels();
  try { localStorage.removeItem('bedside-labels'); } catch (e) { /* storage blocked */ }   // labels are never remembered: off unless the link asks for them
  setLabels(q.get('labels') === '1');
  buildSizeTags();
  setSizes(q.get('sizes') === '1');
  if (q.get('wire') === '1') setWire(true);
  if (q.get('elec') === '0') { elec = false; $('elecToggle').setAttribute('aria-pressed', 'false'); }
  applyURL(q);
  applyVisibility();
  if (q.has('embed')) { document.documentElement.dataset.embed = '1'; setOpen(false); }      // inside the assembly guide: no controls, just the model
  if (applyHashStep(q)) { /* one step of the build, or the flashing view */ }
  else if (q.has('demo')) startDemo(q.get('demo') === 'loop');
  else if (q.has('build')) startBuild(1, q.get('build') === 'loop');
  frame();
}).catch((err) => {
  const el = document.getElementById('loading');
  if (el) el.textContent = 'Could not load the model';
  console.error(err);
});
