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

// Three versions of the case share one set of components. MK I is the chamfered wedge, MK II the leaning slab on a black plinth,
// and MK II.I is MK II reworked for printing: split chassis, fit gaps, vented slats, a speaker grille and a deeper plinth.
const DESIGNS = { 'MK I': 'mk1', 'MK II': 'mk2', 'MK II.I': 'mk21' };
const MK_HASH = { mk1: '1', mk2: '2', mk21: '2.1' };
const CASE_PARTS = ['Front bezel', 'Rear shell', 'Plinth', 'Access cover'];
const PLINTH_WIRES = new Set(['USB-C inlet wires', 'Power switch wires']);
const NOT_MK1 = ['Plinth', 'Access cover'];          // case toggles that mean nothing on MK I
const isScrew = (name) => /screws/i.test(name);
// Case finishes. Beige is the two-tone case as modelled: beige frame, black chassis and plinth. Black gets a light backdrop so the shape still reads.
const SHELL = ['Front bezel', 'Mid chassis and base', 'Base tray', 'Frame plate', 'Rear shell', 'Plinth', 'Access cover'];
const FINISHES = { Beige: { colour: null, light: false }, Black: { colour: '#1b1c1f', light: true } };
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
    p.mesh.material.color.copy(f.colour ? new THREE.Color(f.colour) : p.mesh.userData.base);
    p.mesh.material.roughness = f.colour ? 0.62 : 0.72;
  }
  applyBackdrop();
  for (const b of $('finish').children) b.setAttribute('aria-pressed', String(b.textContent === state.finish));
  if (save) try { localStorage.setItem('bedside-finish', state.finish); } catch (e) { /* storage blocked */ }
  if (hovered) { const h = hovered; hovered = null; setHover(h); }
}

async function load() {
  const get = (stem) => [fetch(`./data/${stem}.json`).then((r) => r.json()), fetch(`./data/${stem}.bin`).then((r) => r.arrayBuffer())];
  const [meta, bin, meta1, bin1, meta2, bin2] = await Promise.all([...get('bedside'), ...get('mk1'), ...get('mk2')]);
  // bedside.* holds the components plus the newest case; mk1.* and mk2.* hold the earlier cases. Each case part says which versions it belongs to.
  const own = (name) => SHELL.includes(name) || name === 'Power switch' || isScrew(name);
  const wires = (name) => (name === 'Power switch wires' ? ['mk2', 'mk21'] : null);      // MK I has no switch
  addParts(meta1, bin1, () => ['mk1']);
  addParts(meta2, bin2, (name) => (isScrew(name) && !name.startsWith('Back screws') ? ['mk1', 'mk2'] : ['mk2']));    // MK I borrows MK II's screws
  addParts(meta, bin, (name) => (own(name) ? ['mk21'] : wires(name)));
  await addScreen(meta);
  document.getElementById('loading').remove();
}
function addParts(meta, bin, variantOf) {
  for (const p of meta.parts) {
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
    parts.push({ mesh, name: p.name, e1: new THREE.Vector3(...p.e1), e2: new THREE.Vector3(...p.e2), variants: variantOf(p.name) });
  }
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
  parts.push({ mesh: screen, name: screen.userData.name, e1: new THREE.Vector3(...s.e1), e2: new THREE.Vector3(...s.e2), screen: true });
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
  try { localStorage.setItem('bedside-version', state.design); } catch (e) { /* storage blocked */ }
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
    const c = p.lines.material.color.copy(f.colour ? new THREE.Color(f.colour) : p.mesh.userData.base);
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

function applyVisibility() {
  for (const p of parts) {
    const hide = state.hidden.has(p.name) || (p.variants && !p.variants.includes(state.design)) || (p.name === 'Accent stripe' && state.hidden.has('Front bezel'));
    p.mesh.visible = !hide;
  }
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
  stopDemo(false);
  track('bedside_reset');
  state.explode = state.parts = state.spin = 0;
  for (const k of Object.keys(ranges)) ranges[k].value = 0;
  state.hidden.clear();
  setFinish('Beige');
  setLabels(false);
  setCats(CATS_DEFAULT);
  setWire(false);
  setDesign('mk2');
  setBackdrop('Auto');
  for (const b of $('show').children) b.setAttribute('aria-pressed', 'true');
  applyVisibility();
  controls.target.copy(CENTRE);
  flight = { from: camera.position.clone(), to: new THREE.Vector3(...VIEWS.Front).add(CENTRE), t: 0 };
});

// ---------- share ----------
// Everything on screen goes into the link's hash: version, colours, sliders, hidden parts, wireframe, labels and the camera.
const SLUGS = { 'Front bezel': 'bezel', 'Rear shell': 'shell', Plinth: 'plinth', 'Access cover': 'cover' };
function shareURL() {
  const q = new URLSearchParams();
  q.set('mk', MK_HASH[state.design]);
  q.set('case', state.finish.toLowerCase());
  q.set('bg', state.bg.toLowerCase());
  if (state.explode > 0) q.set('x', state.explode.toFixed(2));
  if (state.parts > 0) q.set('c', state.parts.toFixed(2));
  if (state.spin > 0) q.set('spin', state.spin.toFixed(1));
  if (state.hidden.size) q.set('hide', [...state.hidden].map((n) => SLUGS[n]).filter(Boolean).join('_'));
  if (wire) q.set('wire', '1');
  if (labelsOn) { q.set('labels', '1'); if ([...cats].sort().join() !== [...CATS_DEFAULT].sort().join()) q.set('lab', [...cats].join('_').toLowerCase()); }
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

// ---------- hover ----------
const ray = new THREE.Raycaster();
const pointer = new THREE.Vector2(2, 2);
let hovered = null, pointerDown = false, pointerMoved = true;
const tip = $('tip');
canvas.addEventListener('pointermove', (e) => {
  pointer.set((e.clientX / window.innerWidth) * 2 - 1, -(e.clientY / window.innerHeight) * 2 + 1);
  pointerMoved = true;
});
canvas.addEventListener('pointerdown', () => { pointerDown = true; });
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
  'Frame plate': 'Printed case, in black. The boards, keys and encoder mount to this; it screws to the base tray.',
  'Base tray': 'Printed case, in black. Carries the battery, speaker, USB-C inlet and power switch.',
  'Frame screws, M3 x 8': 'Four, up through the base tray into the frame plate.',
  'Rear shell': 'Printed case. Slides off the back.',
  'Accent stripe': 'Orange insert, matching the menu key.',
  'Access cover': 'Snaps on over the Pico\'s USB-C port, for reflashing by cable.',
  'Bezel screws, M3 x 8': 'Six, through the chassis plate into the bezel.',
  'Base screws, M3 x 12': 'Four, up through the base into the shell and plinth posts.',
  'Back screws, M3 x 8': 'Four, through the back of the rear shell into chassis posts.',
  'Driver HAT screws, M2.5 x 8': 'Two; the other two corners sit on locating pins.',
  'Power board screws, M2 x 6': 'Four, one per corner.',
  'Flex adapter screws, M2 x 5': 'Four, one per corner.',
  'Key module screws, M2.5 x 8': 'Four, down through the NeoKey into the fins.',
  'Speaker screws and nuts, M4 x 8': 'Two, through the lugs and the baffle.',
  'USB-C inlet screws, M2.5 x 8': 'Two, down into the pedestal.',
  'Power switch': 'Mini toggle, fixed to the chassis beside the USB-C inlet.',
  'Plinth': 'Printed case, in black. Covers the battery, speaker and USB-C inlet.',
  'E Ink ED103TC2 panel': '10.3 inch, 1872 × 1404, 16 greys.',
  'Waveshare IT8951 driver HAT': 'Drives the e-paper panel over SPI.',
  'GPIO splitter': 'Doubles the 40-pin header: one for the HAT, one for wiring.',
  'Pimoroni Pico Plus 2 W': 'RP2350B, 16 MB flash, 8 MB PSRAM, Wi-Fi.',
  'Hard Stuff Pico to Pi HAT': 'Adapts the Pico to the Raspberry Pi header.',
  'Pico sockets and headers': 'The Pico plugs in, so it can come out.',
  'Wires to the GPIO header': 'Soldered into the power board, plugged onto the splitter: 5 V, ground, I2S audio and the three sense lines.',
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
  'E-paper flex adapter': 'Joins the panel flex to the ribbon for the HAT.',
  'Adafruit NeoKey 1x4 QT': 'Four key sockets with backlights, on I2C.',
  'Cherry MX Brown switches': 'Tactile, quiet enough for a bedroom.',
  'Keycaps': 'Three grey, one orange for the menu.',
  'Adafruit I2C rotary encoder': 'Detented, with a push switch.',
  'Knob': 'Volume, scrolling and select.',
};

// ---------- labels ----------
// One callout per part: a label in a column either side of the model, joined to the part by a leader line.
const OUTSIDE = new Set(['Base tray', 'Front bezel', 'Rear shell', 'Plinth', 'Access cover', 'Power switch', 'Accent stripe', 'Mid chassis and base', 'E Ink ED103TC2 panel', 'Keycaps', 'Knob', 'USB-C power inlet']);
const ANCHORS_BY = { mk1: { 'Rear shell': [60, 84, 130] }, mk21: { 'Plinth': [200, 96, 36], 'Base tray': [200, 8, 4], 'Frame plate': [122, 14, 40] } };
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
  for (const p of parts) {
    p.mesh.position.copy(p.e1).multiplyScalar(e1).addScaledVector(p.e2, e2);
    if (p.name === 'USB-C power inlet' && state.design !== 'mk21') p.mesh.position.y -= 12;     // MK II.I's plinth is 12 mm deeper, and the inlet is exported in that place
    if (PLINTH_WIRES.has(p.name)) {
      // these run from the power board (y 34) to the back of the plinth (y 84 on MK II.I), so on the shallower versions they are squeezed to end 12 mm sooner
      const k = state.design === 'mk21' ? 1 : 0.76;
      p.mesh.scale.y = k; p.mesh.position.y += 34 * (1 - k);
    }
  }
  // pull the camera back as the model spreads out, keeping whatever zoom the viewer chose
  const fit = 1 + 0.55 * e1 + 0.12 * e2;
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
    if (flight.t >= 1) flight = null;
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
  buildLabels();
  try { localStorage.removeItem('bedside-labels'); } catch (e) { /* storage blocked */ }   // labels are never remembered: off unless the link asks for them
  setLabels(q.get('labels') === '1');
  if (q.get('wire') === '1') setWire(true);
  applyURL(q);
  if (q.has('demo')) startDemo(q.get('demo') === 'loop');
  applyVisibility(); frame();
}).catch((err) => {
  const el = document.getElementById('loading');
  if (el) el.textContent = 'Could not load the model';
  console.error(err);
});
