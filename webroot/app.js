'use strict';

const MODDIR = '/data/adb/modules/ternak_tt';
const BIN = `${MODDIR}/bin/ternak-tt`;
const ROTATE_SH = `${MODDIR}/rotate_ids.sh`;
const IDENTITY = `${MODDIR}/identity.prop`;
const TARGETS = `${MODDIR}/target.txt`;

const ROTATE_LOG = `${MODDIR}/debug/rotate.log`;
const ACTION_LOG = `${MODDIR}/debug/action.log`;

function shq(s) { return "'" + String(s).replace(/'/g, "'\\''") + "'"; }

const ENV = `cd ${shq(MODDIR)} && export MODDIR=${shq(MODDIR)} && export PATH=${shq(MODDIR + '/bin')}:\"$PATH\"`;

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

function summarizeFreshen(out) {
  const text = String(out || '');
  if (/^OK - fresh/m.test(text)) {
    const m = text.match(/^\s*MODEL\s*:\s*(.+)$/m);
    const model = m ? m[1].trim() : '';
    return { kind: 'ok', title: model ? `Persona baru \u00b7 ${model}` : 'Persona baru siap', detail: text };
  }
  const bang = (text.match(/^!.*$/m) || [])[0];
  return { kind: 'error', title: trimTitle(bang || text || 'Freshen gagal'), detail: text };
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

const PERSONA_KEYS = [
  'MODEL', 'BRAND', 'MANUFACTURER', 'DEVICE', 'PRODUCT',
  'FINGERPRINT', 'SERIAL', 'RADIO',
  'ANDROID_ID', 'GOOGLE_AID',
  'WIFI_MAC', 'BLUETOOTH_ADDR', 'BLUETOOTH_NAME',
];

async function loadPersona() {
  const el = document.getElementById('identity');
  el.className = 'kv';
  el.innerHTML = skKv(7);
  const r = await safeExec(`cat ${shq(IDENTITY)} 2>/dev/null || true`);
  if (!r.ok || !r.out.trim()) {
    el.innerHTML = '<div class="empty">Belum ada identity.prop. Tap Freshen persona.</div>';
    return;
  }
  const kv = parseProp(r.out);
  const html = PERSONA_KEYS.map(k => {
    const v = kv[k];
    if (v === undefined || v === '') return '';
    return `<div class="k">${k}</div><div class="v">${escapeHtml(v)}</div>`;
  }).join('');
  el.className = 'kv in';
  el.innerHTML = html || '<div class="empty">identity.prop kosong.</div>';
}

document.getElementById('refreshBtn').addEventListener('click', loadPersona);
document.getElementById('freshenBtn').addEventListener('click', (ev) => withLoading(ev.currentTarget, async () => {
  const cmd = `${ENV} && mkdir -p ${shq(MODDIR)}/debug && ` +
    `{ printf '[%s] ==> freshen (webui)\\n' "$(date '+%F %T')"; ` +
    `./bin/ternak-tt unlock >/dev/null 2>&1 || true; ` +
    `./bin/ternak-tt freshen 2>&1; RC=$?; ` +
    `./bin/ternak-tt lock >/dev/null 2>&1 || true; ` +
    `if [ $RC -eq 0 ]; then printf '[%s] [OK] freshen selesai \u00b7 locked\\n' "$(date '+%F %T')"; ` +
    `else printf '[%s] [ERR] freshen exit %s\\n' "$(date '+%F %T')" "$RC"; fi; } | ` +
    `tee -a ${shq(ACTION_LOG)}`;
  const r = await run(cmd);
  if (!r.ok) toast(trimTitle(r.err.message || 'Freshen gagal'), { kind: 'error', detail: r.err.stdout || r.err.stderr || '' });
  else { const s = summarizeFreshen(r.out); toast(s.title, { kind: s.kind, detail: s.detail }); }
  loadPersona();
}));

const ROT_CARDS = [
  { key: 'ssaid',       name: 'SSAID',         desc: 'Per-app Settings.Secure.ANDROID_ID (wipe butuh reboot untuk regen)', get: 'ANDROID_ID' },
  { key: 'gaid',        name: 'Google AID',    desc: 'Advertising ID (Settings.Global + GMS xml)',          get: 'GOOGLE_AID' },
  { key: 'wlan-mac',    name: 'wlan MAC',      desc: 'wlan0 MAC + WifiConfigStore reset',                   get: 'WIFI_MAC' },
  { key: 'bt-mac',      name: 'Bluetooth MAC', desc: 'BT adapter MAC + bt_config.conf Address',             get: 'BLUETOOTH_ADDR' },
  { key: 'device-name', name: 'Device name',   desc: 'settings global device_name = identity.prop MODEL',   get: 'MODEL' },
];

async function loadRotate() {
  const wrap = document.getElementById('rotCards');
  wrap.innerHTML = ROT_CARDS.map((c, i) => `
    <div class="card" data-key="${c.key}" style="--i:${i}">
      <div class="name">${c.name}</div>
      <div class="desc">${c.desc}</div>
      <div class="val sk sk-line" data-slot="val"></div>
      <div class="actions"><button class="sm" data-rot="${c.key}">Rotate</button></div>
    </div>`).join('');
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
  else if (src === 'logcat') cmd = `logcat -d -t 200 -v time -s TernakTT:V TernakTTCompanion:V 2>&1 | tail -n 200`;
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
  (async () => {
    const v = await run(`sed -n 's/^version=//p' ${shq(MODDIR)}/module.prop 2>/dev/null | head -n 1`);
    if (v.ok && v.out.trim()) document.getElementById('version').textContent = v.out.trim();
    await run(`mkdir -p ${shq(MODDIR)}/debug && touch ${shq(ROTATE_LOG)} ${shq(ACTION_LOG)}`);
    loadPersona();
  })();
})();
