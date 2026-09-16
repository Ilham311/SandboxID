/* Host check of webroot/console.js under a stubbed DOM.
 *
 * The WebUI cannot run on the host, but its pure logic can: console capture,
 * the unacknowledged-error badge, the stream guards that keep the root bridge
 * from being pinned, the base64 save transport, and above all the contract
 * that a logging path never throws into its caller.
 *
 * Run via tests/host/run.sh (needs node), or directly: node tests/host/console_check.cjs
 */
const real0 = console;
const path = require('path').join(__dirname, '..', '..', 'webroot', 'console.js');

const els = {};
let logTabClick = null;
global.document = {
  getElementById: id => els[id] || null,
  /* wire() attaches the badge-ack listener to the Log tab; capture it so the
   * test can fire it. */
  querySelector: sel => ({
    'nav .tab[data-tab="log"]': {
      addEventListener: (ev, fn) => { if (ev === 'click') logTabClick = fn; },
    },
  })[sel] || null,
  createElement: () => ({ style: {}, classList: { add() {}, remove() {} },
    setAttribute() {}, appendChild() {}, addEventListener() {}, click() {}, remove() {} }),
  addEventListener: () => {},
  readyState: 'complete',
  body: { appendChild() {} },
};
global.window = global;
global.addEventListener = () => {};
global.btoa = s => Buffer.from(s, 'binary').toString('base64');

/* app.js is loaded before console.js on the page and owns escapeHtml. Provide
 * it here too, or the escaping checks would only exercise the identity fallback. */
global.escapeHtml = s => String(s).replace(/[&<>"']/g, c =>
  ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

/* Delegate to the target: console.js replaces console[k] at load, and we must
 * observe the REPLACEMENT, not shadow it — otherwise capture silently never
 * fires and every size assertion fails for the wrong reason. */
const cap = [];
global.console = new Proxy(real0, {
  get(t, k) {
    const v = t[k];
    if (typeof v === 'function' && ['log', 'info', 'warn', 'error'].includes(k))
      return (...a) => { cap.push(k + ': ' + a.join(' ')); return v.apply(t, a); };
    return v;
  },
});

const R = [];
const ok = (c, m) => R.push((c ? 'ok   ' : 'FAIL ') + m);
const fail = () => { process.exitCode = 1; };
const dump = () => { real0.log(R.join('\n')); };

try {
  require(path);
  ok(typeof SbxLog === 'object' && typeof SbxConsole === 'object', 'SbxLog/SbxConsole exposed');
} catch (e) { process.stdout.write('LOAD THROW: ' + e.stack + '\n'); process.exitCode = 1; process.exit(); }

els.logBody = { textContent: '', innerHTML: '', scrollTop: 0, scrollHeight: 0, clientHeight: 0 };
els.logSrc = { value: 'webui' };

/* ---- capture + render ---------------------------------------------------- */
const before = SbxLog.size;
console.log('hello from app');
console.error('boom');
ok(SbxLog.size === before + 2, 'console.* captured ' + SbxLog.size + ' of ' + (before + 2));

console.log('<script>alert(1)</script> & <b>bold</b>');
SbxConsole.render();
ok(/lvl-err/.test(els.logBody.innerHTML), 'render marks errors as lvl-err');
ok(/lvl-info/.test(els.logBody.innerHTML), 'render marks info as lvl-info');
ok(/hello from app/.test(els.logBody.innerHTML), 'render preserves message text');
ok(/&lt;script&gt;/.test(els.logBody.innerHTML), 'a <script> message is escaped');
ok(!/<script>alert/.test(els.logBody.innerHTML), 'no raw <script> reaches the DOM');
ok(/&amp;/.test(els.logBody.innerHTML), 'ampersands are escaped');

/* ---- badge semantics ----------------------------------------------------- */
els.tabLogDot = { hidden: true };
console.warn('careful');
ok(els.tabLogDot.hidden === false, 'badge unhidden on warn');
SbxConsole.clear();
ok(SbxLog.size === 0, 'clear() empties the buffer');
ok(els.tabLogDot.hidden === false, 'clear() leaves the badge for the user to ack');
if (logTabClick) logTabClick();
ok(els.tabLogDot.hidden === true, 'tapping the Log tab acknowledges the badge');

/* ---- stream guards: the reason the runner exists at all ------------------ */
/* exec must be in place BEFORE the first run(), or the guard is skipped. */
let ran = null;
global.exec = async c => { ran = c; return 'ok'; };
SbxLog.run('logcat');               ok(ran === 'logcat -d', 'bare logcat rewritten to -d (got ' + ran + ')');
/* -s is only a filter spec: it still streams, so it needs -d too. */
SbxLog.run('logcat -s X:V');        ok(ran === 'logcat -d -s X:V', 'logcat -s also gets -d (got ' + ran + ')');
SbxLog.run('logcat -d -s X:V');     ok(ran === 'logcat -d -s X:V', 'logcat already exiting is left alone');
ran = null; SbxLog.run('top');      ok(ran === null, 'bare top refused');
ran = null; SbxLog.run('tail -f /x'); ok(ran === null, 'tail -f refused');
ran = null; SbxLog.run('top -n 5'); ok(ran === 'top -n 5', 'top -n allowed');
ran = null; SbxLog.run('getevent -c 3'); ok(ran === 'getevent -c 3', 'getevent -c allowed');
ran = null; SbxLog.run('');         ok(ran === null, 'empty command is a no-op');

/* ---- save: quoting-free base64 transport, round-trips hostile characters -- */
els.logBody.textContent = 'x';
SbxLog.save().then(r => {
  ok(!!r && r.ok === true, 'save() ok -> ' + JSON.stringify(r));
  ok(/base64 -d/.test(ran), 'save travels via base64, not shell quoting');

  /* never throw to the caller: a broken render path must stay inside add() */
  Object.defineProperty(global, 'escapeHtml', {
    value: () => { throw new Error('boom'); }, configurable: true, writable: true,
  });
  let escaped = false;
  try { SbxLog.error('post-break'); } catch (e) { escaped = true; }
  ok(!escaped, 'broken render path did not escape into the caller');
  delete global.escapeHtml;

  ok(cap.length >= 4, 'proxy saw ' + cap.length + ' console calls through the shim');

  const bad = R.filter(l => l.startsWith('FAIL'));
  dump();
  real0.log(bad.length ? '\n== ' + bad.length + ' FAILURES ==' : '\n== ALL CONSOLE.JS CHECKS PASSED ==');
  if (bad.length) fail();
}, e => { dump(); real0.log('save() REJECTED: ' + (e && e.stack || e)); fail(); });
