'use strict';

const MODDIR = '/data/adb/modules/ternak_tt';
const BIN = `${MODDIR}/bin/ternak-tt`;
const ROTATE_SH = `${MODDIR}/rotate_ids.sh`;
const IDENTITY = `${MODDIR}/identity.prop`;
const TARGETS = `${MODDIR}/target.txt`;

const ROTATE_LOG = `${MODDIR}/debug/rotate.log`;
const ACTION_LOG = `${MODDIR}/debug/action.log`;

function shq(s) { return "'" + String(s).replace(/'/g, "'\\''") + "'"; }

// Env prefix: cd into MODDIR and put bin/ on PATH so the native binary
// can find its siblings (resetprop-rs, ternak-tt-arm64, etc.)
const ENV = `cd ${shq(MODDIR)} && export MODDIR=${shq(MODDIR)} && export PATH=${shq(MODDIR + '/bin')}:\"$PATH\"`;

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
      const out = String(stdout || '');
      const err = String(stderr || '');
      if (code === 0) {
        resolve(out);
      } else {
        // On error: prefer whichever stream has content. When callers
        // redirect with 2>&1, stderr is empty and error text is in stdout.
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

function toast(msg, kind, sticky) {
  const el = document.getElementById('toast');
  const text = String(msg || '').trim() || '(empty)';
  // Truncate very long errors in the pill but keep the newlines
  const lines = text.split('\n').filter(Boolean);
  const head = lines.slice(0, 3).join('\n');
  const more = lines.length > 3 ? `\n(+${lines.length - 3} more lines)` : '';
  el.textContent = head + more;
  el.className = 'toast show' + (kind ? ` ${kind}` : '');
  clearTimeout(toast._t);
  const ttl = sticky ? 8000 : (kind === 'error' ? 6000 : 2400);
  toast._t = setTimeout(() => { el.className = 'toast'; }, ttl);
}
document.getElementById('toast').addEventListener('click', () => {
  document.getElementById('toast').className = 'toast';
});

async function safeExec(cmd, okMsg) {
  try {
    const out = await shell(cmd);
    if (okMsg) toast(okMsg, 'ok');
    return { ok: true, out };
  } catch (e) {
    // Show full error content so user can see what went wrong
    toast(e.message || String(e), 'error', true);
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
}

document.getElementById('refreshBtn').addEventListener('click', loadPersona);
document.getElementById('freshenBtn').addEventListener('click', async () => {
  const btn = document.getElementById('freshenBtn');
  btn.disabled = true; const old = btn.textContent; btn.textContent = 'Freshening\u2026';
  // Auto-unlock -> freshen -> auto-lock so the module always ends in a
  // locked state. Each subsequent tap unlocks itself, no manual toggle.
  const cmd = `${ENV} && mkdir -p ${shq(MODDIR)}/debug && ` +
    `{ echo "--- $(date '+%F %T') freshen (webui) ---"; ` +
    `./bin/ternak-tt unlock >/dev/null 2>&1 || true; ` +
    `./bin/ternak-tt freshen 2>&1; RC=$?; ` +
    `./bin/ternak-tt lock >/dev/null 2>&1 || true; ` +
    `echo "[freshen exit $RC | auto-locked]"; exit $RC; } | ` +
    `tee -a ${shq(ACTION_LOG)}`;
  await safeExec(cmd, 'Persona freshened + locked');
  btn.textContent = old; btn.disabled = false;
  loadPersona();
});

/* ---------- Rotate ---------- */
const ROT_CARDS = [
  // SSAID surfaces as Settings.Secure.ANDROID_ID once system_server regens
  // after wipe. identity.prop ANDROID_ID is the persona value returned by
  // the L1/L2 Java hook, so show that as the current value.
  { key: 'ssaid',       name: 'SSAID',           desc: 'Per-app Settings.Secure.ANDROID_ID (wipe requires reboot to regen)', get: 'ANDROID_ID' },
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
  const cmd = `${ENV} && mkdir -p ${shq(MODDIR)}/debug && ` +
    `{ echo "--- $(date '+%F %T') rotate ${key} (webui) ---"; ` +
    `sh ${shq(ROTATE_SH)} ${shq(key)} 2>&1; echo "[exit $?]"; } | ` +
    `tee -a ${shq(ROTATE_LOG)}`;
  await safeExec(cmd, `${key} rotated`);
  btn.textContent = old; btn.disabled = false;
  loadRotate();
}

document.getElementById('rotAll').addEventListener('click', async (ev) => {
  const b = ev.currentTarget; const old = b.textContent;
  b.disabled = true; b.textContent = 'Running\u2026';
  const cmd = `${ENV} && mkdir -p ${shq(MODDIR)}/debug && ` +
    `{ echo "--- $(date '+%F %T') rotate all (webui) ---"; ` +
    `sh ${shq(ROTATE_SH)} all 2>&1; echo "[exit $?]"; } | ` +
    `tee -a ${shq(ROTATE_LOG)}`;
  await safeExec(cmd, 'Rotated all');
  b.textContent = old; b.disabled = false;
  loadRotate();
});
document.getElementById('rotSafe').addEventListener('click', async (ev) => {
  const b = ev.currentTarget; const old = b.textContent;
  b.disabled = true; b.textContent = 'Running\u2026';
  const cmd = `${ENV} && mkdir -p ${shq(MODDIR)}/debug && ` +
    `{ echo "--- $(date '+%F %T') rotate safe (webui) ---"; ` +
    `sh ${shq(ROTATE_SH)} safe 2>&1; echo "[exit $?]"; } | ` +
    `tee -a ${shq(ROTATE_LOG)}`;
  await safeExec(cmd, 'Safe rotate done');
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
  if (src === 'action')  cmd = `tail -n 400 ${shq(ACTION_LOG)} 2>/dev/null || echo '(no action.log yet \u2014 tap Freshen or the KSU/APatch Action button)'`;
  else if (src === 'rotate') cmd = `tail -n 400 ${shq(ROTATE_LOG)} 2>/dev/null || echo '(no rotate.log yet \u2014 tap a Rotate button)'`;
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

(async function boot() {
  try {
    const r = await safeExec(`sed -n 's/^version=//p' ${shq(MODDIR)}/module.prop 2>/dev/null | head -n 1`);
    if (r.ok && r.out.trim()) document.getElementById('version').textContent = r.out.trim();
  } catch (_) {}
  await safeExec(`mkdir -p ${shq(MODDIR)}/debug && touch ${shq(ROTATE_LOG)} ${shq(ACTION_LOG)}`);
  loadPersona();
})();
