# User-facing output — Indonesian house style + parse-safe copy

User-facing strings in `action.sh`, `rotate_ids.sh`, and `webroot/` are a
**product surface**, reviewed as such. Two rules bind together here: the copy
must match the house style, **and** it must not break the machine contract that
`app.js` parses. Read the contract first:

@../documents/parse-token-safelist.md

## House style (Important when copy changes; this is a product decision)

- **Language: Indonesian**, in the shipped *casual-but-clear* register — the
  code uses friendly forms consistently ("nggak", "bikin", "santai", "beres").
  Keep it natural and **consistent**: the thing to flag is a register *clash* —
  a casual line rewritten stiff/formal (or vice-versa) so the copy reads
  "kaku/aneh". Use "perangkat" for device-as-noun, sentence case, drop
  user-directed pronouns where natural.
- **No *new* "alay" decoration.** Don't *add* emoji, decorative separators,
  ASCII-art frames, ALL-CAPS shouting, or marketing exclamation ("Keren!!",
  "🔥") to script stdout/log lines or WebUI prose. These existing affordances
  are established and are **not** findings: the 🎲 label on the Undi button
  (`webroot/index.html`), the toast status glyphs `✓ ✕ ⚠ ℹ` (`app.js:83`), the
  `①②③④` step markers in `action.sh`, and the persona box drawn by
  `autopif.sh` `display_profile`. The de-alay bar applies to *new* additions and
  to the `action.sh` / `rotate_ids.sh` printed summary, which was deliberately
  cleaned up — don't regress it back toward emoji/marketing.
- **Keep technical terms** as terms: `SSAID`, `GAID`, `MAC`, `boot count`,
  `fingerprint`, `resetprop`, package names. Don't "translate" them into
  awkward Indonesian.
- Don't mix a slangy register with a stiff one in the same message — that
  clash ("kaku/aneh") is the specific thing to avoid. Match the surrounding
  lines.

Flag copy that violates the above as **Important** if it ships to users, because
tone is a maintained product decision in this repo, not personal taste. A pure
typo fix that keeps the parse tokens intact is a **Suggestion**.

## Parse-safety when editing copy (Important / Critical)

Before approving any change to a printed line, cross-check
`documents/parse-token-safelist.md`:

- Success banners `OK - persona baru aktif` and `OK - fresh`, the `BRAND :` /
  `MODEL :` label shape, and the `Gagal` / `✗` / `!` failure markers are
  **parsed**, not decorative. Editing their text without updating
  `summarizeAction()` in `app.js` mis-reports success/failure — **Critical** if
  it hides a failure, **Important** otherwise.
- `[OK]` / `[WARN]` / `[ERR]` / `==>` line prefixes and the phrases
  `N step(s) reported failure` / `REBOOT REQUIRED` drive `summarizeRotate()` and
  `classifyLine()`. Keep them exact.
- Adding emoji/box-art to a parsed line can also shift a regex anchor (`^`) or
  inject characters into a captured group — another reason the de-alay rule and
  the parse contract reinforce each other.

## Logging discipline (Suggestion→Important)

- Keep the `[OK]`/`[WARN]`/`[ERR]` tagging consistent — it is both the
  human signal and the WebUI's severity source.
- Route diagnostic noise to the debug log / `2>/dev/null`, not to the user-facing
  summary. A new `echo` that spams every spawn into logcat is an Important
  robustness finding (log growth + noise), separate from style.
