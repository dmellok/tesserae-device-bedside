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

// Two versions of the case share one set of components. MK I is the chamfered wedge; MK II is the leaning slab on a black plinth.
const DESIGNS = { 'MK I': 'mk1', 'MK II': 'mk2' };
const CASE_PARTS = ['Front bezel', 'Rear shell', 'Plinth', 'Access cover'];
const MK2_ONLY = ['Plinth', 'Access cover', 'Power switch'];
// Case finishes. Beige is the two-tone case as modelled: beige frame, black chassis and plinth. Black gets a light backdrop so the shape still reads.
const SHELL = ['Front bezel', 'Mid chassis and base', 'Rear shell', 'Plinth', 'Access cover'];
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
  document.documentElement.classList.toggle('light', b.light);
  document.querySelector('meta[name=theme-color]').content = b.theme;
  paintWire();
}
function setBackdrop(name) {
  state.bg = Object.keys(BACKDROPS).find((k) => k.toLowerCase() === String(name || '').toLowerCase()) || 'Auto';
  for (const b of $('backdrop').children) b.setAttribute('aria-pressed', String(b.textContent === state.bg));
  try { localStorage.setItem('bedside-backdrop', state.bg); } catch (e) { /* storage blocked */ }
  applyBackdrop();
}
function setFinish(name) {
  const f = FINISHES[name] || FINISHES.Beige;
  state.finish = FINISHES[name] ? name : 'Beige';
  for (const p of parts) {
    if (p.screen || !SHELL.includes(p.name)) continue;
    p.mesh.material.color.copy(f.colour ? new THREE.Color(f.colour) : p.mesh.userData.base);
    p.mesh.material.roughness = f.colour ? 0.62 : 0.72;
  }
  applyBackdrop();
  for (const b of $('finish').children) b.setAttribute('aria-pressed', String(b.textContent === state.finish));
  try { localStorage.setItem('bedside-finish', state.finish); } catch (e) { /* storage blocked */ }
  if (hovered) { const h = hovered; hovered = null; setHover(h); }
}

async function load() {
  const [meta, bin, meta1, bin1] = await Promise.all([
    fetch('./data/bedside.json').then((r) => r.json()),
    fetch('./data/bedside.bin').then((r) => r.arrayBuffer()),
    fetch('./data/mk1.json').then((r) => r.json()),
    fetch('./data/mk1.bin').then((r) => r.arrayBuffer()),
  ]);
  addParts(meta1, bin1, () => 'mk1');
  addParts(meta, bin, (name) => (SHELL.includes(name) || MK2_ONLY.includes(name) ? 'mk2' : null));
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
    parts.push({ mesh, name: p.name, e1: new THREE.Vector3(...p.e1), e2: new THREE.Vector3(...p.e2), variant: variantOf(p.name) });
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
  for (const b of $('show').children) if (MK2_ONLY.includes(b.textContent)) b.style.display = state.design === 'mk1' ? 'none' : '';   // MK I has no plinth or access cover
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
function setWire(on) {
  wire = on;
  for (const p of parts) {
    if (p.screen || !SHELL.includes(p.name)) continue;
    if (on && !p.lines) {                 // built on first use: only the creases, not every triangle
      p.lines = new THREE.LineSegments(new THREE.EdgesGeometry(p.mesh.geometry, 28), new THREE.LineBasicMaterial({ transparent: true, opacity: 0.9 }));
      p.lines.raycast = () => {};
      p.mesh.add(p.lines);
    }
    if (p.lines) p.lines.visible = on;
    const m = p.mesh.material;
    m.transparent = on; m.opacity = on ? 0.07 : 1; m.depthWrite = !on; m.needsUpdate = true;
  }
  paintWire();
  $('wireToggle').setAttribute('aria-pressed', String(on));
  pointerMoved = true;
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
    const hide = state.hidden.has(p.name) || (p.variant && p.variant !== state.design) || (p.name === 'Accent stripe' && state.hidden.has('Front bezel'));
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
  track('bedside_reset');
  state.explode = state.parts = state.spin = 0;
  for (const k of Object.keys(ranges)) ranges[k].value = 0;
  state.hidden.clear();
  setFinish('Beige');
  setLabels(false);
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
  q.set('mk', state.design === 'mk1' ? '1' : '2');
  q.set('case', state.finish.toLowerCase());
  q.set('bg', state.bg.toLowerCase());
  if (state.explode > 0) q.set('x', state.explode.toFixed(2));
  if (state.parts > 0) q.set('c', state.parts.toFixed(2));
  if (state.spin > 0) q.set('spin', state.spin.toFixed(1));
  if (state.hidden.size) q.set('hide', [...state.hidden].map((n) => SLUGS[n]).filter(Boolean).join('_'));
  if (wire) q.set('wire', '1');
  if (labelsOn) q.set('labels', '1');
  const eye = camera.position.clone().sub(controls.target).divideScalar(lastFit).add(controls.target);   // without the explode pull-back, which is re-applied on load
  q.set('cam', [...eye.toArray(), ...controls.target.toArray()].map(Math.round).join('_'));
  return location.origin + location.pathname + '#' + q.toString();
}
function applyURL(q) {
  for (const [key, id] of [['x', 'explode'], ['c', 'parts'], ['spin', 'spin']]) {
    const v = parseFloat(q.get(key)), el = ranges[id];
    if (Number.isFinite(v)) { state[id] = Math.min(parseFloat(el.max), Math.max(0, v)); el.value = state[id]; }
  }
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
  'Rear shell': 'Printed case. Slides off the back.',
  'Accent stripe': 'Orange insert, matching the menu key.',
  'Access cover': 'Snaps on over the Pico\'s USB-C port, for reflashing by cable.',
  'Screws': 'M3 for the case, M2 and M2.5 for the boards, M4 for the speaker.',
  'Power switch': 'Mini toggle, fixed to the chassis beside the USB-C inlet.',
  'Plinth': 'Printed case, in black. Covers the battery, speaker and USB-C inlet.',
  'E Ink ED103TC2 panel': '10.3 inch, 1872 × 1404, 16 greys.',
  'Waveshare IT8951 driver HAT': 'Drives the e-paper panel over SPI.',
  'GPIO splitter': 'Doubles the 40-pin header: one for the HAT, one for wiring.',
  'Pimoroni Pico Plus 2 W': 'RP2350B, 16 MB flash, 8 MB PSRAM, Wi-Fi.',
  'Hard Stuff Pico to Pi HAT': 'Adapts the Pico to the Raspberry Pi header.',
  'Pico sockets and headers': 'The Pico plugs in, so it can come out.',
  'Wire harness to the power board': 'Power, I2S audio and battery sense lines.',
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
const OUTSIDE = new Set(['Front bezel', 'Rear shell', 'Plinth', 'Access cover', 'Power switch', 'Accent stripe', 'Mid chassis and base', 'E Ink ED103TC2 panel', 'Keycaps', 'Knob', 'USB-C power inlet']);
const ANCHORS_MK1 = { 'Rear shell': [60, 84, 130] };
const ANCHORS = { 'Front bezel': [30, 46.3, 196.3], 'Rear shell': [60, 68.4, 130], 'Plinth': [200, 84, 36], 'Mid chassis and base': [200, 8, 4], 'Keycaps': [152, 60, 197] };
const labels = [];
const SVGNS = 'http://www.w3.org/2000/svg';
let labelsOn = false, labelHover = null;
function buildLabels() {
  const seen = new Map();
  for (const p of parts) {
    if (p.screen || p.name === 'Screws') continue;       // screws are scattered all over, so one callout would point at nothing
    p.mesh.geometry.computeBoundingBox();
    const id = p.name + '|' + (p.variant || '');       // each case version gets its own callouts
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
    const fixed = (part.variant === 'mk1' && ANCHORS_MK1[name]) || ANCHORS[name];
    const anchor = fixed ? new THREE.Vector3(...fixed) : box.getCenter(new THREE.Vector3());
    labels.push({ name, el, path, dot, anchor, part, w: 0, h: 0, sx: 0, sy: 0, y: 0, alpha: 0 });
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
  col.sort((a, b) => a.sy - b.sy);
  let y = yMin;
  for (const l of col) { l.y = Math.max(l.sy, y); y = l.y + step; }
  y = yMax;
  for (let i = col.length - 1; i >= 0; i--) { col[i].y = Math.min(col[i].y, y); y = col[i].y - step; }
}
function layoutLabels(e1) {
  const W = window.innerWidth, H = window.innerHeight, phone = W <= 640;
  const inside = state.hidden.has('Rear shell') && (state.design === 'mk1' || state.hidden.has('Plinth')) ? 1 : Math.min(1, Math.max(0, (e1 - 0.18) / 0.3));
  const live = [];
  for (const l of labels) {
    proj.copy(l.anchor).add(l.part.mesh.position).project(camera);
    l.sx = (proj.x * 0.5 + 0.5) * W; l.sy = (-proj.y * 0.5 + 0.5) * H;
    l.alpha = !l.part.mesh.visible || proj.z > 1 ? 0 : (OUTSIDE.has(l.name) ? 1 : inside);
    if (l.alpha > 0.02) { if (!l.w) { l.w = l.el.offsetWidth; l.h = l.el.offsetHeight; } live.push(l); }
    else { l.el.style.opacity = 0; l.path.style.opacity = 0; l.dot.style.opacity = 0; }
  }
  live.sort((a, b) => a.sx - b.sx);
  const half = Math.ceil(live.length / 2), left = live.slice(0, half), right = live.slice(half);
  const step = phone ? 21 : 29;
  const yMax = H - (phone ? $('controls').offsetHeight + 26 : 28);
  // On a wide screen the left column sits above the controls panel when it fits there, and beside the panel when it doesn't
  const panelTop = $('controls').getBoundingClientRect().top - 16;
  const above = !phone && left.length * step <= panelTop - 86;
  spread(left, phone ? 20 : 86, above ? panelTop : yMax, step);
  spread(right, phone ? 20 : 96, yMax, step);
  const xl = phone ? 8 : (above ? 16 : 330), xr = W - (phone ? 8 : 16);
  for (const [col, isLeft] of [[left, true], [right, false]]) {
    for (const l of col) {
      const x = isLeft ? xl : xr - l.w, edge = isLeft ? xl + l.w : xr - l.w, elbow = edge + (isLeft ? 12 : -12);
      l.el.style.transform = `translate(${x.toFixed(1)}px, ${(l.y - l.h / 2).toFixed(1)}px)`;
      l.el.style.opacity = l.alpha;
      l.el.classList.toggle('hot', hovered === l.name);
      l.path.setAttribute('d', `M${edge.toFixed(1)} ${l.y.toFixed(1)}H${elbow.toFixed(1)}L${l.sx.toFixed(1)} ${l.sy.toFixed(1)}`);
      l.path.style.opacity = l.alpha * 0.85;
      l.dot.setAttribute('cx', l.sx.toFixed(1)); l.dot.setAttribute('cy', l.sy.toFixed(1));
      l.dot.style.opacity = l.alpha;
    }
  }
}

// ---------- frame loop ----------
function resize() {
  const w = window.innerWidth, h = window.innerHeight;
  renderer.setSize(w, h, false);
  camera.aspect = w / h;
  camera.fov = w < h ? 46 : 32;         // portrait phones need a wider view to fit the model
  camera.updateProjectionMatrix();
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
  for (const p of parts) p.mesh.position.copy(p.e1).multiplyScalar(e1).addScaledVector(p.e2, e2);
  // pull the camera back as the model spreads out, keeping whatever zoom the viewer chose
  const fit = 1 + 0.55 * e1 + 0.12 * e2;
  if (Math.abs(fit - lastFit) > 1e-5) {
    const ratio = fit / lastFit;
    camera.position.sub(controls.target).multiplyScalar(ratio).add(controls.target);
    if (flight) { flight.from.sub(controls.target).multiplyScalar(ratio).add(controls.target); flight.to.sub(controls.target).multiplyScalar(ratio).add(controls.target); }
    lastFit = fit;
  }

  if (flight) {
    flight.t = Math.min(1, flight.t + dt * 1.6);
    const s = flight.t * flight.t * (3 - 2 * flight.t);
    camera.position.lerpVectors(flight.from, flight.to, s);
    if (flight.t >= 1) flight = null;
  } else if (state.spin > 0 && !pointerDown) {
    tmp.copy(camera.position).sub(controls.target).applyAxisAngle(camera.up, dt * state.spin * 0.12);
    camera.position.copy(controls.target).add(tmp);
  }
  controls.update();

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
  const mk = q.get('mk');
  setDesign(mk === '1' || mk === '2' ? 'mk' + mk : stored('bedside-version'));
  const fin = q.get('case');
  setFinish(fin ? fin[0].toUpperCase() + fin.slice(1).toLowerCase() : (stored('bedside-finish') || 'Beige'));
  setBackdrop(q.get('bg') || stored('bedside-backdrop'));
  buildLabels();
  try { localStorage.removeItem('bedside-labels'); } catch (e) { /* storage blocked */ }   // labels are never remembered: off unless the link asks for them
  setLabels(q.get('labels') === '1');
  if (q.get('wire') === '1') setWire(true);
  applyURL(q);
  applyVisibility(); frame();
}).catch((err) => {
  const el = document.getElementById('loading');
  if (el) el.textContent = 'Could not load the model';
  console.error(err);
});
