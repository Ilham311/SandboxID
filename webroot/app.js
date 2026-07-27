'use strict';

const MODDIR = '/data/adb/modules/ternak_tt';
const BIN = `${MODDIR}/bin/ternak-tt`;
const ROTATE_SH = `${MODDIR}/rotate_ids.sh`;
const IDENTITY = `${MODDIR}/identity.prop`;
const TARGETS = `${MODDIR}/target.txt`;
const LOCK_FILE = `${MODDIR}/.locked`;

const ROTATE_LOG = `${MODDIR}/debug/rotate.log`;

function shq(s) { return "'" + String(s).replace(/'/g, "'\\''") + "'"; }

function exec(cmd) {
  return new Promise((resolve, reject) => {
    if (typeof ksu === 'undefined' || !ksu.exec) {
      reject(new Error('root bridge not available'));
      return;
    }
    const cbName = `__ksucb_${Date.now()}_${Math.random().toString(36).slice(2, 8)}`;
    window[cbName] = function (errno, stdout, stderr) {
      try { delete window[cbName]; } catch (e) { window[cbName] = undefined; }
      const code = Number(errno);
      if (code === 0) resolve(String(stdout || ''));
      else reject(Object.assign(new Error(String(stderr || `exit ${code}`).trim()), { code, stdout, stderr }));
    };
    try {
      ksu.exec(cmd, '{}', cbName);
    } catch (e) {
      try { delete window[cbName]; } catch (_) {}
      reject(e);
    }
  });
}

async function shell(cmd) {
  return exec(cmd);
}

function toast(msg, kind) {
  const el = document.getElementById('toast');
  el.textContent = msg;
  el.className = 'toast show' + (kind ? ` ${kind}` : '');
  clearTimeout(toast._t);
  toast._t = setTimeout(() => { el.className = 'toast'; }, 2400);
}

async function safeExec(cmd, okMsg) {
  try {
    const out = await shell(cmd);
    if (okMsg) toast(okMsg, 'ok');
    return { ok: true, out };
  } catch (e) {
    toast(e.message || String(e), 'error');
    return { ok: false, err: e };
  }
}

/* ---------- Tabs ---------- */
document.querySelectorAll('.tab').forEach(btn => {
  btn.addEventListener('click', () => {
    document.querySelectorAll('.tab').forEach(b => b.classList.toggle('active', b === btn));
    const id = btn.dataset.tab;
    document.querySelectorAll('.page').forEach(p => p.classList.toggle('active', p.id === id));
    onTab(id);
  });
});

function onTab(id) {
  if (id === 'persona') loadPersona();
  else if (id === 'rotate') loadRotate();
  else if (id === 'targets') loadTargets();
  else if (id === 'log') loadLog();
}

/* ---------- Persona ---------- */
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

const PERSONA_KEYS = [
  'MODEL', 'BRAND', 'MANUFACTURER', 'DEVICE', 'PRODUCT',
  'FINGERPRINT', 'SERIAL', 'RADIO',
  'ANDROID_ID', 'GOOGLE_AID',
  'WIFI_MAC', 'BLUETOOTH_ADDR', 'BLUETOOTH_NAME',
];

async function loadPersona() {
  const el = document.getElementById('identity');
  el.innerHTML = '<div class="empty">Loading&hellip;</div>';
  const r = await safeExec(`cat ${shq(IDENTITY)} 2>/dev/null || true`);
  if (!r.ok || !r.out.trim()) {
    el.innerHTML = '<div class="empty">No identity.prop yet. Tap Freshen persona.</div>';
    return;
  }
  const kv = parseProp(r.out);
  const html = PERSONA_KEYS.map(k => {
    const v = kv[k];
    if (v === undefined || v === '') return '';
    return `<div class="k">${k}</div><div class="v">${escapeHtml(v)}</div>`;
  }).join('');
  el.innerHTML = html || '<div class="empty">identity.prop is empty.</div>';
  await loadLockState();
}

document.getElementById('refreshBtn').addEventListener('click', loadPersona);
document.getElementById('freshenBtn').addEventListener('click', async () => {
  const btn = document.getElementById('freshenBtn');
  btn.disabled = true; const old = btn.textContent; btn.textContent = 'Freshening&hellip;';
  await safeExec(`${shq(BIN)} freshen 2>&1`, 'New persona written');
  btn.textContent = old; btn.disabled = false;
  loadPersona();
});

/* ---------- Lock ---------- */
async function loadLockState() {
  const r = await safeExec(`[ -f ${shq(LOCK_FILE)} ] && echo locked || echo unlocked`);
  const b = document.getElementById('lockBtn');
  const state = (r.out || '').trim();
  b.dataset.state = state === 'locked' ? 'locked' : 'unlocked';
  b.textContent = b.dataset.state;
}
document.getElementById('lockBtn').addEventListener('click', async () => {
  const b = document.getElementById('lockBtn');
  const next = b.dataset.state === 'locked' ? 'unlock' : 'lock';
  await safeExec(`${shq(BIN)} ${next} 2>&1`, next === 'lock' ? 'Locked' : 'Unlocked');
  loadLockState();
});

/* ---------- Rotate ---------- */
const ROT_CARDS = [
  { key: 'ssaid',       name: 'SSAID',           desc: 'Per-user Settings.Secure.ANDROID_ID wipe (reboot required)', get: null },
  { key: 'gaid',        name: 'Google AID',      desc: 'Advertising ID (Settings.Global + GMS xml)',                get: 'GOOGLE_AID' },
  { key: 'wlan-mac',    name: 'wlan MAC',        desc: 'wlan0 MAC + WifiConfigStore reset',                          get: 'WIFI_MAC' },
  { key: 'bt-mac',      name: 'Bluetooth MAC',   desc: 'BT adapter MAC + bt_config.conf Address',                    get: 'BLUETOOTH_ADDR' },
  { key: 'device-name', name: 'Device name',     desc: 'settings global device_name = identity.prop MODEL',          get: 'MODEL' },
];

async function loadRotate() {
  const wrap = document.getElementById('rotCards');
  wrap.innerHTML = ROT_CARDS.map(c => `
    <div class="card" data-key="${c.key}">
      <div class="name">${c.name}</div>
      <div class="desc">${c.desc}</div>
      <div class="val" data-slot="val">&mdash;</div>
      <div class="actions"><button class="sm" data-rot="${c.key}">Rotate</button></div>
    </div>`).join('');
  wrap.querySelectorAll('button[data-rot]').forEach(b => {
    b.addEventListener('click', () => rotateOne(b.dataset.rot, b));
  });
  const r = await safeExec(`cat ${shq(IDENTITY)} 2>/dev/null || true`);
  const kv = r.ok ? parseProp(r.out) : {};
  for (const c of ROT_CARDS) {
    const slot = wrap.querySelector(`.card[data-key="${c.key}"] [data-slot="val"]`);
    if (!slot) continue;
    if (c.get && kv[c.get]) slot.textContent = kv[c.get];
    else slot.textContent = '\u2014';
  }
}

async function rotateOne(key, btn) {
  const old = btn.textContent;
  btn.disabled = true; btn.textContent = '\u2026';
  await safeExec(`sh ${shq(ROTATE_SH)} ${shq(key)} 2>&1 | tee -a ${shq(ROTATE_LOG)}`, `${key} rotated`);
  btn.textContent = old; btn.disabled = false;
  loadRotate();
}

document.getElementById('rotAll').addEventListener('click', async (ev) => {
  const b = ev.currentTarget; const old = b.textContent;
  b.disabled = true; b.textContent = 'Running&hellip;';
  await safeExec(`sh ${shq(ROTATE_SH)} all 2>&1 | tee -a ${shq(ROTATE_LOG)}`, 'Rotated all');
  b.textContent = old; b.disabled = false;
  loadRotate();
});
document.getElementById('rotSafe').addEventListener('click', async (ev) => {
  const b = ev.currentTarget; const old = b.textContent;
  b.disabled = true; b.textContent = 'Running&hellip;';
  await safeExec(`sh ${shq(ROTATE_SH)} safe 2>&1 | tee -a ${shq(ROTATE_LOG)}`, 'Safe rotate done');
  b.textContent = old; b.disabled = false;
  loadRotate();
});

/* ---------- Targets ---------- */
async function loadTargets() {
  const ta = document.getElementById('tgtArea');
  const r = await safeExec(`cat ${shq(TARGETS)} 2>/dev/null || true`);
  ta.value = r.ok ? r.out : '';
  document.getElementById('tgtStatus').textContent = '';
}
document.getElementById('tgtReload').addEventListener('click', loadTargets);
document.getElementById('tgtSave').addEventListener('click', async () => {
  const ta = document.getElementById('tgtArea');
  const content = ta.value.replace(/\r\n/g, '\n');
  const b64 = btoa(unescape(encodeURIComponent(content)));
  const cmd = `echo ${shq(b64)} | base64 -d > ${shq(TARGETS)} && chmod 0644 ${shq(TARGETS)}`;
  const r = await safeExec(cmd, 'target.txt saved');
  if (r.ok) {
    const lines = content.split('\n').filter(l => l.trim() && !l.trim().startsWith('#')).length;
    document.getElementById('tgtStatus').textContent = `${lines} package${lines === 1 ? '' : 's'} \u00b7 hot-reload on next spawn`;
  }
});

/* ---------- Log ---------- */
async function loadLog() {
  const src = document.getElementById('logSrc').value;
  const body = document.getElementById('logBody');
  body.textContent = 'Loading\u2026';
  let cmd;
  if (src === 'rotate') cmd = `tail -n 400 ${shq(ROTATE_LOG)} 2>/dev/null || echo '(no rotate log yet)'`;
  else if (src === 'session') cmd = `ls -t ${shq(MODDIR)}/debug/session-*.log 2>/dev/null | head -n 1 | xargs -r tail -n 400 || echo '(no session log \u2014 flash the debug variant)'`;
  else if (src === 'crashes') cmd = `tail -n 400 ${shq(MODDIR)}/debug/crashes.log 2>/dev/null || echo '(no crashes.log yet)'`;
  else if (src === 'logcat') cmd = `logcat -d -t 200 -v time -s TernakTT:V TernakTTCompanion:V 2>&1 | tail -n 200`;
  const r = await safeExec(cmd);
  body.textContent = (r.ok ? r.out : (r.err && r.err.message) || 'error') || '(empty)';
  body.scrollTop = body.scrollHeight;
}
document.getElementById('logRefresh').addEventListener('click', loadLog);
document.getElementById('logSrc').addEventListener('change', loadLog);

/* ---------- Helpers ---------- */
function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, c => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
  }[c]));
}

/* ---------- Boot ---------- */
(async function boot() {
  try {
    const r = await safeExec(`sed -n 's/^version=//p' ${shq(MODDIR)}/module.prop 2>/dev/null | head -n 1`);
    if (r.ok && r.out.trim()) document.getElementById('version').textContent = r.out.trim();
  } catch (_) {}
  await safeExec(`mkdir -p ${shq(MODDIR)}/debug && touch ${shq(ROTATE_LOG)}`);
  loadPersona();
})();
