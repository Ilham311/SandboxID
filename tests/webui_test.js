'use strict';

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { execFileSync, spawnSync } = require('child_process');
const web = require('../webroot/app.js');

const tests = [];
let checks = 0;
function test(name, fn) {
  tests.push({ name, fn });
}

async function runTests() {
  for (const { name, fn } of tests) {
    try {
      await fn();
      checks += 1;
      process.stdout.write(`ok ${checks} - ${name}\n`);
    } catch (error) {
      process.stderr.write(`not ok ${checks + 1} - ${name}\n${error.stack}\n`);
      process.exitCode = 1;
      return;
    }
  }
  process.stdout.write(`${checks} checks, 0 failures\n`);
}

const RUN = '0123456789abcdef0123456789abcdef';
const OTHER = 'fedcba9876543210fedcba9876543210';

function shell(command, options = {}) {
  return spawnSync('/bin/sh', ['-c', command], {
    encoding: 'utf8', ...options,
  });
}

function makeSandbox() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'sbx-webui-'));
  fs.mkdirSync(path.join(root, 'bin'));
  fs.mkdirSync(path.join(root, 'debug'));
  const cli = path.join(root, 'bin', 'sandboxid');
  fs.writeFileSync(cli, `#!/bin/sh
if [ "$1" = targets ] && [ "$2" = --processes ]; then
  grep -q '^invalid$' "$MODDIR/target.txt" 2>/dev/null && exit 64
  exit 0
fi
exit 127
`);
  fs.chmodSync(cli, 0o755);
  return root;
}

function clean(root) {
  fs.rmSync(root, { recursive: true, force: true });
}

test('protocol parser ignores human logs and foreign prefixes', () => {
  const text = [
    '[OK] success run=' + RUN,
    'SBX_ACTION_V0\tRESULT\tstatus=success\trun=' + RUN,
    'SBX_ACTION_V1\tSTAGE\tprepare\tok\trun=' + RUN,
    'SBX_ACTION_V1\tRESULT\tstatus=partial\treboot=1\twarnings=2\tfailures=1\trun=' + RUN,
  ].join('\n');
  const records = web.parseActionProtocol(text);
  assert.strictEqual(records.length, 2);
  assert.deepStrictEqual(
    [records[0].type, records[0].stage, records[0].outcome],
    ['STAGE', 'prepare', 'ok'],
  );
  assert.strictEqual(web.actionRunFromRecords(records), RUN);
  assert.strictEqual(web.actionResult(records, RUN).fields.status, 'partial');
  assert.strictEqual(web.actionResult(records, OTHER), null);
});

test('action outcomes render honestly', () => {
  const result = fields => ({ fields });
  assert.deepStrictEqual(web.actionPresentation(null), {
    kind: 'warn', title: 'Hasil belum diketahui', terminal: false,
  });
  assert.strictEqual(web.actionPresentation(result({ status: 'success', reboot: '0', warnings: '0' })).kind, 'ok');
  assert.match(web.actionPresentation(result({ status: 'success', reboot: '1' })).title, /reboot/);
  assert.strictEqual(web.actionPresentation(result({ status: 'partial' })).kind, 'warn');
  assert.match(web.actionPresentation(result({ status: 'rolled-back' })).title, /dipulihkan/);
  assert.match(web.actionPresentation(result({ status: 'busy' })).title, /masih berjalan/);
  assert.strictEqual(web.actionPresentation(result({ status: 'degraded' })).kind, 'error');
  assert.match(web.actionPresentation(result({ status: 'failed' })).title, /sebelum/);
});

test('durable snapshot is split and matched by exact run', () => {
  const raw = `__STATE__\nrun=${RUN}\nstage=commit\nstatus=running\n` +
    `__RESULT__\nSBX_ACTION_V1\tRESULT\tstatus=success\trun=${RUN}\n` +
    `__LOCK__\npid=42\nproc_start=99\nrun=${RUN}\n__ALIVE__\n1`;
  const snap = web.splitActionSnapshot(raw);
  assert.strictEqual(snap.state.stage, 'commit');
  assert.strictEqual(snap.owner.alive, '1');
  assert.strictEqual(web.actionResult(snap.records, RUN).fields.status, 'success');
  assert.strictEqual(web.isActionLive(snap.state, snap.owner, RUN), true);
  snap.owner.alive = '0';
  assert.strictEqual(web.isActionLive(snap.state, snap.owner, RUN), false);
  snap.owner.alive = '1';
  snap.owner.run = OTHER;
  assert.strictEqual(web.isActionLive(snap.state, snap.owner, RUN), false);
});

test('mutation coordinator serializes asynchronous mutations', async () => {
  const seen = [];
  let release;
  const gate = new Promise(resolve => { release = resolve; });
  const first = web.mutate(async () => {
    seen.push('first:start');
    await gate;
    seen.push('first:end');
  });
  const second = web.mutate(async () => { seen.push('second'); });
  await new Promise(resolve => setImmediate(resolve));
  assert.deepStrictEqual(seen, ['first:start']);
  release();
  await Promise.all([first, second]);
  assert.deepStrictEqual(seen, ['first:start', 'first:end', 'second']);
});

test('bridge timeout removes callback and late callback cannot settle again', async () => {
  const oldWindow = global.window;
  const oldKsu = global.ksu;
  const fakeWindow = {};
  let callbackName = '';
  global.window = fakeWindow;
  global.ksu = { exec(_command, _options, name) { callbackName = name; } };
  await assert.rejects(web.exec('true', 10), error => error.code === 'bridge-timeout' && error.unknown);
  assert.ok(callbackName.startsWith('__ksucb_'));
  assert.strictEqual(Object.prototype.hasOwnProperty.call(fakeWindow, callbackName), false);
  global.window = oldWindow;
  global.ksu = oldKsu;
});

test('bridge callbacks are cleaned on success, failure, and synchronous throw', async () => {
  const oldWindow = global.window;
  const oldKsu = global.ksu;
  global.window = {};
  global.ksu = { exec(_command, _options, name) { global.window[name](0, 'ok', ''); } };
  assert.strictEqual(await web.exec('true', 50), 'ok');
  assert.strictEqual(Object.keys(global.window).length, 0);
  global.ksu = { exec(_command, _options, name) { global.window[name](7, '', 'bad'); } };
  await assert.rejects(web.exec('false', 50), error => error.code === 7 && error.message === 'bad');
  assert.strictEqual(Object.keys(global.window).length, 0);
  global.ksu = { exec() { throw new Error('sync'); } };
  await assert.rejects(web.exec('false', 50), /sync/);
  assert.strictEqual(Object.keys(global.window).length, 0);
  global.window = oldWindow;
  global.ksu = oldKsu;
});

test('target publication preserves empty files and validates atomically', () => {
  const root = makeSandbox();
  try {
    fs.writeFileSync(path.join(root, 'target.txt'), 'old.package\n');
    let result = shell(web.targetSaveCmd('', root));
    assert.strictEqual(result.status, 0, result.stderr);
    assert.strictEqual(fs.readFileSync(path.join(root, 'target.txt'), 'utf8'), '');

    result = shell(web.targetSaveCmd('new.package\n', root));
    assert.strictEqual(result.status, 0, result.stderr);
    assert.strictEqual(fs.readFileSync(path.join(root, 'target.txt'), 'utf8'), 'new.package\n');

    result = shell(web.targetSaveCmd('invalid\n', root));
    assert.strictEqual(result.status, 64, result.stderr);
    assert.strictEqual(fs.readFileSync(path.join(root, 'target.txt'), 'utf8'), 'new.package\n');
    assert.deepStrictEqual(
      fs.readdirSync(root).filter(name => name.startsWith('.target.webui')),
      [],
    );
  } finally { clean(root); }
});

test('target decode failure is not swallowed before publication', () => {
  const root = makeSandbox();
  const fakeBin = fs.mkdtempSync(path.join(os.tmpdir(), 'sbx-base64-'));
  try {
    fs.writeFileSync(path.join(root, 'target.txt'), 'unchanged\n');
    fs.writeFileSync(path.join(fakeBin, 'base64'), '#!/bin/sh\nexit 23\n');
    fs.chmodSync(path.join(fakeBin, 'base64'), 0o755);
    const env = { ...process.env, PATH: `${fakeBin}:/bin:/usr/bin` };
    const result = shell(web.targetSaveCmd('replacement\n', root), { env });
    assert.strictEqual(result.status, 23, result.stderr);
    assert.strictEqual(fs.readFileSync(path.join(root, 'target.txt'), 'utf8'), 'unchanged\n');
  } finally {
    clean(root);
    fs.rmSync(fakeBin, { recursive: true, force: true });
  }
});

test('target save refuses a verified live Action owner but ignores stale owner metadata', () => {
  const root = makeSandbox();
  try {
    fs.writeFileSync(path.join(root, 'target.txt'), 'old.package\n');
    fs.mkdirSync(path.join(root, '.action.lock'));
    const stat = fs.readFileSync(`/proc/${process.pid}/stat`, 'utf8');
    const start = stat.slice(stat.lastIndexOf(') ') + 2).trim().split(/\s+/)[19];
    fs.writeFileSync(path.join(root, '.action.lock', 'owner'),
      `pid=${process.pid}\nproc_start=${start}\nrun=${RUN}\n`);
    let result = shell(web.targetSaveCmd('blocked.package\n', root));
    assert.strictEqual(result.status, 75, result.stderr);
    assert.strictEqual(fs.readFileSync(path.join(root, 'target.txt'), 'utf8'), 'old.package\n');

    fs.writeFileSync(path.join(root, '.action.lock', 'owner'),
      `pid=${process.pid}\nproc_start=1\nrun=${RUN}\n`);
    result = shell(web.targetSaveCmd('allowed.package\n', root));
    assert.strictEqual(result.status, 0, result.stderr);
    assert.strictEqual(fs.readFileSync(path.join(root, 'target.txt'), 'utf8'), 'allowed.package\n');
  } finally { clean(root); }
});

test('rotation logging returns only current output and preserves script exit', () => {
  const root = makeSandbox();
  try {
    const log = path.join(root, 'debug', 'rotate.log');
    fs.writeFileSync(log, '[ERR] historical failure\n');
    let result = shell(web.loggedScriptCmd('unit', "printf 'current only\n'", root));
    assert.strictEqual(result.status, 0, result.stderr);
    assert.strictEqual(result.stdout, 'current only\n');
    assert.match(fs.readFileSync(log, 'utf8'), /historical failure[\s\S]*current only/);

    result = shell(web.loggedScriptCmd('unit fail', "printf 'new failure\n'; exit 9", root));
    assert.strictEqual(result.status, 9, result.stderr);
    assert.strictEqual(result.stdout, 'new failure\n');
  } finally { clean(root); }
});

test('HTML has complete accessible tab and DOM contracts', () => {
  const html = fs.readFileSync(path.join(__dirname, '../webroot/index.html'), 'utf8');
  const js = fs.readFileSync(path.join(__dirname, '../webroot/app.js'), 'utf8');
  const ids = Array.from(html.matchAll(/\bid="([^"]+)"/g), match => match[1]);
  assert.strictEqual(new Set(ids).size, ids.length, 'duplicate DOM id');
  const referenced = Array.from(js.matchAll(/getElementById\\(['"]([^'"]+)['"]\\)/g), match => match[1]);
  assert.deepStrictEqual(referenced.filter(id => !ids.includes(id)), []);
  assert.match(html, /<nav id="nav" role="tablist"/);
  const tabs = Array.from(html.matchAll(/<button class="tab[^>]*id="([^"]+)"[^>]*aria-controls="([^"]+)"[^>]*>/g));
  assert.strictEqual(tabs.length, 6);
  for (const match of tabs) {
    const [, tabId, panelId] = match;
    assert.match(match[0], /role="tab"/);
    assert.match(match[0], /aria-selected="(?:true|false)"/);
    const panel = new RegExp(`<section id="${panelId}"[^>]*role="tabpanel"[^>]*aria-labelledby="${tabId}"[^>]*>`);
    assert.match(html, panel);
  }
  assert.match(js, /ArrowRight/);
  assert.match(js, /ArrowLeft/);
  assert.match(js, /event\.key === 'Home'/);
  assert.match(js, /event\.key === 'End'/);
  assert.match(js, /page\.hidden = !active/);
});

test('strict CSP and semantic skeleton contracts remain intact', () => {
  const html = fs.readFileSync(path.join(__dirname, '../webroot/index.html'), 'utf8');
  const js = fs.readFileSync(path.join(__dirname, '../webroot/app.js'), 'utf8');
  const css = fs.readFileSync(path.join(__dirname, '../webroot/style.css'), 'utf8');
  const csp = html.match(/Content-Security-Policy" content="([^"]+)"/);
  assert.ok(csp);
  assert.match(csp[1], /script-src 'self'/);
  assert.match(csp[1], /style-src 'self'/);
  assert.doesNotMatch(csp[1], /unsafe-inline|unsafe-eval/);
  assert.doesNotMatch(html, /\son[a-z]+=/i);
  assert.match(js, /class="val sk sk-line" data-slot="val" aria-hidden="true"/);
  assert.match(js, /class="sk-group" aria-hidden="true"/);
  assert.match(css, /\.page\[hidden\] \{ display: none !important; \}/);
});

test('Node syntax checks pass for WebUI sources', () => {
  execFileSync(process.execPath, ['--check', path.join(__dirname, '../webroot/app.js')]);
  execFileSync(process.execPath, ['--check', path.join(__dirname, '../webroot/theme-init.js')]);
});

runTests();
