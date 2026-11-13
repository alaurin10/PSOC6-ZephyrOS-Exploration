'use strict';

// Live dashboard for the PSoC 6 sound board. Everything shown comes from the
// board over one WebSocket; controls only send commands, and the board's
// answer (a "ui" message) is what updates them, so every open page agrees.

const $ = (id) => document.getElementById(id);

// Colour schemes (as many as the board's UI_COLOUR_COUNT): gradient stops
// for band level 0..1.
const SCHEMES = [
  ['#00261f', '#00a37a', '#00b3ff', '#b46bff'], // aurora
  ['#260803', '#c41f33', '#ff7a00', '#ffe066'], // ember
  ['#050f26', '#1b3b8b', '#2d9cdb', '#c4f4ff'], // ocean
  ['#101010', '#505050', '#a0a0a0', '#ffffff'], // mono
];

const LEVEL_SECONDS = 30;
const TEMP_POINTS = 300;
const DB_MIN = -100; // band value 0, and the bottom of the level graphs

const st = {
  hello: null,
  ui: null,
  bands: new Array(64).fill(0),
  peaks: new Array(64).fill(0),
  lastFrame: 0,
  newFrame: false,
  levels: [],
  temps: [],
  claps: 0,
  palettes: SCHEMES.map(buildPalette),
  dragging: null,
};

// ---- Colours ---------------------------------------------------------------

function hexToRgb(h) {
  const n = parseInt(h.slice(1), 16);
  return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
}

// 101 colours (band value 0..100) interpolated along the scheme's stops.
function buildPalette(stops) {
  const rgb = stops.map(hexToRgb);
  const out = [];
  for (let v = 0; v <= 100; v++) {
    const x = (v / 100) * (rgb.length - 1);
    const i = Math.min(Math.floor(x), rgb.length - 2);
    const f = x - i;
    const c = rgb[i].map((a, k) => Math.round(a + (rgb[i + 1][k] - a) * f));
    out.push({ rgb: c, css: `rgb(${c[0]},${c[1]},${c[2]})` });
  }
  return out;
}

const palette = () => st.palettes[(st.ui && st.ui.colour) || 0];

// ---- Canvas helpers ----------------------------------------------------------

function fit(canvas) {
  const r = canvas.getBoundingClientRect();
  const dpr = window.devicePixelRatio || 1;
  const w = Math.max(1, Math.round(r.width * dpr));
  const h = Math.max(1, Math.round(r.height * dpr));
  if (canvas.width !== w || canvas.height !== h) {
    canvas.width = w;
    canvas.height = h;
  }
  return canvas.getContext('2d');
}

function sparkline(canvas, values, lo, hi, color) {
  const ctx = fit(canvas);
  const { width: w, height: h } = canvas;
  ctx.clearRect(0, 0, w, h);
  if (values.length < 2) return;
  ctx.beginPath();
  values.forEach((v, i) => {
    const x = (i / (values.length - 1)) * w;
    const y = h - ((Math.min(hi, Math.max(lo, v)) - lo) / (hi - lo)) * (h - 2) - 1;
    i ? ctx.lineTo(x, y) : ctx.moveTo(x, y);
  });
  ctx.strokeStyle = color;
  ctx.lineWidth = 1.5 * (window.devicePixelRatio || 1);
  ctx.stroke();
}

// ---- Spectrum views ------------------------------------------------------------

const waterfall = document.createElement('canvas');

function dbGrid(ctx, w, h) {
  ctx.strokeStyle = 'rgba(255,255,255,0.06)';
  ctx.lineWidth = 1;
  ctx.font = `${11 * (window.devicePixelRatio || 1)}px system-ui`;
  ctx.fillStyle = 'rgba(255,255,255,0.3)';
  for (let v = 20; v < 100; v += 20) {
    const y = Math.round(h - (v / 100) * h) + 0.5;
    ctx.beginPath();
    ctx.moveTo(0, y);
    ctx.lineTo(w, y);
    ctx.stroke();
    ctx.fillText(`${v + DB_MIN} dB`, 4, y - 3);
  }
}

function drawBars(ctx, w, h) {
  const pal = palette();
  const n = st.bands.length;
  const bw = w / n;
  for (let i = 0; i < n; i++) {
    const v = st.bands[i];
    const bh = (v / 100) * h;
    ctx.fillStyle = pal[v].css;
    ctx.fillRect(i * bw + 1, h - bh, Math.max(1, bw - 2), bh);
    const py = h - (st.peaks[i] / 100) * h;
    ctx.fillStyle = pal[100].css;
    ctx.fillRect(i * bw + 1, py - 2, Math.max(1, bw - 2), 2);
  }
  dbGrid(ctx, w, h); // over the bars, so the labels stay readable
}

function drawLine(ctx, w, h) {
  const pal = palette();
  const n = st.bands.length;
  const x = (i) => ((i + 0.5) / n) * w;
  dbGrid(ctx, w, h);
  const grad = ctx.createLinearGradient(0, h, 0, 0);
  [0, 50, 100].forEach((v) => grad.addColorStop(v / 100, pal[v].css));
  ctx.beginPath();
  ctx.moveTo(0, h);
  st.bands.forEach((v, i) => ctx.lineTo(x(i), h - (v / 100) * h));
  ctx.lineTo(w, h);
  ctx.closePath();
  ctx.globalAlpha = 0.35;
  ctx.fillStyle = grad;
  ctx.fill();
  ctx.globalAlpha = 1;
  ctx.lineWidth = 2 * (window.devicePixelRatio || 1);
  ctx.strokeStyle = pal[85].css;
  ctx.beginPath();
  st.bands.forEach((v, i) => (i ? ctx.lineTo : ctx.moveTo).call(ctx, x(i), h - (v / 100) * h));
  ctx.stroke();
  ctx.setLineDash([4, 4]);
  ctx.lineWidth = 1;
  ctx.strokeStyle = pal[100].css;
  ctx.beginPath();
  st.peaks.forEach((v, i) => (i ? ctx.lineTo : ctx.moveTo).call(ctx, x(i), h - (v / 100) * h));
  ctx.stroke();
  ctx.setLineDash([]);
}

// One new row per frame, at the bottom; older rows scroll up. The history is
// kept up to date in every view, so switching to the waterfall shows it at once.
function updateWaterfall(w, h) {
  if (waterfall.width !== w || waterfall.height !== h) {
    waterfall.width = w;
    waterfall.height = h;
  }
  const wctx = waterfall.getContext('2d');
  const row = Math.max(2, Math.round(h / 160));
  wctx.drawImage(waterfall, 0, -row);
  const pal = palette();
  const bw = w / st.bands.length;
  st.bands.forEach((v, i) => {
    wctx.fillStyle = pal[v].css;
    wctx.fillRect(Math.floor(i * bw), h - row, Math.ceil(bw), row);
  });
}

function drawWaterfall(ctx) {
  ctx.drawImage(waterfall, 0, 0);
}

function drawLevel(ctx, w, h) {
  const pal = palette();
  const last = st.levels[st.levels.length - 1] || { r: DB_MIN, p: DB_MIN };
  const toX = (db) => ((Math.max(DB_MIN, Math.min(0, db)) - DB_MIN) / -DB_MIN) * w;
  const meterH = h * 0.18;
  // RMS bar with the peak as a marker, over a dB scale.
  ctx.fillStyle = '#20252e';
  ctx.fillRect(0, 0, w, meterH);
  const grad = ctx.createLinearGradient(0, 0, w, 0);
  [0, 60, 85, 100].forEach((v) => grad.addColorStop(v / 100, pal[v].css));
  ctx.fillStyle = grad;
  ctx.fillRect(0, 0, toX(last.r), meterH);
  ctx.fillStyle = '#fff';
  ctx.fillRect(toX(last.p) - 2, 0, 4, meterH);
  ctx.font = `${12 * (window.devicePixelRatio || 1)}px system-ui`;
  ctx.fillStyle = 'rgba(255,255,255,0.45)';
  for (let db = -90; db <= 0; db += 10) ctx.fillText(`${db}`, toX(db) + 2, meterH + 16);
  // History of the last 30 s below it.
  const top = meterH + 28;
  const gh = h - top;
  ctx.strokeStyle = pal[85].css;
  ctx.lineWidth = 2 * (window.devicePixelRatio || 1);
  ctx.beginPath();
  st.levels.forEach((l, i) => {
    const x = (i / Math.max(1, st.levels.length - 1)) * w;
    const y = top + gh - ((Math.max(DB_MIN, l.r) - DB_MIN) / -DB_MIN) * gh;
    i ? ctx.lineTo(x, y) : ctx.moveTo(x, y);
  });
  ctx.stroke();
}

const VIEWS = { bars: drawBars, line: drawLine, waterfall: drawWaterfall, level: drawLevel };

function render(now) {
  const canvas = $('spectrum');
  const ctx = fit(canvas);
  const { width: w, height: h } = canvas;
  // Peak caps fall back at ~25 units per second.
  const dt = Math.min(0.1, (now - (render.last || now)) / 1000);
  render.last = now;
  st.peaks = st.peaks.map((p, i) => Math.max(st.bands[i], p - 25 * dt));

  const view = (st.ui && st.ui.view) || 'bars';
  if (st.newFrame && st.bands.length) updateWaterfall(w, h);
  ctx.clearRect(0, 0, w, h);
  (VIEWS[view] || drawBars)(ctx, w, h);
  st.newFrame = false;
  requestAnimationFrame(render);
}

// ---- Messages from the board -------------------------------------------------------

const fmtDb = (cdb) => (cdb <= -11999 ? '-inf' : (cdb / 100).toFixed(1));
const clock = () => new Date().toLocaleTimeString();

function onHello(m) {
  st.hello = m;
  st.bands = new Array(m.bands.length).fill(0);
  st.peaks = new Array(m.bands.length).fill(0);
  const fps = 16;
  st.levelMax = LEVEL_SECONDS * fps;

  // Frequency axis: a few round numbers at the band that contains them.
  const axis = $('axis');
  axis.innerHTML = '';
  [50, 100, 200, 500, 1000, 2000, 5000].forEach((f) => {
    let best = 0;
    m.bands.forEach((c, i) => {
      if (Math.abs(Math.log(c / f)) < Math.abs(Math.log(m.bands[best] / f))) best = i;
    });
    const s = document.createElement('span');
    s.style.left = `${((best + 0.5) / m.bands.length) * 100}%`;
    s.textContent = f >= 1000 ? `${f / 1000}k` : `${f}`;
    axis.appendChild(s);
  });
  axis.lastChild.textContent += ' Hz';

  segment($('views'), m.views, (v) => send({ cmd: 'view', name: v }));
  segment($('slider-sel'), m.params, (p) => send({ cmd: 'slider', name: p }));
  const sw = $('colours');
  sw.innerHTML = '';
  SCHEMES.slice(0, m.colours).forEach((stops, i) => {
    const b = document.createElement('button');
    b.style.background = `linear-gradient(90deg, ${stops.join(',')})`;
    b.title = `Colour scheme ${i}`;
    b.onclick = () => send({ cmd: 'colour', value: i });
    sw.appendChild(b);
  });
}

function segment(el, names, onPick) {
  el.innerHTML = '';
  names.forEach((n) => {
    const b = document.createElement('button');
    b.textContent = n;
    b.dataset.name = n;
    b.onclick = () => onPick(n);
    el.appendChild(b);
  });
}

function markSegment(el, name) {
  [...el.children].forEach((b) => b.classList.toggle('on', b.dataset.name === name));
}

function onUi(m) {
  st.ui = m;
  markSegment($('views'), m.view);
  markSegment($('slider-sel'), m.slider);
  [...$('colours').children].forEach((b, i) => b.classList.toggle('on', i === m.colour));
  for (const [name, value] of Object.entries(m.param)) {
    if (st.dragging !== name) $(`p-${name}`).value = value;
    $(`o-${name}`).textContent = value;
  }
  $('freeze').checked = !!m.freeze;
  $('ledclap').checked = !!m.ledclap;
  $('frozen').classList.toggle('hidden', !m.freeze);
  $('slider-target').textContent = m.slider;
  $('last-change').textContent = `Last change from ${m.src} at ${clock()}`;
}

function onFrame(m) {
  st.bands = m.b;
  st.newFrame = true;
  st.levels.push({ r: m.r / 100, p: m.p / 100 });
  if (st.levels.length > (st.levelMax || 480)) st.levels.shift();
  $('rms').textContent = fmtDb(m.r);
  $('peak').textContent = fmtDb(m.p);
  if (m.c) showClip();
  sparkline($('level-graph'), st.levels.map((l) => l.r), DB_MIN, 0, '#33c3ff');
}

function onTemp(m) {
  if (!m.n) return;
  $('temp').textContent = (m.c / 100).toFixed(2);
  st.temps.push(m.c / 100);
  if (st.temps.length > TEMP_POINTS) st.temps.shift();
  const lo = Math.min(...st.temps) - 0.2;
  const hi = Math.max(...st.temps) + 0.2;
  sparkline($('temp-graph'), st.temps, lo, hi, '#ffb347');
}

function onInput(m) {
  $('b0').classList.toggle('on', !!m.b0);
  $('b1').classList.toggle('on', !!m.b1);
  const pos = $('cs-pos');
  pos.style.display = m.s < 0 ? 'none' : 'block';
  if (m.s >= 0) pos.style.left = `${(m.s / m.max) * 100}%`;
}

function onNet(m) {
  const b = $('net-badge');
  b.classList.toggle('off', m.state !== 'online');
  b.textContent = m.state === 'online' ? `${m.ssid} ${m.rssi} dBm` : `Wi-Fi ${m.state}`;
}

let clipTimer;
function showClip() {
  $('clip').classList.add('on');
  clearTimeout(clipTimer);
  clipTimer = setTimeout(() => $('clip').classList.remove('on'), 1000);
}

function onEvent(m) {
  if (m.e === 'clip') {
    showClip();
    return;
  }
  st.claps++;
  $('claps').textContent = st.claps;
  const flash = $('clap-flash');
  flash.textContent = m.e === 'double' ? 'DOUBLE CLAP' : 'CLAP';
  flash.classList.remove('hidden');
  flash.style.animation = 'none';
  void flash.offsetWidth; // restart the CSS animation
  flash.style.animation = '';
  const li = document.createElement('li');
  li.textContent = `${clock()}  ${m.e === 'double' ? 'double clap' : 'clap'}, peak ${fmtDb(m.p)} dBFS`;
  const list = $('events');
  list.prepend(li);
  while (list.children.length > 5) list.lastChild.remove();
}

const HANDLERS = { hello: onHello, ui: onUi, f: onFrame, temp: onTemp, in: onInput, net: onNet, ev: onEvent };

// ---- Connection ----------------------------------------------------------------

// The board sends something at least every 2 s ("ka" when there's nothing else).
// Silence means the connection is dead even if the browser still thinks it's open:
// a board that lost its Wi-Fi drops the connection without telling the browser.
const SILENCE_MS = 5000;

let ws = null;
let retryMs = 500;
let lastRx = 0;

function dropped(text) {
  const b = $('ws-badge');
  b.textContent = text;
  b.classList.add('off');
  setTimeout(connect, retryMs);
  retryMs = Math.min(retryMs * 2, 5000);
}

function connect() {
  const sock = new WebSocket(`ws://${location.host}/ws`);
  ws = sock;
  lastRx = Date.now();
  sock.onmessage = (e) => {
    lastRx = Date.now();
    const m = JSON.parse(e.data);
    if (m.t === 'hello') {
      // Live once the board has accepted this page (it may refuse when busy).
      retryMs = 500;
      const b = $('ws-badge');
      b.textContent = 'live';
      b.classList.remove('off');
    }
    const h = HANDLERS[m.t];
    if (h) h(m);
  };
  sock.onclose = (e) => {
    if (ws !== sock) return; // already given up on this one
    ws = null;
    // 1013: the board already serves its maximum number of dashboards.
    dropped(e.code === 1013 ? 'board busy, retrying' : 'offline, reconnecting');
  };
}

setInterval(() => {
  if (ws && Date.now() - lastRx > SILENCE_MS) {
    // Don't wait for a close handshake the board will never answer.
    const sock = ws;
    ws = null;
    sock.close();
    dropped('no data, reconnecting');
  }
}, 1000);

function send(obj) {
  if (ws && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(obj));
}

// ---- Data log ----------------------------------------------------------------------

const size = (b) => (b >= 1048576 ? `${(b / 1048576).toFixed(1)} MB` : `${Math.ceil(b / 1024)} KB`);

async function loadLog() {
  if (document.hidden) return;
  try {
    const info = await (await fetch('/api/logs')).json();
    if (!info.mounted) {
      $('log-summary').textContent = 'not available';
      return;
    }
    $('log-summary').textContent =
      `${info.files.length} file${info.files.length === 1 ? '' : 's'}, ` +
      `${size(info.free_kb * 1024)} free of ${size(info.total_kb * 1024)}, ` +
      `saved every ${info.sync_s} s`;
    $('log-files').replaceChildren(...info.files.map((f) => {
      const a = document.createElement('a');
      a.href = `/api/log?file=${f.name}`;
      a.download = f.name;
      a.title = f.name === info.current ? 'Being written; downloads everything so far' : 'Download';
      a.textContent = `${f.name} · ${size(f.size)}`;
      a.classList.toggle('cur', f.name === info.current);
      return a;
    }));
    $('log-tail').textContent = await (await fetch('/api/log?tail=12')).text();
  } catch (e) {
    $('log-summary').textContent = 'offline';
  }
}

// ---- Controls ----------------------------------------------------------------------

['gain', 'smooth', 'clap'].forEach((name) => {
  const input = $(`p-${name}`);
  let timer = null;
  const push = () => {
    timer = null;
    send({ cmd: 'param', name, value: Number(input.value) });
  };
  input.addEventListener('pointerdown', () => { st.dragging = name; });
  input.addEventListener('pointerup', () => { st.dragging = null; });
  input.addEventListener('input', () => {
    $(`o-${name}`).textContent = input.value;
    if (!timer) timer = setTimeout(push, 80); // at most ~12 commands per second
  });
  input.addEventListener('change', push);
});

// Checkboxes show the board's state: undo the local toggle and ask the board.
['freeze', 'ledclap'].forEach((cmd) => {
  $(cmd).addEventListener('change', (e) => {
    const want = e.target.checked ? 1 : 0;
    e.target.checked = !!(st.ui && st.ui[cmd]);
    send({ cmd, value: want });
  });
});

document.addEventListener('keydown', (e) => {
  if (e.target.tagName === 'INPUT') return;
  const k = e.key.toLowerCase();
  if (k === 'v') send({ cmd: 'view', name: 'next' });
  if (k === 'c') send({ cmd: 'colour', name: 'next' });
  if (k === 'f' && st.ui) send({ cmd: 'freeze', value: st.ui.freeze ? 0 : 1 });
});

connect();
requestAnimationFrame(render);
loadLog();
setInterval(loadLog, 10000);
