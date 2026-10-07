// auto-dark control panel. Plain ES2022, no dependencies.
// API contract: see PLAN.md ("HTTP API"). The parameter UI is generated from /api/schema.
'use strict';
(() => {

// ---------------------------------------------------------------- constants
const STATE_MS = 1500;          // /api/state poll
const STATS_MS = 200;           // /api/stats poll (~5 Hz)
const PREVIEW_MS = 140;         // min interval between preview frames (~7 fps)
const OFFLINE_RETRY_MS = 1000;
const FETCH_TIMEOUT_MS = 3000;
const EDIT_GRACE_MS = 1500;     // ignore polled values for a param this long after a local edit
const HISTORY_MS = 20000;       // meter sparkline window
const METER_Y0 = 0.005, METER_Y1 = 1;   // meter log scale (linear luminance)
const GR_EV_MAX = 6;            // gain-reduction bar full scale, in EV (stops)
const GR_PEAK_MS = 3000;
const MODES = ['Output', 'Input', 'Selection', 'Gain'];

// ---------------------------------------------------------------- helpers
const $ = (q, root = document) => root.querySelector(q);
const sleep = ms => new Promise(r => setTimeout(r, ms));
const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
const num = (v, d) => (typeof v === 'number' && Number.isFinite(v) ? v : d);
const round = (v, d) => Number(v.toFixed(d));
const sig = (v, d) => Number(v.toPrecision(d));

function h(tag, props = {}, ...kids) {
  const n = document.createElement(tag);
  for (const [k, v] of Object.entries(props)) {
    if (v == null || v === false) continue;
    if (k === 'class') n.className = v;
    else if (k === 'text') n.textContent = v;
    else if (k.startsWith('on') && typeof v === 'function') n.addEventListener(k.slice(2), v);
    else n.setAttribute(k, v === true ? '' : String(v));
  }
  for (const c of kids.flat()) if (c != null && c !== false) n.append(c);
  return n;
}
const SVGNS = 'http://www.w3.org/2000/svg';
function s(tag, attrs = {}, text) {
  const n = document.createElementNS(SVGNS, tag);
  for (const [k, v] of Object.entries(attrs)) if (v != null) n.setAttribute(k, String(v));
  if (text != null) n.textContent = text;
  return n;
}

const store = {
  get(k, d) { try { const v = localStorage.getItem('autodark.' + k); return v == null ? d : JSON.parse(v); } catch { return d; } },
  set(k, v) { try { localStorage.setItem('autodark.' + k, JSON.stringify(v)); } catch { /* ignore */ } },
};

function sameValue(a, b) {
  if (typeof a === 'number' && typeof b === 'number') return Math.abs(a - b) < 1e-9;
  return JSON.stringify(a) === JSON.stringify(b);
}

function stepDecimals(step) {
  if (!(step > 0)) return 3;
  const str = String(step);
  if (str.includes('e-')) return parseInt(str.split('e-')[1], 10);
  const i = str.indexOf('.');
  return i < 0 ? 0 : str.length - i - 1;
}

// Percent of a 0..1 fraction with sensible precision for tiny values.
function fmtPct(frac) {
  const p = frac * 100;
  if (!Number.isFinite(p)) return '—';
  if (p >= 10) return p.toFixed(0) + '%';
  if (p >= 1) return p.toFixed(1).replace(/\.0$/, '') + '%';
  if (p <= 0) return '0%';
  return String(sig(p, 2)) + '%';
}
// Axis tick label for log10(area fraction): -7 → 0.00001%, -2 → 1%, 0 → 100%.
function fmtAreaTick(x) {
  const e = x + 2;          // log10 of percent
  if (Number.isInteger(e)) return e >= 0 ? `${10 ** e}%` : `${(10 ** e).toFixed(-e)}%`;
  return fmtPct(10 ** x);
}

function fmtValue(p, v) {
  if (typeof v !== 'number' || !Number.isFinite(v)) return '—';
  switch (p.format) {
    case 'percent': return (v * 100).toFixed(1) + '%';
    case 'ms': return v.toFixed(stepDecimals(num(p.step, 1))) + ' ms';
    case 'kelvin': return `${Math.round(v)} K`;
    case 'min': return `${Math.round(v)} min`;
    case 'clock': { const m = Math.round(v * 60) % 1440; return `${String(Math.floor(m / 60)).padStart(2, '0')}:${String(m % 60).padStart(2, '0')}`; }
    case 'level': { const n = Math.round(v); return `L${n} · ${2 ** n}px cells`; }
    default: return v.toFixed(p.type === 'int' ? 0 : stepDecimals(num(p.step, 0.01)));
  }
}
function editValue(p, v) {
  if (p.format === 'percent') return String(round(v * 100, Math.max(1, stepDecimals(num(p.step, 0.001)) - 2)));
  return String(round(v, p.type === 'int' ? 0 : stepDecimals(num(p.step, 0.01))));
}
function parseEdit(p, str) {
  const m = String(str).replace(',', '.').match(/-?\d*\.?\d+(?:e-?\d+)?/i);
  if (!m) return NaN;
  const n = parseFloat(m[0]);
  return p.format === 'percent' ? n / 100 : n;
}
function quantize(p, v) {
  const isInt = p.type === 'int';
  const min = num(p.min, 0), max = num(p.max, isInt ? 10 : 1);
  const step = num(p.step, isInt ? 1 : 0);
  v = clamp(v, min, max);
  if (step > 0) v = clamp(min + Math.round((v - min) / step) * step, min, max);
  if (isInt) return Math.round(v);
  return round(v, stepDecimals(step) + 2);
}

function monLabel(m) {
  const name = String(m.name ?? `Monitor ${m.index}`).replace(/^\\\\\.\\/, '');
  return `${name} · ${m.width}×${m.height}`;
}

// ---------------------------------------------------------------- app state
const S = {
  schema: null,
  schemaVersion: undefined,
  byId: new Map(),
  controls: new Map(),      // id -> { el, set(v), busy() }
  curves: [],               // CurveEditor instances
  params: {},               // last known value per param id
  state: null,
  dirty: false,
  bypass: false,
  bypassHoldUntil: 0,
  profiles: [],
  activeProfile: null,
  monitors: [],
  monSig: '',
  mon: store.get('mon', 0),
  mode: store.get('mode', 0),
  hist: new Map(),          // monitor index -> [{t,i,o,g}]
  grPeak: new Map(),        // monitor index -> {ev, t}
  meterEls: new Map(),
};
const lastEdit = new Map();

// ---------------------------------------------------------------- connection + fetch
class OfflineError extends Error {}

const conn = {
  online: null, fails: 0, attempts: 0, lastErr: '', since: 0,
  ok() {
    this.fails = 0;
    if (this.online !== true) {
      const wasOffline = this.online === false;
      this.online = true;
      renderConn();
      if (wasOffline) toast('Engine connected', 'ok');
      refreshProfiles().catch(() => {});
      refreshState();
    }
  },
  fail(err) {
    this.fails++;
    this.attempts++;
    this.lastErr = err?.name === 'TimeoutError' ? 'request timed out' : (err?.message || String(err));
    if (this.fails >= 2 && this.online !== false) {
      this.online = false;
      this.since = Date.now();
      this.attempts = 1;
    }
    renderConn();
  },
};

async function api(path, { method = 'GET', body, raw = false, timeout = FETCH_TIMEOUT_MS } = {}) {
  const init = { method, cache: 'no-store', signal: AbortSignal.timeout(timeout) };
  if (body !== undefined) {
    // The engine rejects POSTs without this exact content type (CSRF guard).
    init.headers = { 'Content-Type': 'application/json' };
    init.body = JSON.stringify(body);
  }
  let res;
  try {
    res = await fetch(path, init);
  } catch (e) {
    conn.fail(e);
    throw new OfflineError(e?.message || 'fetch failed');
  }
  conn.ok();
  if (raw) return res;
  if (res.status === 204) return null;
  const text = await res.text();
  let data = null;
  if (text) {
    try { data = JSON.parse(text); }
    catch { if (res.ok) throw new Error(`invalid JSON from ${path}`); }
  }
  if (!res.ok || (data && data.ok === false)) {
    const msg = (data && (data.error || data.message)) || text.trim().slice(0, 200) || res.statusText;
    throw new Error(`${res.status} ${msg}`);
  }
  return data;
}

function renderConn() {
  const c = $('#conn');
  c.classList.toggle('online', conn.online === true);
  c.classList.toggle('offline', conn.online === false);
  $('#conn-text').textContent = conn.online === true ? 'engine online' : conn.online === false ? 'engine offline' : 'connecting…';
  $('#offline').hidden = conn.online !== false;
  if (conn.online === false) {
    const secs = Math.round((Date.now() - conn.since) / 1000);
    $('#offline-detail').textContent = `retrying… attempt ${conn.attempts}, offline for ${secs}s — ${conn.lastErr}`;
  }
}

// ---------------------------------------------------------------- toasts
function toast(msg, kind = 'info', ms = 3200) {
  const t = h('div', { class: `toast ${kind}`, text: msg });
  $('#toasts').append(t);
  setTimeout(() => { t.classList.add('out'); setTimeout(() => t.remove(), 320); }, ms);
}

// ---------------------------------------------------------------- in-page dialogs
// The browser's native prompt and confirm dialogs return null in embedded browsers (e.g. Electron
// panes), so use our own. ask({ title, message, input, okLabel, danger }) -> Promise<string|true|null>
function ask({ title, message = '', input = null, okLabel = 'OK', danger = false }) {
  return new Promise(resolve => {
    const field = input == null ? null : h('input', { class: 'dlg-input', type: 'text', value: input, spellcheck: 'false' });
    const err = h('div', { class: 'dlg-err' });
    const ok = h('button', { class: danger ? 'btn btn-danger-solid' : 'btn btn-primary', type: 'submit', text: okLabel });
    const cancel = h('button', { class: 'btn', type: 'button', text: 'Cancel' });
    const form = h('form', { class: 'dlg' },
      h('div', { class: 'dlg-title', text: title }),
      message ? h('div', { class: 'dlg-msg', text: message }) : null,
      field, err,
      h('div', { class: 'dlg-actions' }, cancel, ok));
    const back = h('div', { class: 'dlg-back' }, form);
    const close = v => { back.remove(); document.removeEventListener('keydown', onKey, true); resolve(v); };
    const onKey = e => { if (e.key === 'Escape') { e.preventDefault(); close(null); } };
    document.addEventListener('keydown', onKey, true);
    cancel.addEventListener('click', () => close(null));
    back.addEventListener('mousedown', e => { if (e.target === back) close(null); });
    form.addEventListener('submit', e => {
      e.preventDefault();
      if (!field) return close(true);
      const v = field.value.trim();
      if (!validName(v)) { err.textContent = 'Letters, digits, space, _ and - only (max 64).'; field.focus(); return; }
      close(v);
    });
    document.body.append(back);
    (field || ok).focus();
    if (field) field.select();
  });
}

// ---------------------------------------------------------------- param sending (coalesced per param)
const sender = new Map();   // id -> { busy, has, val }

function sendParam(id, value) {
  lastEdit.set(id, performance.now());
  setDirty(true);
  let q = sender.get(id);
  if (!q) sender.set(id, q = { busy: false, has: false, val: undefined });
  q.val = value;
  q.has = true;
  if (!q.busy) pump(id, q);
}
async function pump(id, q) {
  q.busy = true;
  while (q.has) {
    const v = q.val;
    q.has = false;
    try {
      await api('/api/params', { method: 'POST', body: { [id]: v } });
    } catch (e) {
      if (!(e instanceof OfflineError)) toast(`Could not set ${id}: ${e.message}`, 'error');
    }
    lastEdit.set(id, performance.now());
  }
  q.busy = false;
}
const isPending = id => { const q = sender.get(id); return !!q && (q.busy || q.has); };
const recentlyEdited = id => performance.now() - (lastEdit.get(id) ?? -Infinity) < EDIT_GRACE_MS;

function paramChanged(id) {
  const c = S.controls.get(id);
  const p = S.byId.get(id);
  if (c?.labelEl && p) c.labelEl.classList.toggle('modified', !sameValue(S.params[id], p.default));
  if (id === 'minLevel' || id === 'maxLevel') S.curves.forEach(cv => cv.render());
}

// ---------------------------------------------------------------- controls (schema-driven)
function makeCard(title, help) {
  const body = h('div', { class: 'card-b' });
  const tools = h('div', { class: 'card-tools' });
  const el = h('section', { class: 'card' },
    h('header', { class: 'card-h' }, h('h2', { text: title }), tools),
    help ? h('p', { class: 'card-help', text: help }) : null,
    body);
  return { el, body, tools };
}

function flash(el) {
  el.classList.remove('flash');
  void el.offsetWidth;
  el.classList.add('flash');
}

function makeSlider(p) {
  const isInt = p.type === 'int';
  const min = num(p.min, 0), max = num(p.max, isInt ? 10 : 1);
  const step = num(p.step, isInt ? 1 : (max - min) / 1000);
  const def = num(p.default, min);
  const frac = v => (max > min ? clamp((v - min) / (max - min), 0, 1) : 0);
  let value = def, dragging = false, editing = false;

  const range = h('input', { type: 'range', id: `p-${p.id}`, min, max, step, 'aria-label': p.label || p.id });
  const out = h('input', { type: 'text', class: 'p-val', spellcheck: 'false', autocomplete: 'off',
    inputmode: 'decimal', 'aria-label': `${p.label || p.id} value`, title: 'Click to type a value · ↑/↓ to nudge' });
  const label = h('label', { class: 'p-label', for: range.id, text: p.label || p.id,
    title: `${p.help ? p.help + '\n\n' : ''}Double-click to reset to ${fmtValue(p, def)}` });
  const defTick = h('span', { class: 'p-def', title: `default ${fmtValue(p, def)}` });
  defTick.style.setProperty('--p', frac(def));
  const nSteps = Math.round((max - min) / step);
  const ticks = isInt && nSteps > 0 && nSteps <= 16
    ? h('div', { class: 'p-ticks' }, Array.from({ length: nSteps + 1 }, () => h('i'))) : null;
  const row = h('div', { class: 'param', 'data-id': p.id, title: p.help || '' },
    h('div', { class: 'p-top' }, label, out),
    h('div', { class: 'p-track' }, range, ticks, defTick),
    p.help ? h('div', { class: 'p-help', text: p.help }) : null);

  const show = v => {
    range.style.setProperty('--p', frac(v));
    if (!editing) out.value = fmtValue(p, v);
  };
  const commit = raw => {
    const v = quantize(p, raw);
    if (!Number.isFinite(v)) return;
    range.value = v;
    show(v);
    if (sameValue(v, value)) return;
    value = v;
    S.params[p.id] = v;
    sendParam(p.id, v);
    paramChanged(p.id);
  };

  range.addEventListener('pointerdown', () => { dragging = true; });
  for (const ev of ['pointerup', 'pointercancel', 'change', 'blur']) range.addEventListener(ev, () => { dragging = false; });
  range.addEventListener('input', () => commit(parseFloat(range.value)));
  label.addEventListener('mousedown', e => { if (e.detail > 1) e.preventDefault(); });
  label.addEventListener('dblclick', e => { e.preventDefault(); commit(def); flash(row); });

  const finishEdit = () => {
    if (!editing) return;
    const n = parseEdit(p, out.value);
    editing = false;
    if (Number.isFinite(n)) commit(n);
    show(value);
  };
  out.addEventListener('focus', () => { editing = true; out.value = editValue(p, value); out.select(); });
  out.addEventListener('input', () => { editing = true; });
  out.addEventListener('blur', finishEdit);
  out.addEventListener('change', finishEdit);
  out.addEventListener('keydown', e => {
    if (e.key === 'Enter') { e.preventDefault(); finishEdit(); out.blur(); }
    else if (e.key === 'Escape') { out.value = editValue(p, value); finishEdit(); out.blur(); }
    else if (e.key === 'ArrowUp' || e.key === 'ArrowDown') {
      e.preventDefault();
      const cur = parseEdit(p, out.value);
      const base = Number.isFinite(cur) ? quantize(p, cur) : value;
      commit(base + (e.key === 'ArrowUp' ? 1 : -1) * step * (e.shiftKey ? 10 : 1));
      out.value = editValue(p, value);
      out.select();
    }
  });

  return {
    el: row,
    labelEl: label,
    busy: () => dragging || editing,
    set(v) {
      if (typeof v !== 'number' || !Number.isFinite(v)) return;
      value = v;
      range.value = v;
      show(v);
    },
  };
}

function makeEnum(p) {
  const opts = Array.isArray(p.options) ? p.options : [];
  const def = Math.round(num(p.default, 0));
  let value = def;
  const show = () => btns.forEach((b, i) => b.setAttribute('aria-pressed', String(i === value)));
  const choose = i => {
    if (i === value) return;
    value = i;
    S.params[p.id] = i;
    show();
    sendParam(p.id, i);
    paramChanged(p.id);
  };
  const btns = opts.map((o, i) => h('button', { type: 'button', text: String(o), 'aria-pressed': 'false', onclick: () => choose(i) }));
  const label = h('span', { class: 'p-label', text: p.label || p.id,
    title: `${p.help ? p.help + '\n\n' : ''}Double-click to reset to ${opts[def] ?? def}` });
  label.addEventListener('mousedown', e => { if (e.detail > 1) e.preventDefault(); });
  const row = h('div', { class: 'param', 'data-id': p.id, title: p.help || '' },
    h('div', { class: 'p-top' }, label),
    h('div', { class: 'seg', role: 'group', 'aria-label': p.label || p.id }, btns),
    p.help ? h('div', { class: 'p-help', text: p.help }) : null);
  label.addEventListener('dblclick', () => { choose(def); flash(row); });
  show();
  return {
    el: row,
    labelEl: label,
    busy: () => false,
    set(v) { if (typeof v === 'number') { value = Math.round(v); show(); } },
  };
}

function makeCurve(p) {
  const ed = new CurveEditor(p, pts => {
    S.params[p.id] = pts;
    sendParam(p.id, pts);
  });
  S.curves.push(ed);
  return { el: ed.el, labelEl: null, busy: () => ed.busy(), set: v => ed.set(v) };
}

function makeControl(p) {
  switch (p.type) {
    case 'float': case 'int': return makeSlider(p);
    case 'enum': return makeEnum(p);
    case 'curve': return makeCurve(p);
    default:
      console.info(`auto-dark: skipping param "${p.id}" of unknown type "${p.type}"`);
      return null;
  }
}

function buildControls(schema) {
  const sx = window.scrollX, sy = window.scrollY;
  const params = Array.isArray(schema?.params) ? schema.params.filter(p => p && typeof p.id === 'string') : [];
  const groups = Array.isArray(schema?.groups) ? schema.groups.filter(g => g && g.id != null) : [];
  const known = new Set(groups.map(g => g.id));
  const orphans = params.filter(p => !known.has(p.group));
  if (orphans.length) groups.push({ id: '\0other', label: 'Other', help: '' });

  S.groupStatus = new Map();
  S.curves.forEach(c => c.destroy());
  S.curves = [];
  S.controls.clear();
  S.schema = schema;
  S.byId = new Map(params.map(p => [p.id, p]));

  const left = $('#controls'), right = $('#curve-slot');
  const leftCards = [], rightCards = [];
  for (const g of groups) {
    const gp = g.id === '\0other' ? orphans : params.filter(p => p.group === g.id);
    if (!gp.length) continue;
    const card = makeCard(g.label || g.id, g.help);
    // Live status line the engine can fill per group (stats.groupStatus[groupId]).
    const status = h('p', { class: 'card-status' });
    card.body.before(status);
    S.groupStatus.set(g.id, status);
    let hasCurve = false;
    for (const p of gp) {
      const c = makeControl(p);
      if (!c) continue;
      card.body.append(c.el);
      S.controls.set(p.id, c);
      if (p.type === 'curve') hasCurve = true;
    }
    if (!card.body.childElementCount) continue;
    // Groups holding a curve editor go to the wide right column, next to the live view.
    (hasCurve ? rightCards : leftCards).push(card.el);
  }
  left.replaceChildren(...leftCards);
  right.replaceChildren(...rightCards);

  for (const [id, c] of S.controls) {
    const v = id in S.params ? S.params[id] : S.byId.get(id).default;
    S.params[id] = v;
    c.set(v);
    paramChanged(id);
  }
  window.scrollTo(sx, sy);
  requestAnimationFrame(() => window.scrollTo(sx, sy));
}

// ---------------------------------------------------------------- curve editor (SVG)
class CurveEditor {
  constructor(p, onChange) {
    this.p = p;
    this.onChange = onChange;
    this.x0 = num(p.xMin, -7); this.x1 = num(p.xMax, 0);
    if (!(this.x1 > this.x0)) { this.x0 = -7; this.x1 = 0; }
    this.y0 = Math.max(1e-6, num(p.yMin, 0.005)); this.y1 = num(p.yMax, 1);
    if (!(this.y1 > this.y0)) { this.y0 = 0.005; this.y1 = 1; }
    this.ly0 = Math.log10(this.y0); this.ly1 = Math.log10(this.y1);
    this.def = this.sanitize(p.default) ?? [[this.x0, this.y1], [this.x1, this.y0]];
    this.pts = this.def.map(q => q.slice());
    this.drag = null;
    this.hoverX = null;
    this.W = 640; this.H = 260;
    this.M = { l: 46, r: 14, t: 24, b: 44 };
    this.uid = 'cv' + Math.random().toString(36).slice(2, 8);

    this.svg = s('svg', { class: 'curve-svg', role: 'img', 'aria-label': `${p.label || p.id} curve editor` });
    this.layerMain = s('g');
    this.layerHover = s('g', { 'pointer-events': 'none' });
    this.svg.append(this.layerMain, this.layerHover);
    this.readout = h('span', { class: 'curve-readout' });
    this.box = h('div', { class: 'curve-box' }, this.svg);
    const reset = h('button', { type: 'button', class: 'btn btn-sm', text: 'Reset curve', title: 'Restore the default curve',
      onclick: () => { this.pts = this.def.map(q => q.slice()); this.render(); this.emit(); } });
    this.el = h('div', { class: 'curve', 'data-id': p.id },
      h('div', { class: 'curve-bar' },
        h('span', { class: 'p-label', text: p.label || p.id, title: p.help || '' }), this.readout, reset),
      this.box,
      h('div', { class: 'curve-hint', text: 'Drag points · double-click empty space to add · double-click or right-click a point to remove · Shift-drag keeps x fixed' }),
      p.help ? h('div', { class: 'p-help', text: p.help, title: p.help }) : null);

    this.ro = new ResizeObserver(() => this.resize());
    this.ro.observe(this.box);
    this.bind();
    this.render();
  }

  destroy() { this.ro.disconnect(); }
  busy() { return !!this.drag; }

  sanitize(v) {
    if (!Array.isArray(v)) return null;
    const pts = v
      .filter(q => Array.isArray(q) && q.length >= 2 && Number.isFinite(+q[0]) && Number.isFinite(+q[1]))
      .map(([x, y]) => [clamp(+x, this.x0, this.x1), clamp(+y, this.y0, this.y1)])
      .sort((a, b) => a[0] - b[0]);
    return pts.length >= 2 ? pts : null;
  }
  set(v) {
    if (this.drag) return;
    const pts = this.sanitize(v);
    if (pts) { this.pts = pts; this.render(); }
  }
  emit() { this.onChange(this.pts.map(q => [q[0], q[1]])); }

  get pw() { return Math.max(20, this.W - this.M.l - this.M.r); }
  get ph() { return this.H - this.M.t - this.M.b; }
  sx(x) { return this.M.l + (x - this.x0) / (this.x1 - this.x0) * this.pw; }
  sy(y) { return this.M.t + (1 - (Math.log10(y) - this.ly0) / (this.ly1 - this.ly0)) * this.ph; }
  ix(px) { return this.x0 + (px - this.M.l) / this.pw * (this.x1 - this.x0); }
  iy(py) { return 10 ** (this.ly0 + (1 - (py - this.M.t) / this.ph) * (this.ly1 - this.ly0)); }

  // Engine interpolates linearly in (log10 area, log10 budget), constant outside the points.
  evalAt(x) {
    const p = this.pts;
    if (x <= p[0][0]) return p[0][1];
    for (let i = 1; i < p.length; i++) {
      if (x <= p[i][0]) {
        const [xa, ya] = p[i - 1], [xb, yb] = p[i];
        const t = xb > xa ? (x - xa) / (xb - xa) : 1;
        return 10 ** (Math.log10(ya) + t * (Math.log10(yb) - Math.log10(ya)));
      }
    }
    return p[p.length - 1][1];
  }

  resize() {
    const w = Math.round(this.box.clientWidth);
    if (w > 0 && w !== this.W) { this.W = w; this.render(); }
  }

  levelContext() {
    const mon = S.monitors.find(m => m.index === S.mon) ?? S.monitors[0] ?? null;
    const levels = (mon?.levels ?? [])
      .filter(l => Number.isFinite(l?.logArea))
      .map((l, i) => ({ level: Number.isFinite(l.level) ? l.level : i, x: l.logArea }))
      .sort((a, b) => a.level - b.level);
    const minL = typeof S.params.minLevel === 'number' ? S.params.minLevel : null;
    const maxL = typeof S.params.maxLevel === 'number' ? S.params.maxLevel : null;
    return { mon, levels, minL, maxL };
  }

  render() {
    const { W, H, M, uid } = this;
    const pw = this.pw, ph = this.ph, top = M.t, bot = M.t + ph, left = M.l, right = M.l + pw;
    this.svg.setAttribute('viewBox', `0 0 ${W} ${H}`);
    const n = [];

    // defs: hatch for unchecked scales, gradient under curve
    const defs = s('defs');
    const pat = s('pattern', { id: `${uid}-hatch`, width: 6, height: 6, patternUnits: 'userSpaceOnUse', patternTransform: 'rotate(45)' });
    pat.append(s('rect', { width: 6, height: 6, class: 'c-hatch-bg' }), s('line', { x1: 0, y1: 0, x2: 0, y2: 6, class: 'c-hatch-line' }));
    const grad = s('linearGradient', { id: `${uid}-fill`, x1: 0, y1: 0, x2: 0, y2: 1 });
    grad.append(s('stop', { offset: 0, class: 'c-fill-top' }), s('stop', { offset: 1, class: 'c-fill-bot' }));
    const clip = s('clipPath', { id: `${uid}-clip` });
    clip.append(s('rect', { x: left, y: top, width: pw, height: ph }));
    defs.append(pat, grad, clip);
    n.push(defs);
    n.push(s('rect', { x: left, y: top, width: pw, height: ph, class: 'c-plot' }));

    // pyramid levels of the selected monitor + unchecked shading
    const { mon, levels, minL, maxL } = this.levelContext();
    const checked = lv => (minL == null || lv >= minL) && (maxL == null || lv <= maxL);
    if (levels.length) {
      const first = levels[0].level, last = levels[levels.length - 1].level;
      const xOfLevel = lv => levels.find(l => l.level === lv)?.x;
      const shade = (xa, xb) => {
        xa = clamp(xa, this.x0, this.x1); xb = clamp(xb, this.x0, this.x1);
        const a = this.sx(xa), b = this.sx(xb);
        if (b - a < 1) return;
        n.push(s('rect', { x: a, y: top, width: b - a, height: ph, fill: `url(#${uid}-hatch)` }));
        if (a > left + 0.5) n.push(s('line', { x1: a, y1: top, x2: a, y2: bot, class: 'c-unchecked-edge' }));
        if (b < right - 0.5) n.push(s('line', { x1: b, y1: top, x2: b, y2: bot, class: 'c-unchecked-edge' }));
        if (b - a > 70) n.push(s('text', { x: (a + b) / 2, y: bot - 8, 'text-anchor': 'middle', class: 'c-unchecked-label' }, 'not checked'));
      };
      if ((minL != null && maxL != null && minL > maxL) || (minL != null && minL > last) || (maxL != null && maxL < first)) {
        shade(this.x0, this.x1);
      } else {
        if (minL != null && minL > first) {
          const a = xOfLevel(minL - 1), b = xOfLevel(minL);
          if (a != null && b != null) shade(this.x0, (a + b) / 2);
        }
        if (maxL != null && maxL < last) {
          const a = xOfLevel(maxL), b = xOfLevel(maxL + 1);
          if (a != null && b != null) shade((a + b) / 2, this.x1);
        }
      }
    }

    // grid: x decades, y log ticks
    for (let d = Math.floor(this.ly0); d <= Math.ceil(this.ly1); d++) {
      for (let m = 2; m <= 9; m++) {
        const y = m * 10 ** d;
        if (y <= this.y0 || y >= this.y1 || m === 2 || m === 5) continue;
        const py = this.sy(y);
        n.push(s('line', { x1: left, x2: right, y1: py, y2: py, class: 'c-grid-minor' }));
      }
    }
    for (const y of [0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1]) {
      if (y < this.y0 - 1e-12 || y > this.y1 + 1e-12) continue;
      const py = Math.round(this.sy(y)) + 0.5;
      n.push(s('line', { x1: left, x2: right, y1: py, y2: py, class: 'c-grid' }));
      n.push(s('text', { x: left - 6, y: py + 3.5, 'text-anchor': 'end', class: 'c-tick' }, fmtPct(y)));
    }
    const xStep = pw < 360 ? 2 : 1;
    for (let x = Math.ceil(this.x0); x <= Math.floor(this.x1); x++) {
      const px = Math.round(this.sx(x)) + 0.5;
      n.push(s('line', { x1: px, x2: px, y1: top, y2: bot, class: 'c-grid' }));
      if ((x - Math.floor(this.x1)) % xStep === 0) {
        const anchor = x === Math.ceil(this.x0) ? 'start' : x === Math.floor(this.x1) ? 'end' : 'middle';
        n.push(s('text', { x: px, y: bot + 14, 'text-anchor': anchor, class: 'c-tick' }, fmtAreaTick(x)));
      }
    }

    // level markers (dedupe levels that land on the same pixel, e.g. clamped top levels)
    const groups = [];
    for (const l of levels) {
      if (l.x < this.x0 - 1e-9 || l.x > this.x1 + 1e-9) continue;
      const px = this.sx(l.x);
      const g = groups[groups.length - 1];
      if (g && Math.abs(g.px - px) < 3) { g.to = l.level; g.on ||= checked(l.level); }
      else groups.push({ px, x: l.x, from: l.level, to: l.level, on: checked(l.level) });
    }
    let lastLabelPx = -Infinity;
    for (const g of groups) {
      const px = Math.round(g.px) + 0.5;
      n.push(s('line', { x1: px, x2: px, y1: top, y2: bot, class: 'c-level' + (g.on ? '' : ' off') }));
      const txt = g.from === g.to ? `L${g.from}` : `L${g.from}–${g.to}`;
      const tw = txt.length * 6;
      if (g.px - lastLabelPx > tw * 0.5 + 12) {
        const anchor = g.px > right - tw / 2 ? 'end' : 'middle';
        n.push(s('text', { x: px, y: top - 7, 'text-anchor': anchor, class: 'c-level-label' + (g.on ? '' : ' off') }, txt));
        lastLabelPx = g.px + (anchor === 'end' ? 0 : tw / 2);
      }
    }

    // curve: fill, dashed extensions to the axis ends, line
    const P = this.pts.map(([x, y]) => [this.sx(x), this.sy(y)]);
    const first = P[0], last = P[P.length - 1];
    const fillPts = [[left, bot], [left, first[1]], ...P, [right, last[1]], [right, bot]];
    const g = s('g', { 'clip-path': `url(#${uid}-clip)` });
    g.append(s('polygon', { points: fillPts.map(q => q.join(',')).join(' '), fill: `url(#${uid}-fill)` }));
    if (first[0] > left + 0.5) g.append(s('polyline', { points: `${left},${first[1]} ${first.join(',')}`, class: 'c-ext' }));
    if (last[0] < right - 0.5) g.append(s('polyline', { points: `${last.join(',')} ${right},${last[1]}`, class: 'c-ext' }));
    g.append(s('polyline', { points: P.map(q => q.join(',')).join(' '), class: 'c-line' }));
    n.push(g);

    // where the engine samples the budget: one dot per checked level position
    for (const gr of groups) {
      if (!gr.on) continue;
      n.push(s('circle', { cx: gr.px, cy: this.sy(this.evalAt(gr.x)), r: 2.4, class: 'c-sample' }));
    }

    // control points
    P.forEach(([px, py], i) => {
      n.push(s('circle', { cx: px, cy: py, r: this.drag?.i === i ? 6 : 5, class: 'c-pt' + (this.drag?.i === i ? ' active' : '') }));
    });

    // axis captions
    n.push(s('text', { x: left, y: H - 6, class: 'c-end' }, '◂ highlight ceiling'));
    n.push(s('text', { x: right, y: H - 6, 'text-anchor': 'end', class: 'c-end' }, 'whole screen ▸'));
    if (pw > 460) n.push(s('text', { x: (left + right) / 2, y: H - 6, 'text-anchor': 'middle', class: 'c-axis' }, 'bright-area size · share of monitor (log)'));

    this.layerMain.replaceChildren(...n);
    this._mon = mon;
    this.renderHover();
  }

  areaPx(x) {
    const mon = this._mon;
    if (!mon?.width || !mon?.height) return '';
    const side = Math.sqrt(10 ** x * mon.width * mon.height);
    const sd = side < 10 ? side.toFixed(1) : Math.round(side);
    return ` (~${sd}×${sd}px)`;
  }

  renderHover() {
    const nodes = [];
    let text;
    const x = this.drag ? this.pts[this.drag.i][0] : this.hoverX;
    if (x != null) {
      const y = this.drag ? this.pts[this.drag.i][1] : this.evalAt(x);
      const px = Math.round(this.sx(x)) + 0.5;
      if (!this.drag) {
        nodes.push(s('line', { x1: px, x2: px, y1: this.M.t, y2: this.M.t + this.ph, class: 'c-cross' }));
        nodes.push(s('circle', { cx: px, cy: this.sy(y), r: 3, class: 'c-cross-dot' }));
      }
      text = `area <em>${fmtPct(10 ** x)}</em>${this.areaPx(x)} → max <em>${fmtPct(y)}</em>`;
    } else {
      text = `ceiling <em>${fmtPct(this.pts[0][1])}</em> · whole screen <em>${fmtPct(this.evalAt(this.x1))}</em>`;
    }
    this.layerHover.replaceChildren(...nodes);
    this.readout.innerHTML = text;   // only our own formatted numbers
  }

  bind() {
    const svg = this.svg;
    const pos = e => { const r = svg.getBoundingClientRect(); return [e.clientX - r.left, e.clientY - r.top]; };
    const hit = (px, py) => {
      let best = -1, bd = 11;
      this.pts.forEach(([x, y], i) => {
        const d = Math.hypot(this.sx(x) - px, this.sy(y) - py);
        if (d <= bd) { bd = d; best = i; }
      });
      return best;
    };
    const inPlot = (px, py) => px >= this.M.l - 4 && px <= this.M.l + this.pw + 4 && py >= this.M.t - 4 && py <= this.M.t + this.ph + 4;
    const remove = i => {
      if (this.pts.length <= 2) { toast('The curve needs at least 2 points'); return; }
      this.pts.splice(i, 1);
      this.render();
      this.emit();
    };

    svg.addEventListener('pointerdown', e => {
      if (e.button !== 0) return;
      const [px, py] = pos(e);
      const i = hit(px, py);
      if (i < 0) return;
      e.preventDefault();
      svg.setPointerCapture(e.pointerId);
      this.drag = { i, id: e.pointerId, x: this.pts[i][0] };
      svg.style.cursor = 'grabbing';
      this.render();
    });
    svg.addEventListener('pointermove', e => {
      const [px, py] = pos(e);
      if (this.drag) {
        if (e.pointerId !== this.drag.id) return;
        const i = this.drag.i, p = this.pts;
        const lo = i > 0 ? p[i - 1][0] + 0.01 : this.x0;
        const hi = i < p.length - 1 ? p[i + 1][0] - 0.01 : this.x1;
        let x = p[i][0];
        if (lo <= hi) x = round(clamp(e.shiftKey ? this.drag.x : this.ix(px), lo, hi), 3);
        const y = clamp(sig(this.iy(clamp(py, this.M.t, this.M.t + this.ph)), 4), this.y0, this.y1);
        if (x !== p[i][0] || y !== p[i][1]) {
          p[i] = [x, y];
          this.render();
          this.emit();
        }
        return;
      }
      const nx = inPlot(px, py) ? clamp(this.ix(px), this.x0, this.x1) : null;
      svg.style.cursor = hit(px, py) >= 0 ? 'grab' : nx != null ? 'crosshair' : '';
      if (nx !== this.hoverX) { this.hoverX = nx; this.renderHover(); }
    });
    const end = e => {
      if (!this.drag || e.pointerId !== this.drag.id) return;
      this.drag = null;
      svg.style.cursor = '';
      if (svg.hasPointerCapture(e.pointerId)) svg.releasePointerCapture(e.pointerId);
      this.render();
    };
    svg.addEventListener('pointerup', end);
    svg.addEventListener('pointercancel', end);
    svg.addEventListener('lostpointercapture', end);
    svg.addEventListener('pointerleave', () => {
      if (!this.drag && this.hoverX != null) { this.hoverX = null; this.renderHover(); }
    });
    svg.addEventListener('dblclick', e => {
      const [px, py] = pos(e);
      const i = hit(px, py);
      if (i >= 0) { remove(i); return; }
      if (!inPlot(px, py)) return;
      const x = round(clamp(this.ix(px), this.x0, this.x1), 3);
      const y = clamp(sig(this.iy(clamp(py, this.M.t, this.M.t + this.ph)), 4), this.y0, this.y1);
      if (this.pts.some(q => Math.abs(q[0] - x) < 0.01)) return;
      this.pts.push([x, y]);
      this.pts.sort((a, b) => a[0] - b[0]);
      this.render();
      this.emit();
    });
    svg.addEventListener('contextmenu', e => {
      const [px, py] = pos(e);
      const i = hit(px, py);
      if (i < 0) return;
      e.preventDefault();
      remove(i);
    });
  }
}

// ---------------------------------------------------------------- state polling
let stateChain = Promise.resolve();
function refreshState() {
  const p = stateChain.then(pollState);
  stateChain = p.catch(e => { if (!(e instanceof OfflineError)) console.warn('auto-dark state:', e); });
  return stateChain;
}
async function pollState() {
  const st = await api('/api/state');
  await applyState(st);
}

async function applyState(st) {
  if (!st || typeof st !== 'object') return;
  if (!S.schema || (st.schemaVersion !== undefined && st.schemaVersion !== S.schemaVersion)) {
    const hadSchema = !!S.schema;
    const schema = await api('/api/schema');
    S.schemaVersion = st.schemaVersion;
    buildControls(schema);
    if (hadSchema) toast('Parameters reloaded (schema changed)');
  }
  S.state = st;

  // monitors first: the curve editor's level backdrop depends on them
  const monitors = Array.isArray(st.monitors) ? st.monitors : [];
  const sigStr = JSON.stringify(monitors.map(m => [m.index, m.name, m.width, m.height, m.primary, m.levels]));
  if (sigStr !== S.monSig) {
    S.monSig = sigStr;
    S.monitors = monitors;
    if (!monitors.some(m => m.index === S.mon)) S.mon = (monitors.find(m => m.primary) ?? monitors[0])?.index ?? 0;
    renderMonitorSelect();
    buildMeters();
    S.curves.forEach(c => c.render());
  }

  // params: take engine values unless the user is touching that control
  for (const [id, v] of Object.entries(st.params ?? {})) {
    const c = S.controls.get(id);
    if (!c) { S.params[id] = v; continue; }
    if (c.busy() || isPending(id) || recentlyEdited(id)) continue;
    if (!sameValue(S.params[id], v)) {
      S.params[id] = v;
      c.set(v);
      paramChanged(id);
    }
  }

  const editing = [...sender.keys()].some(id => isPending(id) || recentlyEdited(id));
  setDirty(!!st.dirty || editing);
  if (performance.now() > S.bypassHoldUntil && typeof st.bypass === 'boolean') setBypass(st.bypass);
  renderShaderError(st.shaderError);
  $('#banner-capture').hidden = st.captureExcluded !== false;

  if (st.profile !== undefined && st.profile !== S.activeProfile) {
    S.activeProfile = st.profile;
    renderProfiles();
    refreshProfiles().catch(() => {});
  }
}

function setDirty(d) {
  S.dirty = d;
  $('#dirty-dot').hidden = !d;
}
function setBypass(b) {
  S.bypass = b;
  const btn = $('#bypass');
  btn.setAttribute('aria-pressed', String(b));
  $('#bypass-sub').textContent = b ? 'filter OFF' : 'filter active';
  $('#banner-bypass').hidden = !b;
  document.body.classList.toggle('bypassed', b);
}
let lastShaderError;
function renderShaderError(err) {
  const text = err == null || err === '' ? null : String(err);
  if (text === lastShaderError) return;
  lastShaderError = text;
  $('#banner-shader').hidden = text == null;
  $('#shader-error').textContent = text ?? '';
}

// ---------------------------------------------------------------- profiles
async function refreshProfiles() {
  const r = await api('/api/profiles');
  S.profiles = Array.isArray(r?.profiles) ? r.profiles.map(String) : [];
  if (r?.active != null && S.activeProfile == null) S.activeProfile = r.active;
  renderProfiles();
}
function renderProfiles() {
  const sel = $('#profile-select');
  const active = S.activeProfile;
  const names = [...S.profiles];
  if (active != null && !names.includes(active)) names.unshift(active);
  sel.replaceChildren(...names.map(n => h('option', { value: n, text: n === active ? `● ${n}` : `   ${n}` })));
  if (active != null) sel.value = active;
  const has = active != null && active !== '';
  $('#btn-save').disabled = !has;
  $('#btn-revert').disabled = !has;
  $('#btn-delete').disabled = !has;
}

async function profileOp(op, name, okMsg) {
  try {
    await api(`/api/profile/${op}`, { method: 'POST', body: { name } });
    lastEdit.clear();
    if (op === 'load' || op === 'save') { S.activeProfile = name; renderProfiles(); }
    if (okMsg) toast(okMsg, 'ok');
  } catch (e) {
    toast(`Profile ${op} failed: ${e.message}`, 'error', 5000);
  }
  await refreshProfiles().catch(() => {});
  await refreshState();
}

// Must match the engine's rule (ParamStore::validName).
function validName(name) {
  return /^[A-Za-z0-9_\-](?:[A-Za-z0-9 _\-]{0,62}[A-Za-z0-9_\-])?$/.test(name);
}

function bindProfileBar() {
  const sel = $('#profile-select');
  sel.addEventListener('change', async () => {
    const name = sel.value, cur = S.activeProfile;
    if (name === cur) return;
    if (S.dirty && !(await ask({ title: 'Unsaved changes', message: `Discard unsaved changes to "${cur}" and load "${name}"?`, okLabel: 'Discard & load', danger: true }))) {
      sel.value = cur;
      return;
    }
    await profileOp('load', name, `Loaded "${name}"`);
  });
  $('#btn-save').addEventListener('click', () => {
    if (S.activeProfile) profileOp('save', S.activeProfile, `Saved "${S.activeProfile}"`);
  });
  $('#btn-saveas').addEventListener('click', async () => {
    const name = await ask({ title: 'Save as profile', message: 'Saves the current settings under a new name.',
      input: S.activeProfile ? `${S.activeProfile}-2` : 'my-profile', okLabel: 'Save' });
    if (name == null) return;
    if (S.profiles.includes(name) &&
        !(await ask({ title: 'Overwrite profile', message: `Profile "${name}" exists. Overwrite it?`, okLabel: 'Overwrite', danger: true }))) return;
    profileOp('save', name, `Saved as "${name}"`);
  });
  $('#btn-revert').addEventListener('click', async () => {
    const name = S.activeProfile;
    if (!name) return;
    if (S.dirty && !(await ask({ title: 'Revert', message: `Discard unsaved changes and reload "${name}"?`, okLabel: 'Revert', danger: true }))) return;
    profileOp('load', name, `Reverted to "${name}"`);
  });
  $('#btn-delete').addEventListener('click', async () => {
    const name = S.activeProfile;
    if (!name || !(await ask({ title: 'Delete profile', message: `Delete profile "${name}"? This removes its file from profiles/.`, okLabel: 'Delete', danger: true }))) return;
    profileOp('delete', name, `Deleted "${name}"`);
  });
}

// ---------------------------------------------------------------- bypass + control
function bindBypass() {
  $('#bypass').addEventListener('click', async () => {
    const want = !S.bypass;
    setBypass(want);
    S.bypassHoldUntil = performance.now() + 1500;
    try {
      await api('/api/control', { method: 'POST', body: { bypass: want } });
    } catch (e) {
      S.bypassHoldUntil = 0;
      setBypass(!want);
      toast(`Bypass failed: ${e.message}`, 'error');
    }
  });
  $('#btn-reset-temporal').addEventListener('click', async () => {
    try { await api('/api/control', { method: 'POST', body: { resetTemporal: true } }); toast('Temporal state reset', 'ok', 1800); }
    catch (e) { toast(`Reset failed: ${e.message}`, 'error'); }
  });
}

// ---------------------------------------------------------------- monitor / mode selectors, preview
function renderMonitorSelect() {
  const seg = $('#mon-seg');
  seg.replaceChildren(...S.monitors.map(m => h('button', {
    type: 'button',
    'aria-pressed': String(m.index === S.mon),
    title: `${m.name ?? ''}${m.primary ? ' (primary)' : ''}`,
    text: `${m.primary ? '★ ' : ''}${monLabel(m)}`,
    onclick: () => selectMonitor(m.index),
  })));
  seg.hidden = !S.monitors.length;
  updatePreviewFrame();
}
function selectMonitor(i) {
  if (S.mon === i) return;
  S.mon = i;
  store.set('mon', i);
  renderMonitorSelect();
  for (const [idx, els] of S.meterEls) els.root.classList.toggle('selected', idx === i);
  S.curves.forEach(c => c.render());
}
function renderModeSelect() {
  const seg = $('#mode-seg');
  seg.replaceChildren(...MODES.map((name, i) => h('button', {
    type: 'button', text: name, 'aria-pressed': String(i === S.mode),
    onclick: () => { S.mode = i; store.set('mode', i); renderModeSelect(); },
  })));
  $('#pv-badge').textContent = MODES[S.mode] ?? '';
}
function updatePreviewFrame() {
  const m = S.monitors.find(x => x.index === S.mon);
  $('#pv-frame').style.aspectRatio = m?.width && m?.height ? `${m.width} / ${m.height}` : '16 / 9';
}

async function previewLoop() {
  const img = $('#pv-img');
  let shownUrl = null, frames = 0, fpsT0 = performance.now();
  for (;;) {
    const t0 = performance.now();
    if (document.hidden || conn.online !== true || !S.monitors.length) {
      $('#pv-fps').textContent = '';
      frames = 0;
      await sleep(400);
      fpsT0 = performance.now();
      continue;
    }
    try {
      const res = await api(`/api/preview?mon=${encodeURIComponent(S.mon)}&mode=${S.mode}&t=${Date.now()}`, { raw: true, timeout: 4000 });
      if (res.status === 200) {
        const blob = await res.blob();
        if (blob.size) {
          const url = URL.createObjectURL(blob);
          const probe = new Image();
          probe.src = url;
          try {
            await probe.decode();
            img.src = url;
            if (shownUrl) URL.revokeObjectURL(shownUrl);
            shownUrl = url;
            frames++;
            $('#pv-empty').hidden = true;
          } catch {
            URL.revokeObjectURL(url);
          }
        }
      } // 204 (no frame yet) or error: keep the previous frame
    } catch { /* offline handled by conn */ }
    const now = performance.now();
    if (now - fpsT0 >= 1000) {
      $('#pv-fps').textContent = `preview ${(frames * 1000 / (now - fpsT0)).toFixed(1)} fps`;
      frames = 0;
      fpsT0 = now;
    }
    await sleep(Math.max(0, PREVIEW_MS - (performance.now() - t0)));
  }
}

// ---------------------------------------------------------------- meters (compressor-style)
const meterFrac = v => clamp((Math.log10(Math.max(v, 1e-9)) - Math.log10(METER_Y0)) / (Math.log10(METER_Y1) - Math.log10(METER_Y0)), 0, 1);
const gainEv = g => (g > 0 ? Math.max(0, -Math.log2(Math.min(1, g))) : GR_EV_MAX);

function buildMeters() {
  const box = $('#meters');
  S.meterEls.clear();
  if (!S.monitors.length) {
    box.replaceChildren(h('div', { class: 'meters-empty', text: 'no monitors reported' }));
    return;
  }
  const nodes = S.monitors.map(m => {
    const bar = cls => {
      const fill = h('div', { class: 'm-fill' });
      const peak = cls === 'gr' ? h('div', { class: 'm-peak' }) : null;
      const col = h('div', { class: 'm-barcol' }, h('div', { class: `m-bar ${cls}` }, fill, peak), h('b', { text: cls.toUpperCase() }));
      return { col, fill, peak };
    };
    const bin = bar('in'), bgr = bar('gr'), bout = bar('out');
    const els = {
      perf: h('span', { class: 'm-perf' }),
      spark: h('canvas', { class: 'm-spark' }),
      fin: bin.fill, fout: bout.fill, fgr: bgr.fill, peak: bgr.peak,
      vin: h('b'), vout: h('b'), vgr: h('b'), vev: h('i'),
    };
    els.root = h('div', { class: 'meter' + (m.index === S.mon ? ' selected' : ''), 'data-mon': m.index,
      title: 'Click to preview this monitor', onclick: () => selectMonitor(m.index) },
      h('div', { class: 'm-head' },
        h('span', { class: 'm-name' }, m.primary ? h('span', { class: 'm-star', text: '★ ' }) : null, monLabel(m)),
        els.perf),
      h('div', { class: 'm-body' },
        h('div', { class: 'm-bars', title: 'IN / gain reduction / OUT' }, bin.col, bgr.col, bout.col),
        els.spark),
      h('div', { class: 'm-foot' },
        h('span', { class: 'm-in' }, 'in ', els.vin),
        h('span', { class: 'm-out' }, 'out ', els.vout),
        h('span', { class: 'm-gr', title: 'Strongest dimming currently applied (minimum gain)' }, 'min gain ', els.vgr, ' ', els.vev)));
    S.meterEls.set(m.index, els);
    return els.root;
  });
  box.replaceChildren(...nodes);
}

let cssColors = null;
function colors() {
  if (!cssColors) {
    const cs = getComputedStyle(document.documentElement);
    const v = n => cs.getPropertyValue(n).trim();
    cssColors = { in: v('--in') || '#8e8e95', out: v('--out') || '#e0902f', grid: '#1f1f23', label: '#55555b' };
  }
  return cssColors;
}

function drawSpark(canvas, hist, now) {
  const dpr = window.devicePixelRatio || 1;
  const w = canvas.clientWidth, hgt = canvas.clientHeight;
  if (!w || !hgt) return;
  const W = Math.round(w * dpr), H = Math.round(hgt * dpr);
  if (canvas.width !== W || canvas.height !== H) { canvas.width = W; canvas.height = H; }
  const ctx = canvas.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, hgt);
  const c = colors();
  const padR = 30, pw = w - padR;
  const yOf = v => 2 + (1 - meterFrac(v)) * (hgt - 4);
  const xOf = t => pw * (1 - (now - t) / HISTORY_MS);

  ctx.font = '9.5px ui-monospace, Consolas, monospace';
  ctx.textBaseline = 'middle';
  for (const [v, lbl] of [[0.01, '1%'], [0.1, '10%'], [1, '100%']]) {
    const y = Math.round(yOf(v)) + 0.5;
    ctx.strokeStyle = c.grid;
    ctx.lineWidth = 1;
    ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(pw, y); ctx.stroke();
    ctx.fillStyle = c.label;
    ctx.fillText(lbl, pw + 4, clamp(y, 6, hgt - 6));
  }
  const pts = hist.filter(p => now - p.t <= HISTORY_MS + 500 && Number.isFinite(p.i) && Number.isFinite(p.o));
  if (pts.length < 2) return;

  ctx.save();
  ctx.beginPath(); ctx.rect(0, 0, pw, hgt); ctx.clip();
  // reduction band between input and output
  ctx.beginPath();
  pts.forEach((p, k) => { const x = xOf(p.t), y = yOf(p.i); k ? ctx.lineTo(x, y) : ctx.moveTo(x, y); });
  for (let k = pts.length - 1; k >= 0; k--) ctx.lineTo(xOf(pts[k].t), yOf(pts[k].o));
  ctx.closePath();
  ctx.fillStyle = 'rgba(224, 144, 47, 0.13)';
  ctx.fill();
  const line = (key, color, width) => {
    ctx.beginPath();
    pts.forEach((p, k) => { const x = xOf(p.t), y = yOf(p[key]); k ? ctx.lineTo(x, y) : ctx.moveTo(x, y); });
    ctx.strokeStyle = color; ctx.lineWidth = width; ctx.lineJoin = 'round'; ctx.stroke();
  };
  line('i', c.in, 1.2);
  line('o', c.out, 1.5);
  ctx.restore();
}

function applyStats(st) {
  if (!st || typeof st !== 'object') return;
  if (performance.now() > S.bypassHoldUntil && typeof st.bypass === 'boolean') setBypass(st.bypass);
  if ('shaderError' in st) renderShaderError(st.shaderError);
  for (const [gid, el] of S.groupStatus || []) {
    const text = st.groupStatus && typeof st.groupStatus[gid] === 'string' ? st.groupStatus[gid] : '';
    if (el.textContent !== text) el.textContent = text;
  }
  const now = performance.now();
  for (const m of Array.isArray(st.monitors) ? st.monitors : []) {
    let hist = S.hist.get(m.index);
    if (!hist) S.hist.set(m.index, hist = []);
    hist.push({ t: now, i: m.inMean, o: m.outMean, g: m.minGain });
    while (hist.length && now - hist[0].t > HISTORY_MS + 1000) hist.shift();

    const els = S.meterEls.get(m.index);
    if (!els) continue;
    const fps = num(m.fps, NaN), ms = num(m.frameMs, NaN);
    const lat = num(m.latMs, NaN), latMax = num(m.latMaxMs, NaN);
    els.perf.replaceChildren(
      h('b', { text: Number.isFinite(fps) ? fps.toFixed(1) : '—' }), ' fps  ',
      h('b', { text: Number.isFinite(ms) ? ms.toFixed(2) : '—' }), ' ms  lag ',
      h('b', { text: Number.isFinite(lat) && lat > 0 ? lat.toFixed(1) : '—' }),
      Number.isFinite(latMax) && latMax > 0 ? ` (max ${latMax.toFixed(1)})` : '', ' ms');
    els.perf.title = 'lag: time from Windows presenting a desktop frame to our overlay being presented for it';
    const inM = num(m.inMean, NaN), outM = num(m.outMean, NaN), g = num(m.minGain, NaN);
    els.fin.style.height = `${(Number.isFinite(inM) ? meterFrac(inM) : 0) * 100}%`;
    els.fout.style.height = `${(Number.isFinite(outM) ? meterFrac(outM) : 0) * 100}%`;
    const ev = Number.isFinite(g) ? gainEv(g) : 0;
    els.fgr.style.height = `${clamp(ev / GR_EV_MAX, 0, 1) * 100}%`;
    let pk = S.grPeak.get(m.index);
    if (!pk || ev >= pk.ev || now - pk.t > GR_PEAK_MS) S.grPeak.set(m.index, pk = { ev, t: now });
    els.peak.style.display = pk.ev > 0.05 ? 'block' : 'none';
    els.peak.style.top = `calc(${clamp(pk.ev / GR_EV_MAX, 0, 1) * 100}% - 1px)`;
    els.vin.textContent = Number.isFinite(inM) ? fmtPct(inM) : '—';
    els.vout.textContent = Number.isFinite(outM) ? fmtPct(outM) : '—';
    els.vgr.textContent = Number.isFinite(g) ? fmtPct(g) : '—';
    els.vev.textContent = Number.isFinite(g) ? (ev < 0.05 ? '(0 EV)' : `(−${ev.toFixed(1)} EV)`) : '';
    drawSpark(els.spark, hist, now);
  }
}

// ---------------------------------------------------------------- loops + boot
function every(fn, msFn) {
  const tick = async () => {
    const t0 = performance.now();
    try { await fn(); } catch (e) { if (!(e instanceof OfflineError)) console.warn('auto-dark:', e); }
    setTimeout(tick, Math.max(16, msFn() - (performance.now() - t0)));
  };
  tick();
}

function boot() {
  $('#offline-host').textContent = location.host || '127.0.0.1:7777';
  renderConn();
  renderModeSelect();
  renderMonitorSelect();
  buildMeters();
  bindProfileBar();
  bindBypass();
  every(refreshState, () => (conn.online === false ? OFFLINE_RETRY_MS : STATE_MS));
  every(async () => applyStats(await api('/api/stats')),
    () => (conn.online === false ? OFFLINE_RETRY_MS : document.hidden ? 2000 : STATS_MS));
  previewLoop();
  setInterval(() => { if (conn.online === false) renderConn(); }, 1000);
  window.addEventListener('resize', () => { for (const [i, els] of S.meterEls) drawSpark(els.spark, S.hist.get(i) ?? [], performance.now()); });
}

boot();
})();
