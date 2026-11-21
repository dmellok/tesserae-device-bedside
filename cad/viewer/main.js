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
const state = { explode: 0, parts: 0, spin: 0, hidden: new Set() };
const shown = { explode: 0, parts: 0 };

const CASE_PARTS = ['Front bezel', 'Rear shell', 'Plinth'];
// Case finishes. Beige is the two-tone case as modelled: beige frame, black chassis and plinth. Black gets a light backdrop so the shape still reads.
const SHELL = ['Front bezel', 'Mid chassis and base', 'Rear shell', 'Plinth'];
const FINISHES = { Beige: { colour: null, light: false }, Black: { colour: '#1b1c1f', light: true } };
function setFinish(name) {
  const f = FINISHES[name] || FINISHES.Beige;
  state.finish = FINISHES[name] ? name : 'Beige';
  for (const p of parts) {
    if (p.screen || !SHELL.includes(p.name)) continue;
    p.mesh.material.color.copy(f.colour ? new THREE.Color(f.colour) : p.mesh.userData.base);
    p.mesh.material.roughness = f.colour ? 0.62 : 0.72;
  }
  paintWire();
  document.documentElement.classList.toggle('light', f.light);
  document.querySelector('meta[name=theme-color]').content = f.light ? '#f4f5f7' : '#0b0d12';
  for (const b of $('finish').children) b.setAttribute('aria-pressed', String(b.textContent === state.finish));
  try { localStorage.setItem('bedside-finish', state.finish); } catch (e) { /* storage blocked */ }
  if (hovered) { const h = hovered; hovered = null; setHover(h); }
}

async function load() {
  const [meta, bin] = await Promise.all([
    fetch('./data/bedside.json').then((r) => r.json()),
    fetch('./data/bedside.bin').then((r) => r.arrayBuffer()),
  ]);
  for (const p of meta.parts) {
    const geo = new THREE.BufferGeometry();
    geo.setAttribute('position', new THREE.BufferAttribute(new Float32Array(bin, p.pos, p.verts * 3), 3));
    geo.setAttribute('normal', new THREE.BufferAttribute(new Float32Array(bin, p.nrm, p.verts * 3), 3));
    geo.setIndex(new THREE.BufferAttribute(new Uint32Array(bin, p.idx, p.tris * 3), 1));
    const colour = new THREE.Color().setRGB(p.color[0] / 255, p.color[1] / 255, p.color[2] / 255, THREE.SRGBColorSpace);
    const metal = /gold|pin|knob|shield|usb|steel/i.test(p.name) ? 0.35 : 0.04;
    const mat = new THREE.MeshStandardMaterial({ color: colour, roughness: 0.72, metalness: metal, transparent: p.opacity < 1, opacity: p.opacity, depthWrite: p.opacity >= 1 });
    const mesh = new THREE.Mesh(geo, mat);
    mesh.userData.name = p.name;
    mesh.userData.base = colour.clone();
    model.add(mesh);
    parts.push({ mesh, name: p.name, e1: new THREE.Vector3(...p.e1), e2: new THREE.Vector3(...p.e2) });
  }
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
  document.getElementById('loading').remove();
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
  for (const p of parts) if (p.lines) p.lines.material.color.copy(hovered === p.name ? HOT : (f.colour ? new THREE.Color(f.colour) : p.mesh.userData.base));
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
    const hide = state.hidden.has(p.name);
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
  for (const b of $('show').children) b.setAttribute('aria-pressed', 'true');
  applyVisibility();
  controls.target.copy(CENTRE);
  flight = { from: camera.position.clone(), to: new THREE.Vector3(...VIEWS.Front).add(CENTRE), t: 0 };
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
  const group = parts.filter((p) => p.name === name && !p.screen);
  const c = group.length ? '#' + group[0].mesh.material.color.getHexString() : '#ccc';
  tip.querySelector('.name').innerHTML = `<i style="background:${c}"></i>`;
  tip.querySelector('.name').append(name);
  tip.querySelector('.meta').textContent = DETAILS[name] || '';
  tip.classList.add('on');
}
const DETAILS = {
  'Front bezel': 'Printed case, part 1 of 4. Holds the glass from the front.',
  'Mid chassis and base': 'Printed case, part 2 of 4, in black. Everything mounts to this.',
  'Rear shell': 'Printed case, part 3 of 4. The back of the frame; slides off once the plinth is off.',
  'Plinth': 'Printed case, part 4 of 4, in black. Covers the battery, speaker and USB-C inlet.',
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
const OUTSIDE = new Set(['Front bezel', 'Rear shell', 'Plinth', 'Mid chassis and base', 'E Ink ED103TC2 panel', 'Keycaps', 'Knob', 'USB-C power inlet']);
const ANCHORS = { 'Front bezel': [30, 46.3, 196.3], 'Rear shell': [60, 68.4, 130], 'Plinth': [200, 84, 36], 'Mid chassis and base': [200, 8, 4], 'Keycaps': [152, 60, 197] };
const labels = [];
const SVGNS = 'http://www.w3.org/2000/svg';
let labelsOn = false, labelHover = null;
function buildLabels() {
  const seen = new Map();
  for (const p of parts) {
    if (p.screen) continue;
    p.mesh.geometry.computeBoundingBox();
    if (!seen.has(p.name)) seen.set(p.name, { box: new THREE.Box3(), part: p });
    seen.get(p.name).box.union(p.mesh.geometry.boundingBox);
  }
  for (const [name, { box, part }] of seen) {
    const el = document.createElement('div');
    el.className = 'lab'; el.textContent = name;
    el.addEventListener('pointerenter', () => { labelHover = name; setHover(name); });
    el.addEventListener('pointerleave', () => { labelHover = null; pointerMoved = true; });
    $('labels').appendChild(el);
    const path = document.createElementNS(SVGNS, 'path'), dot = document.createElementNS(SVGNS, 'circle');
    dot.setAttribute('r', '2.5');
    $('leaders').append(path, dot);
    const anchor = ANCHORS[name] ? new THREE.Vector3(...ANCHORS[name]) : box.getCenter(new THREE.Vector3());
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
  const inside = state.hidden.has('Rear shell') && state.hidden.has('Plinth') ? 1 : Math.min(1, Math.max(0, (e1 - 0.18) / 0.3));
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
  let saved = null;
  try { saved = localStorage.getItem('bedside-finish'); } catch (e) { /* storage blocked */ }
  const m = /(?:^#|&)case=(\w+)/i.exec(location.hash);
  const want = m ? m[1][0].toUpperCase() + m[1].slice(1).toLowerCase() : saved;
  setFinish(want || 'Beige');
  buildLabels();
  const lab = /(?:^#|&)labels=1/.test(location.hash);
  try { localStorage.removeItem('bedside-labels'); } catch (e) { /* storage blocked */ }   // labels are never remembered: off unless the link asks for them
  setLabels(lab);
  if (/(?:^#|&)wire=1/.test(location.hash)) setWire(true);
  applyVisibility(); frame();
}).catch((err) => {
  const el = document.getElementById('loading');
  if (el) el.textContent = 'Could not load the model';
  console.error(err);
});
