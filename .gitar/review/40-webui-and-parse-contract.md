# WebUI review (`webroot/`) + the output parse-token contract

The WebUI is a KernelSU/APatch **WebUI** page (`webroot/index.html`, `app.js`,
`style.css`) that talks to the device only through the root bridge
`ksu.exec(cmd, "{}", cb)` (`webroot/app.js:48`). Two concerns dominate review
here: **shell-injection through that bridge**, and the **parse-token contract**
(the WebUI parses script stdout, so copy edits can silently break it).

@../documents/parse-token-safelist.md

## Shell-injection through `ksu.exec` — Critical

Everything passed to `exec()`/`shell()`/`run()` runs as **root** in a shell.
Review every command string that interpolates a variable:

- User/file-derived values **must** go through `shq()` (`app.js:12`,
  single-quote escaping) — as `target.txt` save does via base64
  (`app.js:429`). A raw `${x}` inside a command template is a finding unless `x`
  is a module-controlled constant.
- Prefer the existing pattern: build the argument in JS, base64-encode, and
  `base64 -d` on the device (see `tgtSave`, `app.js:425`) rather than
  interpolating free text into the command.
- Flag any new `eval`, backticks, or nested `sh -c "...$var..."` that reaches
  the bridge without escaping.

## CSP / XSS — Important

- The page runs under a CSP with `style-src 'self'` (no `unsafe-inline`).
  Per-element styling is done via **CSSOM** `el.style.setProperty(...)`
  (`app.js:336`, `379`), **not** inline `style="..."` attributes, which the CSP
  blocks. A PR that reintroduces inline `style=` attributes or an inline
  `<script>`/`<style>` will be silently dropped by the CSP — flag it.
- All device-derived text rendered into the DOM must pass through
  `escapeHtml()` (`app.js:455`). `identity.prop` values, log lines, and prop
  keys are attacker-influenceable (a target app can set some of them), so
  `innerHTML` with un-escaped device data is an XSS finding. Confirm
  `renderHero`, `renderTiles`, `renderLogHtml`, and the detail grid keep using
  `escapeHtml`.

## Bridge robustness — Suggestion→Important

- `exec()` rejects with a clear message when `ksu`/`ksu.exec` is absent
  (`app.js:50`) — keep that guard; the page also loads in a plain browser for
  layout work.
- Callbacks are namespaced and deleted after use (`app.js:54`). A new async call
  that leaks `window[cb]` or races two calls onto one callback name is a leak
  finding.
- `withLoading()` (`app.js:142`) guards against double-submit via
  `dataset.busy`. New action buttons should reuse it, not re-implement.

## What is *not* a finding here

- The WebUI shelling out to run `action.sh`/`rotate_ids.sh` as root is the
  intended design (it is the same code the root-manager Action button runs).
- Using `Date.now()`/`Math.random()` for a callback nonce (`app.js:54`) is fine
  in the browser context — this is not the native RNG.
