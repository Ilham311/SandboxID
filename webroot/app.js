'use strict';

const MODDIR = '/data/adb/modules/sandboxid';
const ROTATE_SH = `${MODDIR}/rotate_ids.sh`;
const IDENTITY = `${MODDIR}/identity.prop`;
const TARGETS = `${MODDIR}/target.txt`;

const ROTATE_LOG = `${MODDIR}/debug/rotate.log`;
const ACTION_LOG = `${MODDIR}/debug/action.log`;

function shq(s) { return "'" + String(s).replace(/'/g, "'\\''") + "'"; }

const ENV = `cd ${shq(MODDIR)} && export MODDIR=${shq(MODDIR)} && export PATH=${shq(MODDIR + '/bin')}:\"$PATH\"`;

const BRAND_DOT = {
  google: '#4285f4', samsung: '#2e6be6', xiaomi: '#ff6900', redmi: '#ff453a',
  poco: '#ffcc00', oppo: '#10b981', vivo: '#3aa0ff', infinix: '#00c2a8',
};
function setBrand(brand) {
  const hex = BRAND_DOT[String(brand || '').trim().toLowerCase()] || '';
  const root = document.documentElement.style;
  if (hex) root.setProperty('--brand', hex);
  else root.removeProperty('--brand');
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
  T.detail.innerHTML = hasDetail ? renderLogHtml(detail).html : '';
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
  if (/^==>/.test(rest) || /^===/.test(rest)) lvl = 'step';
  else if (/^\[OK\]/.test(rest) || /^OK\b/.test(rest) || /\bSELESAI\b/i.test(rest)) lvl = 'ok';
  else if (/^\[WARN\]/.test(rest) || /\bREBOOT REQUIRED\b/i.test(rest)) lvl = 'warn';
  else if (/^\[ERR\]/.test(rest) || /^!/.test(rest) || /^\s*(Gagal|FAIL(ED)?)\b/i.test(rest)) lvl = 'err';
  else {
    // logcat "MM-DD HH:MM:SS.mmm  PID  TID L Tag: msg" (L = V/D/I/W/E/F)
    const lc = rest.match(/^\d\d-\d\d \d\d:\d\d:\d\d\.\d+\s+(?:\d+\s+\d+\s+)?([VDIWEF])[\/\s]/);
    const p = lc ? lc[1] : '';
    if (p === 'E' || p === 'F') lvl = 'err';
    else if (p === 'W') lvl = 'warn';
    else if (p === 'I') lvl = 'info';
    else if (p === 'V' || p === 'D') lvl = 'muted';
    else if (/\b(error|exception|denied|cannot|not found|no such|refused|fatal|crash|segfault|abort)\b/i.test(rest)) lvl = 'err';
    else if (/\b(warn(ing)?|skip(ped)?|dilewati|belum|missing)\b/i.test(rest)) lvl = 'warn';
  }
  return { ts, rest, lvl };
}

// Level-rank untuk penyaringan: err menyaring err saja; warn menyaring warn+err; ok = ok saja.
const LOG_LVL_MATCH = {
  err:  l => l === 'err',
  warn: l => l === 'warn',
  ok:   l => l === 'ok',
};

function renderLogHtml(text, filter) {
  filter = filter || {};
  const q = (filter.q || '').toLowerCase();
  const lvl = filter.lvl && filter.lvl !== 'all' ? filter.lvl : '';
  const counts = { err: 0, warn: 0, ok: 0, step: 0, info: 0, muted: 0 };
  const lines = String(text).replace(/\r/g, '').split('\n');
  let shown = 0;
  const html = lines.map(line => {
    if (line === '') {
      if (q || lvl) return '';
      return '<div class="ln">&nbsp;</div>';
    }
    const c = classifyLine(line);
    if (counts[c.lvl] !== undefined) counts[c.lvl]++;
    if (lvl && !(LOG_LVL_MATCH[lvl] && LOG_LVL_MATCH[lvl](c.lvl))) return '';
    if (q && line.toLowerCase().indexOf(q) === -1) return '';
    shown++;
    const ts = c.ts ? `<span class="ts">${escapeHtml(c.ts)}</span> ` : '';
    const badge = (c.lvl === 'err' || c.lvl === 'warn' || c.lvl === 'ok')
      ? `<span class="ln-tag ln-tag-${c.lvl}">${c.lvl.toUpperCase()}</span>` : '';
    return `<div class="ln lvl-${c.lvl}">${badge}${ts}${escapeHtml(c.rest)}</div>`;
  }).join('');
  return { html, counts, shown, total: lines.filter(l => l !== '').length };
}

function summarizeAction(out) {
  const text = String(out || '');
  if (/^OK - persona baru aktif/m.test(text)) {
    const b = (text.match(/^\s*BRAND\s*:\s*(.+)$/m) || [])[1];
    const md = (text.match(/^\s*MODEL\s*:\s*(.+)$/m) || [])[1];
    const label = [b && b.trim(), md && md.trim()].filter(Boolean).join(' \u00b7 ');
    return { kind: 'ok', title: label ? `Perangkat baru \u00b7 ${label}` : 'Perangkat baru aktif', detail: text };
  }
  if (/^OK - fresh/m.test(text)) {
    const md = (text.match(/^\s*MODEL\s*:\s*(.+)$/m) || [])[1];
    return { kind: 'ok', title: md ? `Perangkat baru \u00b7 ${md.trim()}` : 'Perangkat baru siap', detail: text };
  }
  const bang = (text.match(/^(?:Gagal\b|[\u2717!]).*$/m) || [])[0];
  return { kind: 'error', title: trimTitle(bang || text || 'Gagal mengacak perangkat'), detail: text };
}

function summarizeRotate(out, label) {
  const text = String(out || '');
  const errs = (text.match(/\[ERR\]/g) || []).length;
  const warns = (text.match(/\[WARN\]/g) || []).length;
  const fail = text.match(/(\d+) step\(s\) reported failure/);
  const reboot = /REBOOT REQUIRED/i.test(text);
  const name = label || 'Rotasi';
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
  if (id !== 'log') stopLogAuto();
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
  const mkt = kv.MARKETNAME || kv.MODEL || '(tidak dikenal)';
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
    t.push(`<div class="tile boot"><div class="tlabel">Boot count</div><div class="tval">${escapeHtml(kv.BOOT_COUNT)}</div><div class="tsub">jumlah reboot</div></div>`);
  const up = kv.UPTIME_HUMAN || (kv.UPTIME_SECONDS ? kv.UPTIME_SECONDS + 's' : '');
  if (up)
    t.push(`<div class="tile up"><div class="tlabel">Uptime</div><div class="tval">${escapeHtml(up)}</div><div class="tsub">lama menyala</div></div>`);
  if (kv.FRESH) {
    const yes = /^(y|yes|true|1)$/i.test(kv.FRESH.trim());
    t.push(`<div class="tile fresh"><div class="tlabel">Fresh</div><div class="tval ${yes ? 'yes' : 'no'}">${yes ? 'Ya' : 'Tidak'}</div><div class="tsub">baru direset?</div></div>`);
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
    setBrand('');
    hero.innerHTML = '<div class="empty">Belum ada perangkat. Tekan "Acak perangkat baru" untuk mulai.</div>';
    el.className = 'kv';
    el.innerHTML = '<div class="empty">identity.prop belum ada.</div>';
    return;
  }
  const kv = parseProp(r.out);
  setBrand(kv.BRAND);
  hero.innerHTML = renderHero(kv);
  tiles.innerHTML = renderTiles(kv);
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
  const cmd = `${ENV} && sh ${shq(MODDIR)}/action.sh 2>&1`;
  const r = await run(cmd);
  if (!r.ok) toast(trimTitle(r.err.message || 'Gagal mengacak perangkat'), { kind: 'error', detail: r.err.stdout || r.err.stderr || '' });
  else { const s = summarizeAction(r.out); toast(s.title, { kind: s.kind, detail: s.detail }); }
  loadPersona();
  if (document.getElementById('rotate').classList.contains('active')) loadRotate();
}));

let rebootArmed = false;
let rebootArmTimer = null;
document.getElementById('rebootBtn').addEventListener('click', (ev) => {
  const btn = ev.currentTarget;
  if (!rebootArmed) {
    // Klik pertama: arm (butuh klik kedua untuk konfirmasi — cegah reboot tak sengaja).
    rebootArmed = true;
    btn.classList.add('armed');
    btn.textContent = 'Ketuk lagi untuk reboot';
    toast('Ketuk sekali lagi untuk reboot sekarang', { kind: 'warn' });
    clearTimeout(rebootArmTimer);
    rebootArmTimer = setTimeout(() => {
      rebootArmed = false;
      btn.classList.remove('armed');
      btn.textContent = 'Reboot';
    }, 4000);
    return;
  }
  clearTimeout(rebootArmTimer);
  rebootArmed = false;
  btn.classList.remove('armed');
  btn.textContent = 'Reboot';
  withLoading(btn, async () => {
    toast('Menjalankan reboot…', { kind: 'info' });
    // Fallback berlapis: svc power reboot → setprop → reboot(8). Salah satu pasti jalan di root.
    const cmd = 'svc power reboot 2>/dev/null || setprop sys.powerctl reboot 2>/dev/null || reboot 2>/dev/null';
    const r = await run(cmd);
    // Jika perintah sukses, sistem biasanya sudah turun sebelum callback tiba.
    if (!r.ok) toast(trimTitle(r.err.message || 'Gagal reboot'), { kind: 'error', detail: r.err.stdout || r.err.stderr || '' });
  });
});

const ROT_CARDS = [
  { key: 'ssaid',       name: 'SSAID',         desc: 'Android ID per-aplikasi (Settings.Secure) — dihapus, dibuat ulang setelah reboot', get: 'ANDROID_ID' },
  { key: 'gaid',        name: 'Google AID',    desc: 'Advertising ID (Settings.Global + XML GMS)',        get: 'GOOGLE_AID' },
  { key: 'wlan-mac',    name: 'WiFi MAC',      desc: 'MAC wlan0 + reset WifiConfigStore',                 get: 'WIFI_MAC' },
  { key: 'bt-mac',      name: 'Bluetooth MAC', desc: 'MAC adapter BT + Address di bt_config.conf',        get: 'BLUETOOTH_ADDR' },
  { key: 'device-name', name: 'Nama perangkat', desc: 'device_name = MODEL dari identity.prop',           get: 'MODEL' },
  { key: 'boot-count',  name: 'Boot count',    desc: 'Settings.Global.boot_count = BOOT_COUNT identity.prop', get: 'BOOT_COUNT' },
  { key: 'applog',      name: 'AppLog ByteDance', desc: 'did/iid/ssid/openudid/clientudid/cdid untuk TikTok/Douyin — di-spoof in-process oleh hook JNI (L9)', get: null, applog: true },
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
    if (c.applog) {
      slot.textContent = '\u2026';
    } else {
      slot.textContent = (c.get && kv[c.get]) ? kv[c.get] : '\u2014';
    }
  }
  renderApplogStatus(wrap);
}

async function renderApplogStatus(wrap) {
  const slot = wrap.querySelector('.card[data-key="applog"] [data-slot="val"]');
  if (!slot) return;
  const cmd = `${ENV} && . ${shq(MODDIR + '/helpers.sh')} 2>/dev/null && ` +
    `if [ -r ${shq(TARGETS)} ]; then ` +
    `  while IFS= read -r _l || [ -n "$_l" ]; do ` +
    `    _l=$(printf '%s' "$_l" | sed -e "s/#.*//" -e "s/^[[:space:]]*//" -e "s/[[:space:]]*$//"); ` +
    `    [ -n "$_l" ] || continue; ` +
    `    applog_probe "$_l"; ` +
    `  done < ${shq(TARGETS)}; ` +
    `fi`;
  const r = await run(cmd);
  if (!r.ok) { slot.textContent = '\u2014'; return; }
  const lines = String(r.out || '').split('\n').map(x => x.trim()).filter(Boolean);
  if (lines.length === 0) {
    slot.textContent = 'target.txt kosong';
    return;
  }
  const parts = lines.map(line => {
    const [pkg, count, state] = line.split(/\s+/);
    const short = String(pkg || '').split('.').slice(-1)[0] || pkg;
    const label = { active: 'aktif', fresh: 'bersih', absent: 'nihil' }[state] || state;
    return `${short}: ${label} (${count})`;
  });
  slot.textContent = parts.join(' \u00b7 ');
  slot.title = lines.join('\n');
}

function rotateCmd(key) {
  return `${ENV} && mkdir -p ${shq(MODDIR)}/debug && ` +
    `{ printf '[%s] ==> rotate ${key} (webui)\\n' "$(date '+%F %T')"; ` +
    `sh ${shq(ROTATE_SH)} ${shq(key)} 2>&1; } | tee -a ${shq(ROTATE_LOG)}`;
}

function finishRotate(r, label) {
  if (!r.ok) toast(trimTitle(r.err.message || 'Rotasi gagal'), { kind: 'error', detail: r.err.stdout || r.err.stderr || '' });
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
  finishRotate(r, 'Rotasi semua');
}));

// ---- Target: pemilih aplikasi (saklar on/off + cari) --------------------
let TGT_APPS = [];          // [{pkg, label, user}]
let TGT_SELECTED = new Set(); // paket yang aktif (dari target.txt)
let TGT_HEADER = '';        // baris komentar di atas target.txt, dipertahankan
let TGT_FILTER = 'user';    // user | all | on
let TGT_LOADED = false;

// Ubah nama paket \u2192 label yang enak dibaca: ambil segmen bermakna terakhir,
// buang TLD umum & suffix (.android/.app), Title-Case-kan. Contoh:
// com.zhiliaoapp.musically \u2192 "Musically"; com.google.android.youtube \u2192 "Youtube".
function labelFromPkg(pkg) {
  const parts = String(pkg).split('.').filter(Boolean);
  const drop = new Set(['com', 'org', 'net', 'io', 'app', 'apps', 'android', 'mobile', 'co']);
  let seg = '';
  for (let i = parts.length - 1; i >= 0; i--) {
    if (!drop.has(parts[i].toLowerCase())) { seg = parts[i]; break; }
  }
  if (!seg) seg = parts[parts.length - 1] || pkg;
  seg = seg.replace(/[_-]+/g, ' ').replace(/([a-z])([A-Z])/g, '$1 $2');
  return seg.replace(/\b\w/g, c => c.toUpperCase());
}

// Baca daftar paket. -3 = user apps saja; tanpa -3 = semua. Universal di semua Android.
async function fetchApps(all) {
  const flag = all ? '' : '-3';
  const r = await run(`pm list packages ${flag} 2>/dev/null | sed 's/^package://' | sort -u`);
  if (!r.ok) return [];
  const userSet = new Set();
  if (all) {
    const ru = await run(`pm list packages -3 2>/dev/null | sed 's/^package://'`);
    if (ru.ok) ru.out.split(/\r?\n/).forEach(p => { p = p.trim(); if (p) userSet.add(p); });
  }
  return String(r.out).split(/\r?\n/).map(l => l.trim()).filter(Boolean).map(pkg => ({
    pkg,
    label: labelFromPkg(pkg),
    user: all ? userSet.has(pkg) : true,
  }));
}

function parseTargetsFile(text) {
  const sel = new Set();
  const header = [];
  let sawPkg = false;
  for (const raw of String(text).split(/\r?\n/)) {
    const line = raw.replace(/#.*$/, '').trim();
    if (!line) { if (!sawPkg && raw.trim().startsWith('#')) header.push(raw); continue; }
    sel.add(line);
    sawPkg = true;
  }
  return { sel, header: header.join('\n') };
}

function tgtVisibleApps() {
  const q = (document.getElementById('tgtSearch').value || '').trim().toLowerCase();
  let list = TGT_APPS.slice();
  if (TGT_FILTER === 'user') list = list.filter(a => a.user);
  else if (TGT_FILTER === 'on') list = list.filter(a => TGT_SELECTED.has(a.pkg));
  if (q) list = list.filter(a => a.pkg.toLowerCase().indexOf(q) !== -1 || a.label.toLowerCase().indexOf(q) !== -1);
  // Yang aktif tampil di atas, lalu alfabet by label.
  list.sort((a, b) => {
    const sa = TGT_SELECTED.has(a.pkg) ? 0 : 1, sb = TGT_SELECTED.has(b.pkg) ? 0 : 1;
    if (sa !== sb) return sa - sb;
    return a.label.localeCompare(b.label);
  });
  return list;
}

function tgtUpdateCount() {
  const el = document.getElementById('tgtCount');
  if (el) el.textContent = `${TGT_SELECTED.size} dipilih`;
  const badge = document.getElementById('tgtBadge');
  if (badge) {
    const n = TGT_SELECTED.size;
    badge.textContent = n > 99 ? '99+' : String(n);
    badge.hidden = n === 0;
  }
}

// Baca cepat jumlah target aktif untuk badge nav (tanpa enumerasi aplikasi penuh).
async function prefetchTgtCount() {
  const r = await run(`cat ${shq(TARGETS)} 2>/dev/null || true`);
  if (!r.ok) return;
  const parsed = parseTargetsFile(r.out);
  if (!TGT_LOADED) { TGT_SELECTED = parsed.sel; TGT_HEADER = parsed.header; }
  tgtUpdateCount();
}

// Paket terpilih yang tak ada di daftar (mis. aplikasi sudah dihapus) tetap ditampilkan
// agar bisa dimatikan, dengan penanda.
function renderTgtList() {
  const wrap = document.getElementById('tgtList');
  if (!wrap) return;
  const list = tgtVisibleApps();
  const known = new Set(TGT_APPS.map(a => a.pkg));
  const ghosts = (TGT_FILTER !== 'on') ? [] :
    [...TGT_SELECTED].filter(p => !known.has(p)).map(p => ({ pkg: p, label: labelFromPkg(p), user: true, ghost: true }));
  const all = list.concat(ghosts);
  if (!all.length) {
    wrap.innerHTML = `<div class="empty">${TGT_LOADED ? 'Tidak ada aplikasi cocok.' : 'Memuat daftar aplikasi\u2026'}</div>`;
    return;
  }
  wrap.innerHTML = all.map(a => {
    const on = TGT_SELECTED.has(a.pkg);
    const initial = escapeHtml((a.label[0] || '?').toUpperCase());
    return `<label class="approw${on ? ' on' : ''}" data-pkg="${escapeHtml(a.pkg)}">
      <span class="app-ic" aria-hidden="true">${initial}</span>
      <span class="app-txt">
        <span class="app-name">${escapeHtml(a.label)}${a.ghost ? ' <span class="app-ghost">(tidak terpasang)</span>' : ''}</span>
        <span class="app-pkg">${escapeHtml(a.pkg)}</span>
      </span>
      <span class="switch"><input type="checkbox" ${on ? 'checked' : ''} aria-label="${escapeHtml(a.label)}"><span class="track"></span></span>
    </label>`;
  }).join('');
  wrap.querySelectorAll('.approw').forEach(row => {
    const cb = row.querySelector('input[type="checkbox"]');
    cb.addEventListener('change', () => {
      const pkg = row.dataset.pkg;
      if (cb.checked) TGT_SELECTED.add(pkg); else TGT_SELECTED.delete(pkg);
      row.classList.toggle('on', cb.checked);
      tgtUpdateCount();
      markTgtDirty();
    });
  });
}

let TGT_DIRTY = false;
function markTgtDirty() {
  TGT_DIRTY = true;
  const s = document.getElementById('tgtStatus');
  if (s) s.textContent = 'Perubahan belum disimpan \u2014 tekan "Simpan pilihan".';
}

function buildTargetsContent() {
  const head = TGT_HEADER ? TGT_HEADER.replace(/\s*$/, '') + '\n' : '';
  const body = [...TGT_SELECTED].sort().join('\n');
  return head + body + (body ? '\n' : '');
}

async function saveTargets(okMsg) {
  const content = buildTargetsContent();
  const b64 = btoa(unescape(encodeURIComponent(content)));
  const cmd = `echo ${shq(b64)} | base64 -d > ${shq(TARGETS)} && chmod 0644 ${shq(TARGETS)}`;
  const r = await safeExec(cmd, okMsg);
  if (r.ok) {
    TGT_DIRTY = false;
    document.getElementById('tgtStatus').textContent =
      `${TGT_SELECTED.size} paket tersimpan \u00b7 dimuat ulang saat aplikasi target dibuka lagi`;
  }
  return r;
}

async function loadTargets(force) {
  const wrap = document.getElementById('tgtList');
  if (wrap && (!TGT_LOADED || force)) wrap.innerHTML = skTargets(6);
  // Baca target.txt & daftar aplikasi paralel.
  const [rt, apps] = await Promise.all([
    run(`cat ${shq(TARGETS)} 2>/dev/null || true`),
    fetchApps(TGT_FILTER === 'all'),
  ]);
  const parsed = parseTargetsFile(rt.ok ? rt.out : '');
  TGT_SELECTED = parsed.sel;
  TGT_HEADER = parsed.header;
  TGT_APPS = apps;
  TGT_LOADED = true;
  TGT_DIRTY = false;
  // Sinkronkan textarea manual (mode lanjutan).
  const ta = document.getElementById('tgtArea');
  if (ta) ta.value = rt.ok ? rt.out : '';
  tgtUpdateCount();
  renderTgtList();
  const s = document.getElementById('tgtStatus');
  if (s && !TGT_DIRTY) s.textContent = apps.length ? `${apps.length} aplikasi terbaca` : 'Daftar aplikasi kosong / pm tidak tersedia.';
}

function skTargets(n) {
  let s = '';
  for (let i = 0; i < n; i++) {
    s += '<div class="approw sk-row"><span class="app-ic sk sk-ic"></span>' +
      '<span class="app-txt"><span class="sk sk-line sk-name"></span>' +
      '<span class="sk sk-line sk-sub"></span></span>' +
      '<span class="sk sk-sw"></span></div>';
  }
  return s;
}

// Cari & saring (debounce ringan biar mulus di list besar).
let tgtSearchTimer = null;
document.getElementById('tgtSearch').addEventListener('input', () => {
  clearTimeout(tgtSearchTimer);
  tgtSearchTimer = setTimeout(renderTgtList, 120);
});
document.getElementById('tgtFilter').querySelectorAll('.seg-btn').forEach(btn => {
  btn.addEventListener('click', async () => {
    if (btn.classList.contains('active')) return;
    document.getElementById('tgtFilter').querySelectorAll('.seg-btn').forEach(b => b.classList.toggle('active', b === btn));
    const nf = btn.dataset.filter;
    const needReload = (nf === 'all') !== (TGT_FILTER === 'all');
    TGT_FILTER = nf;
    if (needReload) {
      // Beralih user\u2194all butuh baca ulang daftar (ambil superset atau subset).
      const keep = new Set(TGT_SELECTED);
      TGT_APPS = await fetchApps(TGT_FILTER === 'all');
      TGT_SELECTED = keep;
    }
    renderTgtList();
  });
});
document.getElementById('tgtRefresh').addEventListener('click', (ev) =>
  withLoading(ev.currentTarget, () => loadTargets(true)));
document.getElementById('tgtSave').addEventListener('click', (ev) =>
  withLoading(ev.currentTarget, () => saveTargets('Pilihan target tersimpan')));
document.getElementById('tgtApplyNow').addEventListener('click', (ev) => withLoading(ev.currentTarget, async () => {
  const r = await saveTargets(null);
  if (!r.ok) return;
  // "Terapkan sekarang": reset aplikasi target agar langsung baca identitas baru.
  toast('Menerapkan & mereset aplikasi target\u2026', { kind: 'info' });
  const rr = await run(`${ENV} && sh ${shq(ROTATE_SH)} applog 2>&1`);
  if (rr.ok) toast(`Diterapkan \u00b7 ${TGT_SELECTED.size} target`, { kind: 'ok', detail: rr.out });
  else toast(trimTitle(rr.err.message || 'Sebagian gagal'), { kind: 'warn', detail: rr.err.stdout || rr.err.stderr || '' });
}));

// Mode teks manual (lanjutan) \u2014 tetap tersedia untuk power user.
document.getElementById('tgtRawToggle').addEventListener('click', () => {
  const wrap = document.getElementById('tgtRawWrap');
  wrap.hidden = !wrap.hidden;
  if (!wrap.hidden) {
    document.getElementById('tgtArea').value = buildTargetsContent();
  }
});
document.getElementById('tgtRawSave').addEventListener('click', (ev) => withLoading(ev.currentTarget, async () => {
  const ta = document.getElementById('tgtArea');
  const content = ta.value.replace(/\r\n/g, '\n');
  const b64 = btoa(unescape(encodeURIComponent(content)));
  const cmd = `echo ${shq(b64)} | base64 -d > ${shq(TARGETS)} && chmod 0644 ${shq(TARGETS)}`;
  const r = await safeExec(cmd, 'target.txt tersimpan');
  if (r.ok) { await loadTargets(true); }
}));
document.getElementById('tgtReload').addEventListener('click', () => loadTargets(true));

let LOG_RAW = '';       // teks log mentah terakhir (untuk filter tanpa fetch ulang)
let LOG_LEVEL = 'all';  // all | err | warn | ok

function applyLogFilter() {
  const body = document.getElementById('logBody');
  // Ikuti ke bawah hanya jika sudah dekat dasar (biar auto-refresh tak menarik view saat baca).
  const nearBottom = body.scrollHeight - body.scrollTop - body.clientHeight < 40;
  const prevTop = body.scrollTop;
  const q = document.getElementById('logSearch').value || '';
  const res = renderLogHtml(LOG_RAW, { q, lvl: LOG_LEVEL });
  body.innerHTML = res.html || `<div class="empty">Tidak ada baris cocok${q || LOG_LEVEL !== 'all' ? ' \u2014 coba ubah filter/pencarian.' : '.'}</div>`;
  // Perbarui angka di tombol level & ringkasan.
  const setCount = (lvl, n) => {
    const b = document.querySelector(`#logLevels .seg-btn[data-lvl="${lvl}"]`);
    if (b) b.dataset.count = String(n);
  };
  setCount('err', res.counts.err);
  setCount('warn', res.counts.warn);
  setCount('ok', res.counts.ok);
  const sum = document.getElementById('logSummary');
  if (sum) {
    const parts = [];
    if (res.counts.err) parts.push(`<span class="chip-err">${res.counts.err} error</span>`);
    if (res.counts.warn) parts.push(`<span class="chip-warn">${res.counts.warn} warn</span>`);
    if (res.counts.ok) parts.push(`<span class="chip-ok">${res.counts.ok} ok</span>`);
    const filtered = (q || LOG_LEVEL !== 'all');
    const tail = filtered ? ` \u00b7 ${res.shown}/${res.total} baris` : ` \u00b7 ${res.total} baris`;
    sum.innerHTML = (parts.join(' ') || '<span class="chip-muted">tidak ada error/warning</span>') + tail;
  }
  if (nearBottom) body.scrollTop = body.scrollHeight;
  else body.scrollTop = prevTop;
}

async function loadLog(opts) {
  opts = (opts && typeof opts === 'object' && !opts.type) ? opts : {};
  const src = document.getElementById('logSrc').value;
  const body = document.getElementById('logBody');
  if (!opts.silent) body.innerHTML = skLines(10);
  let cmd;
  if (src === 'action')  cmd = `tail -n 400 ${shq(ACTION_LOG)} 2>/dev/null || echo '(belum ada action.log \u2014 tekan "Acak semua \u00b7 1 klik" atau tombol Action di KSU/APatch)'`;
  else if (src === 'rotate') cmd = `tail -n 400 ${shq(ROTATE_LOG)} 2>/dev/null || echo '(belum ada rotate.log \u2014 tekan tombol Rotasi)'`;
  else if (src === 'session') cmd = `ls -t ${shq(MODDIR)}/debug/session-*.log 2>/dev/null | head -n 1 | xargs -r tail -n 400 || echo '(tidak ada session log \u2014 pasang varian debug)'`;
  else if (src === 'crashes') cmd = `tail -n 400 ${shq(MODDIR)}/debug/crashes.log 2>/dev/null || echo '(belum ada crashes.log)'`;
  else if (src === 'logcat') cmd = `logcat -d -t 200 -v time -s SandboxID:V SandboxIDCompanion:V 2>&1 | tail -n 200`;
  const r = await safeExec(cmd);
  LOG_RAW = (r.ok ? r.out : (r.err && r.err.message) || 'error') || '(kosong)';
  applyLogFilter();
}
document.getElementById('logRefresh').addEventListener('click', (ev) => withLoading(ev.currentTarget, loadLog));
document.getElementById('logSrc').addEventListener('change', () => loadLog());

// Auto-refresh: tail berkala tanpa flicker (silent), berhenti saat pindah tab.
let LOG_AUTO = false;
let LOG_AUTO_TIMER = null;
function stopLogAuto() {
  LOG_AUTO = false;
  if (LOG_AUTO_TIMER) { clearInterval(LOG_AUTO_TIMER); LOG_AUTO_TIMER = null; }
  const b = document.getElementById('logAuto');
  if (b) { b.classList.remove('active'); b.setAttribute('aria-pressed', 'false'); }
}
function startLogAuto() {
  LOG_AUTO = true;
  const b = document.getElementById('logAuto');
  if (b) { b.classList.add('active'); b.setAttribute('aria-pressed', 'true'); }
  clearInterval(LOG_AUTO_TIMER);
  LOG_AUTO_TIMER = setInterval(() => loadLog({ silent: true }), 3000);
  loadLog();
}
document.getElementById('logAuto').addEventListener('click', () => {
  if (LOG_AUTO) stopLogAuto(); else startLogAuto();
});
let logSearchTimer = null;
document.getElementById('logSearch').addEventListener('input', () => {
  clearTimeout(logSearchTimer);
  logSearchTimer = setTimeout(applyLogFilter, 130);
});
document.getElementById('logLevels').querySelectorAll('.seg-btn').forEach(btn => {
  btn.addEventListener('click', () => {
    document.getElementById('logLevels').querySelectorAll('.seg-btn').forEach(b => b.classList.toggle('active', b === btn));
    LOG_LEVEL = btn.dataset.lvl;
    applyLogFilter();
  });
});
document.getElementById('logCopy').addEventListener('click', async (ev) => {
  const btn = ev.currentTarget;
  try {
    if (navigator.clipboard && navigator.clipboard.writeText) {
      await navigator.clipboard.writeText(LOG_RAW);
    } else {
      const ta = document.createElement('textarea');
      ta.value = LOG_RAW; ta.style.position = 'fixed'; ta.style.opacity = '0';
      document.body.appendChild(ta); ta.select();
      document.execCommand('copy'); document.body.removeChild(ta);
    }
    const old = btn.textContent; btn.textContent = 'Tersalin \u2713';
    setTimeout(() => { btn.textContent = old; }, 1400);
  } catch (e) {
    toast('Gagal menyalin log', { kind: 'warn' });
  }
});

function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, c => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
  }[c]));
}

function applyTheme(mode) {
  const root = document.documentElement;
  if (mode === 'light' || mode === 'dark') root.setAttribute('data-theme', mode);
  else root.removeAttribute('data-theme');
}
function currentTheme() {
  const attr = document.documentElement.getAttribute('data-theme');
  if (attr === 'light' || attr === 'dark') return attr;
  return (window.matchMedia && window.matchMedia('(prefers-color-scheme: light)').matches) ? 'light' : 'dark';
}
function initTheme() {
  const btn = document.getElementById('themeBtn');
  if (!btn) return;
  btn.addEventListener('click', () => {
    const next = currentTheme() === 'light' ? 'dark' : 'light';
    applyTheme(next);
    try { localStorage.setItem('sbx-theme', next); } catch (e) {}
  });
}

(function boot() {
  initTheme();
  toastInit();
  wireTabs();
  moveIndicator();
  const nav = document.getElementById('nav');
  nav.addEventListener('scroll', moveIndicator);
  window.addEventListener('resize', moveIndicator);
  window.addEventListener('load', moveIndicator);
  const bridge = (typeof ksu !== 'undefined' && !!ksu.exec);
  const live = document.getElementById('live');
  if (live) {
    live.classList.add(bridge ? 'on' : 'off');
    live.title = bridge ? 'root bridge aktif' : 'root bridge tidak tersedia';
  }
  (async () => {
    const v = await run(`sed -n 's/^version=//p' ${shq(MODDIR)}/module.prop 2>/dev/null | head -n 1`);
    if (v.ok && v.out.trim()) document.getElementById('version').textContent = v.out.trim();
    await run(`mkdir -p ${shq(MODDIR)}/debug && touch ${shq(ROTATE_LOG)} ${shq(ACTION_LOG)}`);
    loadPersona();
    prefetchTgtCount();
  })();
})();
