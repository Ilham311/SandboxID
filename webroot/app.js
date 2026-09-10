'use strict';

const MODDIR = '/data/adb/modules/sandboxid';
const ROTATE_SH = `${MODDIR}/rotate_ids.sh`;
const IDENTITY = `${MODDIR}/identity.prop`;
const TARGETS = `${MODDIR}/target.txt`;
const SELFTEST_SH = `${MODDIR}/selftest.sh`;

const ROTATE_LOG = `${MODDIR}/debug/rotate.log`;
const ACTION_LOG = `${MODDIR}/debug/action.log`;
const ACTION_STATE = `${MODDIR}/debug/action.state`;
const ACTION_RESULT = `${MODDIR}/debug/action.result`;
const ACTION_LOCK = `${MODDIR}/.action.lock`;
const BRIDGE_TIMEOUT_MS = 120000;
const ACTION_POLL_MS = 2000;
const RUN_ID_RE = /^[0-9a-f]{32}$/;

function shq(s) { return "'" + String(s).replace(/'/g, "'\\''") + "'"; }

function moduleEnv(moddir) {
  return `cd ${shq(moddir)} && export MODDIR=${shq(moddir)} && ` +
    `export PATH=${shq(moddir + '/bin')}:\"$PATH\"`;
}

const ENV = moduleEnv(MODDIR);

function ownerProcessProbeShell(ownerPath) {
  const owner = shq(ownerPath);
  return `_sbx_owner_pid=$(sed -n 's/^pid=//p' ${owner} 2>/dev/null | sed -n '1p'); ` +
    `_sbx_owner_start=$(sed -n 's/^proc_start=//p' ${owner} 2>/dev/null | sed -n '1p'); ` +
    `_sbx_actual_start=''; case \"$_sbx_owner_pid\" in ''|*[!0-9]*) : ;; *) ` +
    `if [ -r \"/proc/$_sbx_owner_pid/stat\" ]; then ` +
    `_sbx_stat=$(cat \"/proc/$_sbx_owner_pid/stat\" 2>/dev/null || :); ` +
    `_sbx_rest=\${_sbx_stat##*) }; set -- $_sbx_rest; ` +
    `if [ \"$#\" -ge 20 ]; then shift 19; _sbx_actual_start=$1; fi; fi ;; esac`;
}

function actionMutationGuardShell(moddir) {
  return `${ownerProcessProbeShell(moddir + '/.action.lock/owner')}; ` +
    `if [ -n \"$_sbx_actual_start\" ] && ` +
    `[ \"$_sbx_actual_start\" = \"$_sbx_owner_start\" ]; then ` +
    `printf '%s\\n' 'Action lain masih berjalan' >&2; exit 75; fi`;
}

function guardedMutationCmd(command, moddir = MODDIR) {
  return `${moduleEnv(moddir)} && ${actionMutationGuardShell(moddir)}; ${command}`;
}

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

function cleanupBridgeCallback(name) {
  try { delete window[name]; } catch (_) { window[name] = undefined; }
}

function exec(cmd, timeoutMs) {
  return new Promise((resolve, reject) => {
    if (typeof ksu === 'undefined' || !ksu.exec) {
      reject(Object.assign(new Error('root bridge tidak tersedia'), { code: 'bridge-unavailable' }));
      return;
    }
    const cbName = `__ksucb_${Date.now()}_${Math.random().toString(36).slice(2, 8)}`;
    let settled = false;
    const timeout = setTimeout(() => {
      if (settled) return;
      settled = true;
      cleanupBridgeCallback(cbName);
      reject(Object.assign(new Error('hasil belum diketahui: root bridge melewati batas waktu'), {
        code: 'bridge-timeout', unknown: true,
      }));
    }, timeoutMs || BRIDGE_TIMEOUT_MS);
    window[cbName] = function (errno, stdout, stderr) {
      if (settled) return;
      settled = true;
      clearTimeout(timeout);
      cleanupBridgeCallback(cbName);
      const code = Number(errno);
      const out = String(stdout || '');
      const err = String(stderr || '');
      if (code === 0) resolve(out);
      else {
        const msg = (err.trim() || out.trim() || `exit ${code}`);
        reject(Object.assign(new Error(msg), { code, stdout: out, stderr: err }));
      }
    };
    try {
      ksu.exec(cmd, '{}', cbName);
    } catch (e) {
      if (settled) return;
      settled = true;
      clearTimeout(timeout);
      cleanupBridgeCallback(cbName);
      reject(e);
    }
  });
}

async function shell(cmd) { return exec(cmd); }

async function run(cmd) {
  try { return { ok: true, out: await shell(cmd) }; }
  catch (e) { return { ok: false, err: e }; }
}

let mutationTail = Promise.resolve();
let mutationActive = 0;

function setMutationBusy(busy) {
  if (typeof document === 'undefined') return;
  document.querySelectorAll('[data-mutation]').forEach(el => {
    if (busy) {
      el.dataset.mutationWasDisabled = el.disabled ? '1' : '0';
      el.disabled = true;
      el.setAttribute('aria-disabled', 'true');
    } else {
      el.disabled = el.dataset.mutationWasDisabled === '1';
      if (!el.disabled) el.removeAttribute('aria-disabled');
      delete el.dataset.mutationWasDisabled;
    }
  });
}

function mutate(fn) {
  const task = mutationTail.catch(() => {}).then(async () => {
    mutationActive += 1;
    setMutationBusy(true);
    try { return await fn(); }
    finally {
      mutationActive -= 1;
      if (!mutationActive) setMutationBusy(false);
    }
  });
  mutationTail = task;
  return task;
}

function parseKeyValues(text) {
  const out = {};
  for (const line of String(text || '').replace(/\r/g, '').split('\n')) {
    const at = line.indexOf('=');
    if (at > 0) out[line.slice(0, at)] = line.slice(at + 1);
  }
  return out;
}

function parseActionProtocol(text) {
  const records = [];
  for (const line of String(text || '').replace(/\r/g, '').split('\n')) {
    const fields = line.split('\t');
    if (fields[0] !== 'SBX_ACTION_V1' || fields.length < 2) continue;
    const record = { type: fields[1], fields: {}, raw: line };
    let begin = 2;
    if (record.type === 'STAGE' && fields.length >= 4) {
      record.stage = fields[2];
      record.outcome = fields[3];
      begin = 4;
    }
    for (let i = begin; i < fields.length; i += 1) {
      const at = fields[i].indexOf('=');
      if (at > 0) record.fields[fields[i].slice(0, at)] = fields[i].slice(at + 1);
    }
    records.push(record);
  }
  return records;
}

function splitActionSnapshot(text) {
  const raw = String(text || '');
  const stateAt = raw.indexOf('__STATE__\n');
  const resultAt = raw.indexOf('__RESULT__\n');
  const lockAt = raw.indexOf('__LOCK__\n');
  const aliveAt = raw.indexOf('__ALIVE__\n');
  if (stateAt < 0 || resultAt < 0 || lockAt < 0 || aliveAt < 0 ||
      !(stateAt < resultAt && resultAt < lockAt && lockAt < aliveAt)) {
    return { state: {}, records: [], owner: {} };
  }
  const state = parseKeyValues(raw.slice(stateAt + 10, resultAt));
  const records = parseActionProtocol(raw.slice(resultAt + 11, lockAt));
  const owner = parseKeyValues(raw.slice(lockAt + 9, aliveAt));
  owner.alive = raw.slice(aliveAt + 10).trim() === '1' ? '1' : '0';
  return { state, records, owner };
}

function actionRunFromRecords(records) {
  for (const record of records) {
    const runId = record.fields.run;
    if (RUN_ID_RE.test(runId || '')) return runId;
  }
  return '';
}

function actionResult(records, expectedRun) {
  return records.find(record => record.type === 'RESULT' &&
    RUN_ID_RE.test(record.fields.run || '') &&
    (!expectedRun || record.fields.run === expectedRun)) || null;
}

function isActionLive(state, owner, expectedRun) {
  return RUN_ID_RE.test(expectedRun || '') && state.run === expectedRun &&
    owner.run === expectedRun && owner.alive === '1' && state.status !== 'terminal';
}

function actionPresentation(result) {
  if (!result) return { kind: 'warn', title: 'Hasil belum diketahui', terminal: false };
  const f = result.fields;
  const status = f.status || 'degraded';
  if (status === 'success' && f.reboot === '1')
    return { kind: 'warn', title: 'Identitas baru aktif · perlu reboot', terminal: true };
  if (status === 'success')
    return { kind: f.warnings === '0' ? 'ok' : 'warn', title: 'Identitas dan privasi baru selesai', terminal: true };
  if (status === 'partial')
    return { kind: 'warn', title: 'Identitas baru aktif · pekerjaan lanjutan parsial', terminal: true };
  if (status === 'rolled-back')
    return { kind: 'warn', title: 'Gagal diterapkan · identitas lama dipulihkan', terminal: true };
  if (status === 'busy')
    return { kind: 'warn', title: 'Action lain masih berjalan', terminal: true };
  if (status === 'degraded')
    return { kind: 'error', title: 'Konsistensi hasil tidak dapat dibuktikan', terminal: true };
  return { kind: 'error', title: 'Action gagal sebelum identitas aktif', terminal: true };
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

function activateTab(btn, focus) {
  const id = btn.dataset.tab;
  document.querySelectorAll('.tab').forEach(tab => {
    const active = tab === btn;
    tab.classList.toggle('active', active);
    tab.setAttribute('aria-selected', active ? 'true' : 'false');
    tab.tabIndex = active ? 0 : -1;
  });
  document.querySelectorAll('.page').forEach(page => {
    const active = page.id === id;
    page.classList.toggle('active', active);
    page.hidden = !active;
  });
  if (focus) btn.focus();
  moveIndicator();
  onTab(id);
}

function wireTabs() {
  const tabs = Array.from(document.querySelectorAll('.tab'));
  tabs.forEach(btn => {
    btn.addEventListener('click', () => activateTab(btn, false));
    btn.addEventListener('keydown', event => {
      const current = tabs.indexOf(btn);
      let next = current;
      if (event.key === 'ArrowRight') next = (current + 1) % tabs.length;
      else if (event.key === 'ArrowLeft') next = (current - 1 + tabs.length) % tabs.length;
      else if (event.key === 'Home') next = 0;
      else if (event.key === 'End') next = tabs.length - 1;
      else return;
      event.preventDefault();
      activateTab(tabs[next], true);
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
  else if (id === 'settings') loadSettings();
  else if (id === 'targets') loadTargets();
  else if (id === 'selftest') loadSelftest();
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
  let s = '<div aria-hidden="true">';
  for (let i = 0; i < n; i++) s += `<div class="ln sk sk-line${i % 3 === 2 ? ' short' : ''}"></div>`;
  return s + '</div>';
}

function skKv(n) {
  let s = '<div class="sk-group" aria-hidden="true">';
  for (let i = 0; i < n; i++) s += '<div class="k sk sk-line short"></div><div class="v sk sk-line"></div>';
  return s + '</div>';
}

const DETAIL_KEYS = [
  ['MANUFACTURER', 'Pabrikan'], ['PRODUCT', 'Product'],
  ['SECURITY_PATCH', 'Security patch'],
  ['SERIAL', 'Serial'], ['ANDROID_ID', 'Entropy profil'], ['GOOGLE_AID', 'GAID lokal'],
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

function setActionProgress(state) {
  const el = document.getElementById('actionProgress');
  if (!el) return;
  const run = RUN_ID_RE.test(state.run || '') ? state.run.slice(0, 8) : '—';
  if (!state.stage) {
    el.hidden = true;
    el.textContent = '';
    return;
  }
  el.hidden = false;
  el.textContent = `Run ${run} · ${state.stage} · ${state.status || 'running'}`;
}

async function readActionSnapshot(expectedRun) {
  const probe = ownerProcessProbeShell(ACTION_LOCK + '/owner');
  const cmd = `printf '%s\\n' '__STATE__'; cat ${shq(ACTION_STATE)} 2>/dev/null || true; ` +
    `printf '%s\\n' '__RESULT__'; cat ${shq(ACTION_RESULT)} 2>/dev/null || true; ` +
    `printf '%s\\n' '__LOCK__'; cat ${shq(ACTION_LOCK + '/owner')} 2>/dev/null || true; ` +
    `printf '%s\\n' '__ALIVE__'; ${probe}; ` +
    `if [ -n "$_sbx_actual_start" ] && ` +
    `[ "$_sbx_actual_start" = "$_sbx_owner_start" ]; then printf 1; else printf 0; fi`;
  const r = await run(cmd);
  if (!r.ok) return { state: {}, records: [], owner: {}, live: false };
  const snapshot = splitActionSnapshot(r.out);
  snapshot.live = isActionLive(snapshot.state, snapshot.owner, expectedRun);
  return snapshot;
}

function sleep(ms) {
  return new Promise(resolve => setTimeout(resolve, ms));
}

async function monitorAction(runId, stop) {
  while (!stop.done) {
    const snapshot = await readActionSnapshot(runId);
    if (snapshot.state.run === runId) setActionProgress(snapshot.state);
    if (actionResult(snapshot.records, runId)) return;
    await sleep(ACTION_POLL_MS);
  }
}

async function reconcileAction(runId, detail) {
  if (!RUN_ID_RE.test(runId || '')) {
    toast('Hasil belum diketahui · run ID tidak diterima', { kind: 'warn', sticky: true, detail });
    return null;
  }
  for (;;) {
    const snapshot = await readActionSnapshot(runId);
    if (snapshot.state.run === runId) setActionProgress(snapshot.state);
    const result = actionResult(snapshot.records, runId);
    if (result) {
      const view = actionPresentation(result);
      toast(view.title, { kind: view.kind, sticky: view.kind === 'error', detail: result.raw });
      return result;
    }
    if (!snapshot.live) {
      toast('Hasil belum diketahui · jangan jalankan ulang otomatis', { kind: 'warn', sticky: true, detail });
      return null;
    }
    toast('Action masih berjalan · menunggu hasil tahan lama', { kind: 'info', sticky: true, detail: `run=${runId}` });
    await sleep(ACTION_POLL_MS);
  }
}

async function runAction() {
  const runId = Array.from(crypto.getRandomValues(new Uint8Array(16)), b => b.toString(16).padStart(2, '0')).join('');
  setActionProgress({ run: runId, stage: 'starting', status: 'running' });
  const cmd = `${ENV} && SBX_ACTION_RUN_ID=${shq(runId)} sh ${shq(MODDIR)}/action.sh 2>&1`;
  const stop = { done: false };
  const monitor = monitorAction(runId, stop).catch(() => {});
  try {
    const out = await shell(cmd);
    const records = parseActionProtocol(out);
    const result = actionResult(records, runId);
    if (!result) return reconcileAction(runId, out);
    const view = actionPresentation(result);
    toast(view.title, { kind: view.kind, sticky: view.kind === 'error', detail: out });
    return result;
  } catch (error) {
    const detail = [error.stdout, error.stderr, error.message].filter(Boolean).join('\n');
    const records = parseActionProtocol(`${error.stdout || ''}\n${error.stderr || ''}`);
    const result = actionResult(records, runId);
    if (result) {
      const view = actionPresentation(result);
      toast(view.title, { kind: view.kind, sticky: view.kind === 'error', detail });
      return result;
    }
    return reconcileAction(runId, detail);
  } finally {
    stop.done = true;
    await monitor;
    await Promise.all([loadPersona(), loadRotate()]);
  }
}

if (typeof document !== 'undefined') {
document.getElementById('refreshBtn').addEventListener('click', loadPersona);
document.getElementById('freshenBtn').addEventListener('click', ev =>
  withLoading(ev.currentTarget, () => mutate(runAction)));
}

const ROT_CARDS = [
  { key: 'ssaid',       name: 'Regenerasi SSAID', desc: 'Hapus penyimpanan SSAID sistem; Android membuat ulang saat reboot (bukan hook API per-aplikasi)', get: null },
  { key: 'gaid',        name: 'GAID lokal',      desc: 'Tulis Settings.Global + XML GMS best-effort; nilai nol mempertahankan opt-out lokal, API tetap milik layanan', get: 'GOOGLE_AID' },
  { key: 'boot-count',  name: 'Boot count',    desc: 'Settings.Global.boot_count = BOOT_COUNT identity.prop', get: 'BOOT_COUNT' },
  { key: 'applog',      name: 'AppLog ByteDance', desc: 'did/iid/ssid/openudid/clientudid/cdid untuk TikTok/Douyin — di-spoof in-process oleh hook JNI (L9)', get: null, applog: true },
];

async function loadRotate() {
  const wrap = document.getElementById('rotCards');
  wrap.innerHTML = ROT_CARDS.map((c, i) => `
    <div class="card" data-key="${c.key}">
      <div class="name">${c.name}</div>
      <div class="desc">${c.desc}</div>
      <div class="val sk sk-line" data-slot="val" aria-hidden="true"></div>
      <div class="actions"><button class="sm" data-rot="${c.key}" data-mutation>Rotasi</button></div>
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
    slot.removeAttribute('aria-hidden');
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

function loggedScriptCmd(label, script, moddir = MODDIR) {
  const env = moduleEnv(moddir);
  const guard = actionMutationGuardShell(moddir);
  const log = moddir + '/debug/rotate.log';
  const out = moddir + '/debug/.webui-output.$$';
  const header = `printf '[%s] ==> ${label} (webui)\\n' "$(date '+%F %T')"`;
  return `${env} && ${guard}; mkdir -p ${shq(moddir)}/debug && ` +
    `_sbx_out=${shq(out)}; rm -f "$_sbx_out"; ` +
    `${script} >"$_sbx_out" 2>&1; _sbx_rc=$?; ` +
    `{ ${header}; cat "$_sbx_out"; } >> ${shq(log)} 2>&1; _sbx_log_rc=$?; ` +
    `cat "$_sbx_out"; rm -f "$_sbx_out"; ` +
    `[ "$_sbx_rc" -ne 0 ] && exit "$_sbx_rc"; exit "$_sbx_log_rc"`;
}

function rotateCmd(key) {
  return loggedScriptCmd(`rotate ${key}`, `sh ${shq(ROTATE_SH)} ${shq(key)}`);
}

async function finishRotate(r, label) {
  if (!r.ok) toast(trimTitle(r.err.message || 'Rotasi gagal'), { kind: 'error', detail: r.err.stdout || r.err.stderr || '' });
  else { const s = summarizeRotate(r.out, label); toast(s.title, { kind: s.kind, detail: s.detail }); }
  await loadRotate();
}

async function rotateOne(key, btn) {
  await withLoading(btn, () => mutate(async () => {
    const r = await run(rotateCmd(key));
    const label = (ROT_CARDS.find(c => c.key === key) || {}).name || key;
    await finishRotate(r, label);
  }));
}

if (typeof document !== 'undefined') {
document.getElementById('rotAll').addEventListener('click', ev => withLoading(ev.currentTarget, () =>
  mutate(async () => {
    const r = await run(rotateCmd('all'));
    await finishRotate(r, 'Rotasi semua');
  })));
}

const EXPERIMENT_FLAGS = [
  'SBX_NATIVE_READ', 'SBX_PROC_VERSION', 'SBX_MEMINFO',
  'SBX_CPU_REVISION',
];

function renderSettingsState(kv, available = true) {
  const master = kv.SBX_NATIVE_READ !== '0';
  for (const key of EXPERIMENT_FLAGS) {
    const input = document.querySelector(`input[data-flag="${key}"]`);
    if (!input) continue;
    input.checked = key === 'SBX_NATIVE_READ' ? master : kv[key] === '1';
    input.disabled = !available || (key !== 'SBX_NATIVE_READ' && !master);
  }
  document.getElementById('settingsStatus').textContent = !available
    ? 'identity.prop belum ada. Buat persona terlebih dahulu.'
    : master
      ? 'Master aktif. Opsi anak tetap independen dan default-nonaktif.'
      : 'Master nonaktif: seluruh presentasi native dilewatkan genuine.';
}

async function loadSettings() {
  const status = document.getElementById('settingsStatus');
  status.textContent = 'Memuat pengaturan…';
  const r = await run(`cat ${shq(IDENTITY)} 2>/dev/null || true`);
  if (!r.ok || !r.out.trim()) {
    renderSettingsState({}, false);
    return;
  }
  renderSettingsState(parseProp(r.out));
}

if (typeof document !== 'undefined') {
document.getElementById('settingsReload').addEventListener('click', loadSettings);
document.querySelectorAll('#settings input[data-flag]').forEach(input => {
  input.addEventListener('change', () => mutate(async () => {
    const key = input.dataset.flag;
    const value = input.checked ? '1' : '0';
    input.disabled = true;
    const cmd = guardedMutationCmd(`sandboxid set-flag ${shq(key)} ${value}`);
    const r = await run(cmd);
    if (!r.ok) {
      toast(trimTitle(r.err.message || 'Gagal menyimpan pengaturan'), {
        kind: 'error', detail: r.err.stdout || r.err.stderr || '',
      });
    } else {
      toast(`${key}=${value} tersimpan`, { kind: 'ok', detail: r.out });
    }
    await loadSettings();
  }));
});
}

function targetSaveCmd(content, moddir = MODDIR) {
  const env = moduleEnv(moddir);
  const targets = moddir + '/target.txt';
  const tmp = moddir + '/.target.webui.$$';
  const backup = moddir + '/.target.webui-backup.$$';
  const b64 = (typeof Buffer !== 'undefined')
    ? Buffer.from(content, 'utf8').toString('base64')
    : btoa(unescape(encodeURIComponent(content)));
  return `${env} && umask 077 && ${actionMutationGuardShell(moddir)}; ` +
    `_sbx_tmp=${shq(tmp)}; _sbx_backup=${shq(backup)}; _sbx_had_old=0; ` +
    `rm -f "$_sbx_tmp" "$_sbx_backup" || exit $?; ` +
    `printf '%s' ${shq(b64)} | base64 -d > "$_sbx_tmp" || ` +
    `{ _sbx_rc=$?; rm -f "$_sbx_tmp" "$_sbx_backup"; exit "$_sbx_rc"; }; ` +
    `chmod 0644 "$_sbx_tmp" || ` +
    `{ _sbx_rc=$?; rm -f "$_sbx_tmp" "$_sbx_backup"; exit "$_sbx_rc"; }; ` +
    `if [ -e ${shq(targets)} ]; then ` +
    `cp -p ${shq(targets)} "$_sbx_backup" || ` +
    `{ _sbx_rc=$?; rm -f "$_sbx_tmp" "$_sbx_backup"; exit "$_sbx_rc"; }; ` +
    `_sbx_had_old=1; fi; ` +
    `mv -f "$_sbx_tmp" ${shq(targets)} || ` +
    `{ _sbx_rc=$?; rm -f "$_sbx_tmp" "$_sbx_backup"; exit "$_sbx_rc"; }; ` +
    `sandboxid targets --processes >/dev/null 2>&1; _sbx_rc=$?; ` +
    `if [ "$_sbx_rc" -ne 0 ]; then ` +
    `if [ "$_sbx_had_old" -eq 1 ]; then ` +
    `mv -f "$_sbx_backup" ${shq(targets)} || exit 32; ` +
    `else rm -f ${shq(targets)} || exit 32; fi; exit "$_sbx_rc"; fi; ` +
    `rm -f "$_sbx_backup" || exit $?`;
}

async function loadTargets() {
  const ta = document.getElementById('tgtArea');
  const r = await safeExec(`cat ${shq(TARGETS)} 2>/dev/null || true`);
  ta.value = r.ok ? r.out : '';
  document.getElementById('tgtStatus').textContent = '';
}
if (typeof document !== 'undefined') {
document.getElementById('tgtReload').addEventListener('click', loadTargets);
document.getElementById('tgtSave').addEventListener('click', ev => withLoading(ev.currentTarget, () =>
  mutate(async () => {
    const ta = document.getElementById('tgtArea');
    const content = ta.value.replace(/\r\n/g, '\n');
    const r = await run(targetSaveCmd(content));
    if (!r.ok) {
      toast('target.txt ditolak; file lama dipertahankan', { kind: 'error', detail: r.err.stdout || r.err.stderr || r.err.message });
      return;
    }
    toast('target.txt tersimpan', { kind: 'ok' });
    const lines = content.split('\n').filter(line => line.trim() && !line.trim().startsWith('#')).length;
    document.getElementById('tgtStatus').textContent = `${lines} proses \u00b7 dimuat ulang saat spawn berikutnya`;
  })));
}

const ST_CAT = {
  identitas: 'Identitas', koherensi: 'Koherensi', vbmeta: 'Verified boot', build: 'Build',
  selinux: 'SELinux', rom: 'Emulator / ROM', root: 'Root', mount: 'Mount', hosts: 'Hosts',
  hooks: 'Hook per-app',
};
const ST_KIND = {
  PASS: 'st-pass', WARN: 'st-warn', FAIL: 'st-fail', INFO: 'st-info',
};

function parseSelftest(out) {
  const rows = [];
  let summary = null;
  for (const line of String(out || '').replace(/\r/g, '').split('\n')) {
    const s = line.match(/^SELFTEST\s+SUMMARY\s+(.*)$/);
    if (s) {
      const g = {};
      s[1].replace(/(\w+)=(\d+)/g, (_, k, v) => { g[k] = Number(v); return ''; });
      summary = g;
      continue;
    }
    const m = line.match(/^SELFTEST\s+(\S+)\s+(PASS|WARN|FAIL|INFO)\s*(.*)$/);
    if (m) rows.push({ cat: m[1], status: m[2], detail: m[3] });
  }
  return { rows, summary };
}

function renderSelftest(out) {
  const { rows, summary } = parseSelftest(out);
  const body = document.getElementById('stBody');
  const sum = document.getElementById('stSummary');
  if (!rows.length) {
    body.innerHTML = '<div class="empty">Tidak ada hasil uji. Coba jalankan lagi.</div>';
    sum.textContent = '';
    return;
  }
  body.innerHTML = rows.map(r => {
    const cls = ST_KIND[r.status] || ST_KIND.INFO;
    const cat = ST_CAT[r.cat] || r.cat;
    return `<div class="card st ${cls}">` +
      `<div class="st-head"><span class="st-badge">${escapeHtml(r.status)}</span>` +
      `<span class="name">${escapeHtml(cat)}</span></div>` +
      `<div class="desc">${escapeHtml(r.detail)}</div></div>`;
  }).join('');
  body.querySelectorAll('.card').forEach((el, i) => el.style.setProperty('--i', i));
  if (summary) {
    const p = summary.pass || 0, w = summary.warn || 0, f = summary.fail || 0, inf = summary.info || 0;
    sum.textContent = `${p} pass · ${w} warn · ${f} fail · ${inf} info`;
  } else {
    sum.textContent = '';
  }
}

async function runSelftest(showToast) {
  const body = document.getElementById('stBody');
  body.innerHTML = skLines(8);
  const r = await run(`${ENV} && sh ${shq(SELFTEST_SH)} 2>&1`);
  if (!r.ok) {
    const msg = (r.err && r.err.message) || 'error';
    body.innerHTML = `<div class="empty">Gagal menjalankan uji: ${escapeHtml(msg)}</div>`;
    document.getElementById('stSummary').textContent = '';
    if (showToast) toast(trimTitle(msg), { kind: 'error', detail: (r.err && (r.err.stdout || r.err.stderr)) || '' });
    return;
  }
  renderSelftest(r.out);
  if (showToast) {
    const { summary } = parseSelftest(r.out);
    const f = (summary && summary.fail) || 0, w = (summary && summary.warn) || 0, p = (summary && summary.pass) || 0;
    const kind = f > 0 ? 'error' : (w > 0 ? 'warn' : 'ok');
    toast(`Uji selesai · ${p} pass · ${w} warn · ${f} fail`, { kind, detail: r.out });
  }
}

async function loadSelftest() { return runSelftest(false); }

if (typeof document !== 'undefined') {
document.getElementById('stRun').addEventListener('click', (ev) =>
  withLoading(ev.currentTarget, () => runSelftest(true)));
}

async function loadLog() {
  const src = document.getElementById('logSrc').value;
  const body = document.getElementById('logBody');
  body.innerHTML = skLines(10);
  let cmd;
  if (src === 'action')  cmd = `tail -n 400 ${shq(ACTION_LOG)} 2>/dev/null || echo '(belum ada action.log \u2014 tekan "Acak perangkat baru" atau tombol Action di KSU/APatch)'`;
  else if (src === 'rotate') cmd = `tail -n 400 ${shq(ROTATE_LOG)} 2>/dev/null || echo '(belum ada rotate.log \u2014 tekan tombol Rotasi)'`;
  else if (src === 'session') cmd = `ls -t ${shq(MODDIR)}/debug/session-*.log 2>/dev/null | head -n 1 | xargs -r tail -n 400 || echo '(tidak ada session log \u2014 pasang varian debug)'`;
  else if (src === 'crashes') cmd = `tail -n 400 ${shq(MODDIR)}/debug/crashes.log 2>/dev/null || echo '(belum ada crashes.log)'`;
  else if (src === 'logcat') cmd = `logcat -d -t 200 -v time -s SandboxID:V SandboxIDCompanion:V 2>&1 | tail -n 200`;
  const r = await safeExec(cmd);
  const text = (r.ok ? r.out : (r.err && r.err.message) || 'error') || '(kosong)';
  body.innerHTML = renderLogHtml(text);
  body.scrollTop = body.scrollHeight;
}
if (typeof document !== 'undefined') {
document.getElementById('logRefresh').addEventListener('click', loadLog);
document.getElementById('logSrc').addEventListener('change', loadLog);
}

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

if (typeof document !== 'undefined') {
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
    await loadPersona();
    const snapshot = await readActionSnapshot('');
    if (snapshot.state.status === 'running' && RUN_ID_RE.test(snapshot.state.run || '')) {
      setActionProgress(snapshot.state);
      await mutate(() => reconcileAction(snapshot.state.run, 'Action ditemukan masih berjalan saat WebUI dibuka.'));
    }
  })();
})();
}

if (typeof module !== 'undefined' && module.exports) {
  module.exports = {
    actionMutationGuardShell, actionPresentation, actionResult,
    actionRunFromRecords, cleanupBridgeCallback, exec, guardedMutationCmd,
    isActionLive, loggedScriptCmd, moduleEnv, mutate, ownerProcessProbeShell,
    parseActionProtocol, parseKeyValues, shq,
    splitActionSnapshot, targetSaveCmd,
  };
}
