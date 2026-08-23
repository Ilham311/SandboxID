'use strict';

const MODDIR = '/data/adb/modules/sandboxid';
const BIN = `${MODDIR}/bin/sandboxid`;
const ROTATE_SH = `${MODDIR}/rotate_ids.sh`;
const IDENTITY = `${MODDIR}/identity.prop`;
const TARGETS = `${MODDIR}/target.txt`;

const ROTATE_LOG = `${MODDIR}/debug/rotate.log`;
const ACTION_LOG = `${MODDIR}/debug/action.log`;

function shq(s) { return "'" + String(s).replace(/'/g, "'\\''") + "'"; }

const ENV = `cd ${shq(MODDIR)} && export MODDIR=${shq(MODDIR)} && export PATH=${shq(MODDIR + '/bin')}:\"$PATH\"`;

// Palet warna per-brand: aksen UI ganti sesuai brand yang lagi aktif biar
// "meriah" & langsung ketahuan ini device apa. Di-set via CSSOM setProperty
// (bukan atribut style inline yang diblok CSP style-src 'self').
const BRAND_ACCENT = {
  google: '#4285f4', samsung: '#2e6be6', xiaomi: '#ff6900', redmi: '#ff453a',
  poco: '#ffd400', oppo: '#12b981', vivo: '#00a6ff', infinix: '#00c2a8',
};
const DEFAULT_ACCENT = '#3ba1ff';

function hexToRgb(hex) {
  const h = String(hex).replace('#', '');
  const n = h.length === 3 ? h.split('').map(c => c + c).join('') : h;
  return { r: parseInt(n.slice(0, 2), 16), g: parseInt(n.slice(2, 4), 16), b: parseInt(n.slice(4, 6), 16) };
}
function mixHex(hex, target, t) { // t=0 -> hex, t=1 -> target
  const a = hexToRgb(hex), b = hexToRgb(target);
  const to2 = v => Math.max(0, Math.min(255, Math.round(v))).toString(16).padStart(2, '0');
  return `#${to2(a.r + (b.r - a.r) * t)}${to2(a.g + (b.g - a.g) * t)}${to2(a.b + (b.b - a.b) * t)}`;
}
function setAccent(hex) {
  const base = /^#?[0-9a-fA-F]{3}([0-9a-fA-F]{3})?$/.test(String(hex || '')) ? hex : DEFAULT_ACCENT;
  const { r, g, b } = hexToRgb(base);
  const root = document.documentElement.style;
  root.setProperty('--accent', base);
  root.setProperty('--accent-hi', mixHex(base, '#ffffff', 0.18));
  root.setProperty('--accent-lo', mixHex(base, '#000000', 0.12));
  root.setProperty('--accent-soft', `rgba(${r}, ${g}, ${b}, .16)`);
}
function accentForBrand(brand) {
  return BRAND_ACCENT[String(brand || '').trim().toLowerCase()] || DEFAULT_ACCENT;
}

function exec(cmd) {
  return new Promise((resolve, reject) => {
    if (typeof ksu === 'undefined' || !ksu.exec) {
      reject(new Error('root bridge tidak tersedia'));
      return;
    }
    const cbName = `__ksucb_${Date.now()}_${Math.random().toString(36).slice(2, 8)}`;
    window[cbName] = function (errno, stdout, stderr) {
      try { delete window[cbName]; } catch (e) { window[cbName] = undefined; }
      const code = Number(errno);
      const out = String(stdout || '');
      const err = String(stderr || '');
      if (code === 0) {
        resolve(out);
      } else {
        const msg = (err.trim() || out.trim() || `exit ${code}`);
        reject(Object.assign(new Error(msg), { code, stdout: out, stderr: err }));
      }
    };
    try {
      ksu.exec(cmd, '{}', cbName);
    } catch (e) {
      try { delete window[cbName]; } catch (_) {}
      reject(e);
    }
  });
}

async function shell(cmd) { return exec(cmd); }

async function run(cmd) {
  try { return { ok: true, out: await shell(cmd) }; }
  catch (e) { return { ok: false, err: e }; }
}

const ICON = { ok: '\u2713', error: '\u2715', warn: '\u26a0', info: '\u2139' };
const T = {};

function toastInit() {
  T.el = document.getElementById('toast');
  T.icon = document.getElementById('toastIcon');
  T.title = document.getElementById('toastTitle');
  T.exp = document.getElementById('toastExp');
  T.detail = document.getElementById('toastDetail');
  document.getElementById('toastClose').addEventListener('click', hideToast);
  T.exp.addEventListener('click', toggleToast);
  T.title.addEventListener('click', () => { if (!T.exp.hidden) toggleToast(); });
}

function toast(title, opts) {
  opts = opts || {};
  const kind = opts.kind || 'info';
  const detail = String(opts.detail || '').trim();
  const hasDetail = detail.length > 0;
  T.icon.textContent = ICON[kind] || ICON.info;
  T.title.textContent = title || '';
  T.el.className = 'toast show ' + kind;
  T.exp.hidden = !hasDetail;
  T.detail.hidden = true;
  T.detail.innerHTML = hasDetail ? renderLogHtml(detail) : '';
  T.title.style.cursor = hasDetail ? 'pointer' : 'default';
  clearTimeout(toast._t);
  const sticky = opts.sticky || kind === 'error';
  if (!sticky) toast._t = setTimeout(hideToast, kind === 'warn' ? 4200 : 2600);
}

function toggleToast() {
  const open = T.el.classList.toggle('open');
  T.detail.hidden = !open;
  if (open) clearTimeout(toast._t);
}

function hideToast() {
  clearTimeout(toast._t);
  T.el.className = 'toast';
  T.detail.hidden = true;
}

function trimTitle(s) {
  const first = String(s || '').split('\n').map(x => x.trim()).filter(Boolean)[0] || 'Error';
  return first.length > 90 ? first.slice(0, 89) + '\u2026' : first;
}

async function safeExec(cmd, okMsg) {
  try {
    const out = await shell(cmd);
    if (okMsg) toast(okMsg, { kind: 'ok' });
    return { ok: true, out };
  } catch (e) {
    toast(trimTitle(e.message || String(e)), { kind: 'error', detail: e.stdout || e.stderr || '' });
    return { ok: false, err: e };
  }
}

async function withLoading(btn, fn) {
  if (!btn || btn.dataset.busy) return;
  btn.dataset.busy = '1';
  btn.classList.add('loading');
  btn.disabled = true;
  btn.setAttribute('aria-busy', 'true');
  try { return await fn(); }
  finally {
    btn.disabled = false;
    btn.classList.remove('loading');
    btn.removeAttribute('aria-busy');
    delete btn.dataset.busy;
  }
}

function classifyLine(raw) {
  let ts = '';
  let rest = raw;
  const m = raw.match(/^(\[\d{4}-\d\d-\d\d[ T]\d\d:\d\d:\d\d\])\s?(.*)$/);
  if (m) { ts = m[1]; rest = m[2]; }
  let lvl = 'info';
  if (/^==>/.test(rest)) lvl = 'step';
  else if (/^\[OK\]/.test(rest) || /^OK\b/.test(rest)) lvl = 'ok';
  else if (/^\[WARN\]/.test(rest)) lvl = 'warn';
  else if (/^\[ERR\]/.test(rest) || /^!/.test(rest)) lvl = 'err';
  else {
    const lc = rest.match(/^\d\d-\d\d \d\d:\d\d:\d\d\.\d+\s+([VDIWEF])\//);
    const p = lc ? lc[1] : '';
    if (p === 'E' || p === 'F') lvl = 'err';
    else if (p === 'W') lvl = 'warn';
    else if (p === 'V' || p === 'D') lvl = 'info';
  }
  return { ts, rest, lvl };
}

function renderLogHtml(text) {
  return String(text).replace(/\r/g, '').split('\n').map(line => {
    if (line === '') return '<div class="ln">&nbsp;</div>';
    const c = classifyLine(line);
    const ts = c.ts ? `<span class="ts">${escapeHtml(c.ts)}</span> ` : '';
    return `<div class="ln lvl-${c.lvl}">${ts}${escapeHtml(c.rest)}</div>`;
  }).join('');
}

function summarizeAction(out) {
  const text = String(out || '');
  // jalur multibrand baru (action.sh)
  if (/^OK - persona baru aktif/m.test(text)) {
    const b = (text.match(/^\s*BRAND\s*:\s*(.+)$/m) || [])[1];
    const md = (text.match(/^\s*MODEL\s*:\s*(.+)$/m) || [])[1];
    const label = [b && b.trim(), md && md.trim()].filter(Boolean).join(' \u00b7 ');
    return { kind: 'ok', title: label ? `Device baru \u00b7 ${label}` : 'Device baru aktif', detail: text };
  }
  // jalur cadangan freshen (Pixel bawaan)
  if (/^OK - fresh/m.test(text)) {
    const md = (text.match(/^\s*MODEL\s*:\s*(.+)$/m) || [])[1];
    return { kind: 'ok', title: md ? `Persona baru \u00b7 ${md.trim()}` : 'Persona baru siap', detail: text };
  }
  const bang = (text.match(/^[\u2717!].*$/m) || [])[0];
  return { kind: 'error', title: trimTitle(bang || text || 'Undi device gagal'), detail: text };
}

function summarizeRotate(out, label) {
  const text = String(out || '');
  const errs = (text.match(/\[ERR\]/g) || []).length;
  const warns = (text.match(/\[WARN\]/g) || []).length;
  const fail = text.match(/(\d+) step\(s\) reported failure/);
  const reboot = /REBOOT REQUIRED/i.test(text);
  const name = label || 'Rotate';
  if (errs > 0 || (fail && Number(fail[1]) > 0)) {
    const n = fail ? fail[1] : String(errs);
    return { kind: 'error', title: `${name}: ${n} langkah gagal`, detail: text };
  }
  let note = '';
  let kind = 'ok';
  if (reboot) { note = ' \u00b7 perlu reboot'; kind = 'warn'; }
  else if (warns > 0) { note = ` \u00b7 ${warns} warning`; kind = 'warn'; }
  return { kind, title: `${name} selesai${note}`, detail: text };
}

function wireTabs() {
  document.querySelectorAll('.tab').forEach(btn => {
    btn.addEventListener('click', () => {
      document.querySelectorAll('.tab').forEach(b => b.classList.toggle('active', b === btn));
      const id = btn.dataset.tab;
      document.querySelectorAll('.page').forEach(p => p.classList.toggle('active', p.id === id));
      moveIndicator();
      onTab(id);
    });
  });
}

function moveIndicator() {
  const nav = document.getElementById('nav');
  const ind = document.getElementById('navInd');
  const btn = nav && nav.querySelector('.tab.active');
  if (!nav || !ind || !btn) return;
  ind.style.width = btn.offsetWidth + 'px';
  ind.style.transform = `translateX(${btn.offsetLeft - nav.scrollLeft}px)`;
}

function onTab(id) {
  if (id === 'persona') loadPersona();
  else if (id === 'rotate') loadRotate();
  else if (id === 'targets') loadTargets();
  else if (id === 'log') loadLog();
}

function parseProp(text) {
  const out = {};
  for (const line of text.split(/\r?\n/)) {
    const t = line.trim();
    if (!t || t.startsWith('#')) continue;
    const eq = t.indexOf('=');
    if (eq <= 0) continue;
    out[t.slice(0, eq).trim()] = t.slice(eq + 1).trim();
  }
  return out;
}

function skLines(n) {
  let s = '';
  for (let i = 0; i < n; i++) s += `<div class="ln sk sk-line${i % 3 === 2 ? ' short' : ''}"></div>`;
  return s;
}

function skKv(n) {
  let s = '';
  for (let i = 0; i < n; i++) s += '<div class="k sk sk-line short"></div><div class="v sk sk-line"></div>';
  return s;
}

// grid detail (di bawah hero + tiles). label ramah, key = kosakata identity.prop.
const DETAIL_KEYS = [
  ['MANUFACTURER', 'Pabrikan'], ['PRODUCT', 'Product'], ['BOARD', 'Board'],
  ['SOC_MANUFACTURER', 'SoC vendor'], ['SOC_MODEL', 'SoC'],
  ['SECURITY_PATCH', 'Security patch'],
  ['SERIAL', 'Serial'], ['ANDROID_ID', 'Android ID'], ['GOOGLE_AID', 'Google AID'],
  ['WIFI_MAC', 'WiFi MAC'], ['BLUETOOTH_ADDR', 'BT MAC'], ['BLUETOOTH_NAME', 'Nama BT'],
  ['RADIO', 'Radio'], ['FIRST_BOOT', 'Boot awal'], ['LAST_BOOT', 'Boot terakhir'],
];

function renderHero(kv) {
  const brand = kv.BRAND || '';
  const mkt = kv.MARKETNAME || kv.MODEL || '(tak dikenal)';
  const sub = [kv.MODEL, kv.DEVICE].filter(Boolean).join(' \u00b7 ');
  const rel = kv.RELEASE || '', sdk = kv.SDK_INT || '';
  const os = rel
    ? `Android ${escapeHtml(rel)}${sdk ? ` \u00b7 SDK ${escapeHtml(sdk)}` : ''}`
    : '';
  return `${brand ? `<span class="brandchip">${escapeHtml(brand)}</span>` : ''}` +
    `<div class="mkt">${escapeHtml(mkt)}</div>` +
    `${sub ? `<div class="mdl">${escapeHtml(sub)}</div>` : ''}` +
    `${os ? `<div class="os">${os}</div>` : ''}` +
    `${kv.FINGERPRINT ? `<div class="fp">${escapeHtml(kv.FINGERPRINT)}</div>` : ''}`;
}

function renderTiles(kv) {
  const t = [];
  if (kv.BOOT_COUNT)
    t.push(`<div class="tile boot"><div class="tlabel">Boot count</div><div class="tval">${escapeHtml(kv.BOOT_COUNT)}</div><div class="tsub">kali reboot</div></div>`);
  const up = kv.UPTIME_HUMAN || (kv.UPTIME_SECONDS ? kv.UPTIME_SECONDS + 's' : '');
  if (up)
    t.push(`<div class="tile up"><div class="tlabel">Uptime</div><div class="tval">${escapeHtml(up)}</div><div class="tsub">nyala terus</div></div>`);
  if (kv.FRESH) {
    const yes = /^(y|yes|true|1)$/i.test(kv.FRESH.trim());
    t.push(`<div class="tile fresh"><div class="tlabel">Fresh</div><div class="tval ${yes ? 'yes' : 'no'}">${yes ? 'Ya' : 'Nggak'}</div><div class="tsub">baru direset?</div></div>`);
  }
  if (kv.USAGE_PROFILE)
    t.push(`<div class="tile usage"><div class="tlabel">Pemakaian</div><div class="tval">${escapeHtml(kv.USAGE_PROFILE)}</div><div class="tsub">pola pakai</div></div>`);
  return t.join('');
}

async function loadPersona() {
  const hero = document.getElementById('hero');
  const tiles = document.getElementById('tiles');
  const el = document.getElementById('identity');
  hero.innerHTML = '';
  tiles.innerHTML = '';
  el.className = 'kv';
  el.innerHTML = skKv(6);
  const r = await safeExec(`cat ${shq(IDENTITY)} 2>/dev/null || true`);
  if (!r.ok || !r.out.trim()) {
    setAccent(DEFAULT_ACCENT);
    hero.innerHTML = '<div class="empty">Belum ada device. Tap \ud83c\udfb2 Undi device baru buat mulai.</div>';
    el.className = 'kv';
    el.innerHTML = '<div class="empty">identity.prop belum ada.</div>';
    return;
  }
  const kv = parseProp(r.out);
  setAccent(accentForBrand(kv.BRAND));
  hero.innerHTML = renderHero(kv);
  tiles.innerHTML = renderTiles(kv);
  // stagger animasi tile via CSSOM (--i), aman terhadap CSP.
  tiles.querySelectorAll('.tile').forEach((elt, i) => elt.style.setProperty('--i', i));
  const html = DETAIL_KEYS.map(([k, label]) => {
    const v = kv[k];
    if (v === undefined || v === '') return '';
    return `<div class="k">${escapeHtml(label)}</div><div class="v">${escapeHtml(v)}</div>`;
  }).join('');
  el.className = 'kv in';
  el.innerHTML = html || '<div class="empty">identity.prop kosong.</div>';
}

document.getElementById('refreshBtn').addEventListener('click', loadPersona);
document.getElementById('freshenBtn').addEventListener('click', (ev) => withLoading(ev.currentTarget, async () => {
  // Jalankan action.sh: undi 1 device acak multi-brand -> apply-boot -> reset
  // app target -> rotasi ID (SSAID/GAID/MAC/nama/boot-count). Persis tombol
  // Action fisik di KSU/APatch, jadi hasil dari web = hasil dari tombol.
  const cmd = `${ENV} && sh ${shq(MODDIR)}/action.sh 2>&1`;
  const r = await run(cmd);
  if (!r.ok) toast(trimTitle(r.err.message || 'Undi device gagal'), { kind: 'error', detail: r.err.stdout || r.err.stderr || '' });
  else { const s = summarizeAction(r.out); toast(s.title, { kind: s.kind, detail: s.detail }); }
  loadPersona();
}));

const ROT_CARDS = [
  { key: 'ssaid',       name: 'SSAID',         desc: 'Android ID per-app (Settings.Secure) — wipe butuh reboot buat regen', get: 'ANDROID_ID' },
  { key: 'gaid',        name: 'Google AID',    desc: 'Advertising ID (Settings.Global + XML GMS)',        get: 'GOOGLE_AID' },
  { key: 'wlan-mac',    name: 'WiFi MAC',      desc: 'MAC wlan0 + reset WifiConfigStore',                 get: 'WIFI_MAC' },
  { key: 'bt-mac',      name: 'Bluetooth MAC', desc: 'MAC adapter BT + Address di bt_config.conf',        get: 'BLUETOOTH_ADDR' },
  { key: 'device-name', name: 'Nama device',   desc: 'device_name = MODEL dari identity.prop',            get: 'MODEL' },
  { key: 'boot-count',  name: 'Boot count',    desc: 'Settings.Global.boot_count = BOOT_COUNT identity.prop', get: 'BOOT_COUNT' },
];

async function loadRotate() {
  const wrap = document.getElementById('rotCards');
  wrap.innerHTML = ROT_CARDS.map((c, i) => `
    <div class="card" data-key="${c.key}">
      <div class="name">${c.name}</div>
      <div class="desc">${c.desc}</div>
      <div class="val sk sk-line" data-slot="val"></div>
      <div class="actions"><button class="sm" data-rot="${c.key}">Rotasi</button></div>
    </div>`).join('');
  // M9/CSP: index stagger di-set lewat CSSOM (.style.setProperty), bukan atribut
  // inline style="--i:.." — atribut inline diblok oleh style-src 'self' tanpa
  // 'unsafe-inline'. CSSOM write tidak tunduk CSP, jadi animasi tetap jalan.
  wrap.querySelectorAll('.card').forEach((el, i) => el.style.setProperty('--i', i));
  wrap.querySelectorAll('button[data-rot]').forEach(b => {
    b.addEventListener('click', () => rotateOne(b.dataset.rot, b));
  });
  const r = await run(`cat ${shq(IDENTITY)} 2>/dev/null || true`);
  const kv = r.ok ? parseProp(r.out) : {};
  for (const c of ROT_CARDS) {
    const slot = wrap.querySelector(`.card[data-key="${c.key}"] [data-slot="val"]`);
    if (!slot) continue;
    slot.classList.remove('sk', 'sk-line');
    slot.textContent = (c.get && kv[c.get]) ? kv[c.get] : '\u2014';
  }
}

function rotateCmd(key) {
  return `${ENV} && mkdir -p ${shq(MODDIR)}/debug && ` +
    `{ printf '[%s] ==> rotate ${key} (webui)\\n' "$(date '+%F %T')"; ` +
    `sh ${shq(ROTATE_SH)} ${shq(key)} 2>&1; } | tee -a ${shq(ROTATE_LOG)}`;
}

function finishRotate(r, label) {
  if (!r.ok) toast(trimTitle(r.err.message || 'Rotate gagal'), { kind: 'error', detail: r.err.stdout || r.err.stderr || '' });
  else { const s = summarizeRotate(r.out, label); toast(s.title, { kind: s.kind, detail: s.detail }); }
  loadRotate();
}

async function rotateOne(key, btn) {
  await withLoading(btn, async () => {
    const r = await run(rotateCmd(key));
    const label = (ROT_CARDS.find(c => c.key === key) || {}).name || key;
    finishRotate(r, label);
  });
}

document.getElementById('rotAll').addEventListener('click', (ev) => withLoading(ev.currentTarget, async () => {
  const r = await run(rotateCmd('all'));
  finishRotate(r, 'Rotate all');
}));

async function loadTargets() {
  const ta = document.getElementById('tgtArea');
  const r = await safeExec(`cat ${shq(TARGETS)} 2>/dev/null || true`);
  ta.value = r.ok ? r.out : '';
  document.getElementById('tgtStatus').textContent = '';
}
document.getElementById('tgtReload').addEventListener('click', loadTargets);
document.getElementById('tgtSave').addEventListener('click', (ev) => withLoading(ev.currentTarget, async () => {
  const ta = document.getElementById('tgtArea');
  const content = ta.value.replace(/\r\n/g, '\n');
  const b64 = btoa(unescape(encodeURIComponent(content)));
  const cmd = `echo ${shq(b64)} | base64 -d > ${shq(TARGETS)} && chmod 0644 ${shq(TARGETS)}`;
  const r = await safeExec(cmd, 'target.txt tersimpan');
  if (r.ok) {
    const lines = content.split('\n').filter(l => l.trim() && !l.trim().startsWith('#')).length;
    document.getElementById('tgtStatus').textContent = `${lines} paket \u00b7 hot-reload saat spawn berikutnya`;
  }
}));

async function loadLog() {
  const src = document.getElementById('logSrc').value;
  const body = document.getElementById('logBody');
  body.innerHTML = skLines(10);
  let cmd;
  if (src === 'action')  cmd = `tail -n 400 ${shq(ACTION_LOG)} 2>/dev/null || echo '(belum ada action.log \u2014 tap Freshen atau tombol Action KSU/APatch)'`;
  else if (src === 'rotate') cmd = `tail -n 400 ${shq(ROTATE_LOG)} 2>/dev/null || echo '(belum ada rotate.log \u2014 tap tombol Rotate)'`;
  else if (src === 'session') cmd = `ls -t ${shq(MODDIR)}/debug/session-*.log 2>/dev/null | head -n 1 | xargs -r tail -n 400 || echo '(tidak ada session log \u2014 flash varian debug)'`;
  else if (src === 'crashes') cmd = `tail -n 400 ${shq(MODDIR)}/debug/crashes.log 2>/dev/null || echo '(belum ada crashes.log)'`;
  else if (src === 'logcat') cmd = `logcat -d -t 200 -v time -s SandboxID:V SandboxIDCompanion:V 2>&1 | tail -n 200`;
  const r = await safeExec(cmd);
  const text = (r.ok ? r.out : (r.err && r.err.message) || 'error') || '(kosong)';
  body.innerHTML = renderLogHtml(text);
  body.scrollTop = body.scrollHeight;
}
document.getElementById('logRefresh').addEventListener('click', loadLog);
document.getElementById('logSrc').addEventListener('change', loadLog);

function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, c => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
  }[c]));
}

(function boot() {
  toastInit();
  wireTabs();
  moveIndicator();
  const nav = document.getElementById('nav');
  nav.addEventListener('scroll', moveIndicator);
  window.addEventListener('resize', moveIndicator);
  window.addEventListener('load', moveIndicator);
  // live dot: hijau berdenyut kalau root bridge kebaca, merah kalau nggak.
  const bridge = (typeof ksu !== 'undefined' && !!ksu.exec);
  const live = document.getElementById('live');
  if (live) {
    live.classList.add(bridge ? 'on' : 'off');
    live.title = bridge ? 'root bridge aktif' : 'root bridge tak tersedia';
  }
  (async () => {
    const v = await run(`sed -n 's/^version=//p' ${shq(MODDIR)}/module.prop 2>/dev/null | head -n 1`);
    if (v.ok && v.out.trim()) document.getElementById('version').textContent = v.out.trim();
    await run(`mkdir -p ${shq(MODDIR)}/debug && touch ${shq(ROTATE_LOG)} ${shq(ACTION_LOG)}`);
    loadPersona();
  })();
})();
