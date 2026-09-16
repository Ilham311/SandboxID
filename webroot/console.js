'use strict';

/* console.js — capture the WebUI's own JavaScript surface.
 *
 * Why this exists: the module's whole purpose is making invisible things
 * observable, yet the WebUI itself was blind. If identity.prop failed to
 * parse, or a fetch of carriers.tsv threw, the page just sat there — the
 * failure was as invisible as the identifiers it is supposed to surface.
 * This module captures console.*, window errors and rejected promises into
 * a buffer that the existing Log page can render, so a broken page says
 * why instead of saying nothing.
 *
 * It also offers a shell runner. ksu.exec resolves only when the spawned
 * process EXITS, buffering stdout first. A command that never exits
 * (bare `logcat`, `top`, `tail -f`) therefore never resolves and pins the
 * bridge until the timeout in app.js fires. The runner rewrites or refuses
 * those instead of relying on the timeout — the timeout is a backstop, not
 * the mechanism.
 *
 * No new CSS: output reuses the .logbody / .ln / .lvl-* classes the Log
 * page already defines. Loaded after app.js and deliberately defensive
 * about its globals, since the two files can be served independently. */

(function () {
  const MAX = 800;                  // cap on buffered and rendered lines
  const buffer = [];                // { t, lvl, msg }
  let badgeShown = false;

  const byId = id => document.getElementById(id);

  /* app.js's globals. MODDIR there is a top-level const, which does not become
   * a window property, so a bare reference throws if these two files are ever
   * served separately. typeof on an undeclared identifier is safe; use it. */
  const DIR = (typeof MODDIR !== 'undefined' && MODDIR) || '/data/adb/modules/sandboxid';

  function stamp() {
    const d = new Date(), p = n => ('0' + n).slice(-2);
    return p(d.getHours()) + ':' + p(d.getMinutes()) + ':' + p(d.getSeconds());
  }

  /* A logging path must never throw into its caller: if render() is broken,
   * the symptom would be the app crashing at every console call, which is
   * worse than a missing log line. So everything is swallowed here, and the
   * re-entrancy guard also stops the window error handler from looping. */
  let busy = false;
  function add(msg, lvl) {
    if (busy) return;
    busy = true;
    try {
      buffer.push({ t: stamp(), lvl: lvl || 'info', msg: String(msg) });
      if (buffer.length > MAX) buffer.shift();
      if (lvl === 'err' || lvl === 'warn') markTab(true);
      if (isCurrentSource()) render();
    } catch (_) { /* a broken render path must not escape into the caller */ }
    finally { busy = false; }
  }

  /* An error badge on the Log tab. Clearing it is the user's call — a
   * failure the user has not acknowledged should not quietly disappear. */
  function markTab(on) {
    const tab = byId('tabLogDot');
    if (!tab) return;
    if (on && !badgeShown) { badgeShown = true; tab.hidden = false; }
    else if (!on) { badgeShown = false; tab.hidden = true; }
  }

  function isCurrentSource() {
    const sel = byId('logSrc');
    return sel && sel.value === 'webui';
  }

  /* Renders into the same #logBody the other sources use, with the same
   * line markup, so a captured stack trace looks like any other log. */
  function render() {
    const body = byId('logBody');
    if (!body) return;
    if (!buffer.length) {
      body.innerHTML = '<div class="ln lvl-info"><span class="ts">--:--:--</span> (masih kosong — jejak JS dan error akan muncul di sini)</div>';
      return;
    }
    const atBottom = body.scrollHeight - body.scrollTop - body.clientHeight < 48;
    const esc = typeof window.escapeHtml === 'function' ? window.escapeHtml : (s => s);
    body.innerHTML = buffer.map(e =>
      `<div class="ln lvl-${e.lvl}"><span class="ts">${esc(e.t)}</span> ${esc(e.msg)}</div>`
    ).join('');
    if (atBottom) body.scrollTop = body.scrollHeight;
  }

  /* ---- native console + global error capture -------------------------- */
  const fmt = a => {
    if (typeof a === 'string') return a;
    if (a instanceof Error) return a.stack || a.message;
    try { return JSON.stringify(a); } catch (_) { return String(a); }
  };

  ['log', 'info', 'warn', 'error'].forEach(k => {
    const orig = (window.console && console[k]) ? console[k].bind(console) : function () {};
    console[k] = (...args) => {
      orig(...args);
      add(args.map(fmt).join(' '), k === 'error' ? 'err' : (k === 'warn' ? 'warn' : 'info'));
    };
  });
  window.addEventListener('error', ev => {
    const where = String(ev.filename || '').split('/').pop();
    add(`Error: ${ev.message}${where ? ` (${where}:${ev.lineno})` : ''}`, 'err');
  });
  window.addEventListener('unhandledrejection', ev => {
    const r = ev && ev.reason;
    add('Unhandled rejection: ' + (r && (r.stack || r.message) ? (r.stack || r.message) : String(r)), 'err');
  });

  /* ---- shell runner ---------------------------------------------------- */
  /* -d/-c/-g/-S/--help make logcat exit; without one of them it streams. */
  function fixStreaming(cmd) {
    if (/\blogcat\b/.test(cmd) && !/-[dcgS]\b|--help/.test(cmd))
      return cmd.replace(/\blogcat\b/, 'logcat -d');
    return cmd;
  }
  function isInfinite(cmd) {
    return /(^|[;&|]\s*)(top|htop)\b(?![^|;&]*\s-n\b)/.test(cmd)   // top without -n
        || /\btail\s+-[fF]\b/.test(cmd)                             // tail -f
        || /\bgetevent\b(?![^|;&]*-c\b)/.test(cmd)                  // getevent without -c
        || /\bcat\b[^|;&]*(\/dev\/|\/proc\/kmsg)/.test(cmd);        // cat of a stream
  }

  async function run(cmd) {
    cmd = String(cmd || '').trim();
    if (!cmd) return;
    add('$ ' + cmd, 'cmd');
    if (isInfinite(cmd)) {
      add('Perintah itu tidak pernah selesai dan akan mengunci bridge. Tambahkan jumlah / -d / redirect ke file agar berhenti.', 'warn');
      return;
    }
    const fixed = fixStreaming(cmd);
    if (fixed !== cmd) add('↪ dijalankan sebagai: ' + fixed + '   (logcat tanpa opsi stream selamanya — ditambah -d)', 'warn');
    if (typeof exec !== "function") { add('root bridge tidak tersedia — perintah shell hanya jalan di perangkat.', 'warn'); return; }
    try {
      const out = await exec(fixed);
      add(String(out || '').replace(/\s+$/, '') || '(no output)', 'ok');
    } catch (e) { add(String(e && e.message || e), 'err'); }
  }

  /* ---- copy / save ----------------------------------------------------- */
  /* Both act on what the Log page is currently showing, so they are useful
   * for every source — a logcat dump can be saved exactly like a JS trace.
   * The buffer is the fallback when the page has not rendered anything. */
  function currentText() {
    const body = byId('logBody');
    const shown = body && String(body.textContent || '').trim();
    if (shown) return shown;
    return buffer.map(e => `${e.t}  ${(e.lvl || 'info').toUpperCase().padEnd(7)} ${e.msg}`).join('\n');
  }

  async function copy() {
    const text = currentText();
    if (!text) return false;
    try {
      if (navigator.clipboard && navigator.clipboard.writeText) {
        await navigator.clipboard.writeText(text);
        return true;
      }
    } catch (_) {}
    return false;
  }

  /* base64 transport: a log can contain any character a shell command can,
   * so rather than quoting it, encode it and let base64 -d decode on the
   * far side. toybox ships base64, so this is safe on-device. */
  async function save() {
    const text = currentText();
    if (!text) return { ok: false, why: 'empty' };
    let b64 = '';
    try { b64 = btoa(unescape(encodeURIComponent(text))); } catch (_) { return { ok: false, why: 'encode' }; }
    if (typeof exec !== "function") return { ok: false, why: 'nobridge' };
    const q = typeof window.shq === 'function' ? window.shq : (s => `'${String(s).replace(/'/g, "'\\''")}'`);
    try {
      const r = await exec(`mkdir -p ${q(DIR + '/debug')} && printf '%s\\n' ${b64} | base64 -d >> "${DIR}/debug/webui.log" && echo ok`);
      return { ok: /ok/.test(String(r || '')), path: DIR + '/debug/webui.log' };
    } catch (e) { return { ok: false, why: String(e && e.message || e) }; }
  }

  /* Clearing the badge is a user action; clearing the buffer is not, so
   * clear() leaves the badge alone. */
  function clear() { buffer.length = 0; render(); }

  /* ---- wire the Log page controls ------------------------------------- */
  /* Defensive about each element: this file must not break the page if the
   * markup it expects is not there. */
  function wire() {
    /* Tapping the Log tab is how the user acknowledges the badge. app.js owns
     * tab switching; this only clears the dot, so the two stay decoupled. */
    const logTab = document.querySelector('nav .tab[data-tab="log"]');
    if (logTab) logTab.addEventListener('click', () => markTab(false));

    const form = byId('shellForm');
    if (form) {
      form.addEventListener('submit', e => {
        e.preventDefault();
        const input = byId('shellCmd');
        if (!input) return;
        const cmd = input.value;
        input.value = '';
        run(cmd);
      });
      /* keep the input above the on-screen keyboard */
      const input = byId('shellCmd');
      if (input) input.addEventListener('focus', () => {
        setTimeout(() => input.scrollIntoView({ block: 'nearest' }), 250);
      });
    }
    const cp = byId('logCopy');
    if (cp) cp.addEventListener('click', async () => {
      const ok = await copy();
      if (window.toast) toast(ok ? 'Disalin' : 'Gagal menyalin', { kind: ok ? 'ok' : 'error' });
    });
    const sv = byId('logSave');
    if (sv) sv.addEventListener('click', async () => {
      const r = await save();
      if (window.toast) {
        if (r.ok) toast('Disimpan ke ' + r.path, { kind: 'ok' });
        else if (r.why === 'nobridge') toast('root bridge tidak tersedia', { kind: 'error' });
        else if (r.why !== 'empty') toast('Gagal menyimpan: ' + r.why, { kind: 'error' });
      }
    });
  }
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', wire);
  else wire();

  window.SbxLog = {
    info: m => add(m, 'info'),
    success: m => add(m, 'ok'),
    warn: m => add(m, 'warn'),
    error: m => add(m, 'err'),
    render, clear, run, copy, save, markTab,
    get size() { return buffer.length; },
  };

  /* app.js calls this when the Log tab is opened or the source switches to
   * 'webui', so the live buffer is what the user sees. */
  window.SbxConsole = { render, isCurrentSource, clear };
})();
