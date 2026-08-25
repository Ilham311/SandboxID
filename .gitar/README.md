# `.gitar/` — Gitar AI configuration for SandboxID

This directory configures [Gitar](https://docs.gitar.ai) AI code review for this
repository. It is tailored to SandboxID's real conventions (root Zygisk module,
native + shell + WebUI layers, the shell↔WebUI parse-token contract, casual
Indonesian output). Nothing here changes the module's runtime behaviour — it
only instructs the reviewer.

## Layout

```
.gitar/
  review/                     # custom review instructions — Gitar loads every .md here
    00-project-context.md     #   entry point: what the repo is, north star, severity
    10-security.md            #   root/IPC/mounts/SELinux/supply-chain
    20-native-and-hooks.md    #   Zygisk v5, JNI/PLT/LSPlant hooks, clocks, boot safety
    30-shell-scripts.md       #   POSIX sh, framework CLIs, kill-switches, data schemas
    40-webui-and-parse-contract.md  # WebUI (CSP/XSS/ksu.exec) + parse tokens
    50-output-and-localization.md   # Indonesian house style + parse-safe copy
  documents/                  # shared reference, pulled into review/ via @include (NOT auto-loaded)
    glossary.md               #   domain terms + native/shell boundary
    severity-rubric.md        #   Critical/Important/Suggestion calibrated for this module
    parse-token-safelist.md   #   exact shell-output ↔ app.js regex/key contract
  rules/                      # natural-language automations (YAML frontmatter + body)
    native-change-reflash.md
    parse-token-contract.md
    framework-cli-guard.md
    release-artifacts-ci-owned.md
```

## How it works

- **Review instructions**: Gitar reads every `.md` under `.gitar/review/` and
  uses them to tailor its security / bug / performance / edge-case / quality
  review of each PR. Files are numbered only for human reading order; Gitar loads
  all of them.
- **`@` includes**: a line like `@../documents/glossary.md` inlines that file
  (resolved relative to the including file, then falling back to the repo root).
  The context file pulls in the glossary and severity rubric; the WebUI and
  output files share the parse-token safelist — so the contract is defined once.
- **Rules**: each file in `.gitar/rules/` is a `when`/`actions` automation that
  fires on PR events (opened, new commits, etc.) to post targeted reminders for
  this repo's specific footguns.

## Maintaining these files

- Keep them **grounded**: cite `file:line` and real symbols; don't invent rules.
- When the code moves a parsed string, a prop key, or a convention, update the
  matching instruction (especially `documents/parse-token-safelist.md`).
- Prefer **fewer, higher-signal** rules — the severity rubric explicitly asks the
  reviewer not to re-file the README's documented "Known limitations".

See the Gitar docs for the authoritative feature reference:
<https://docs.gitar.ai/features/code-review> and
<https://docs.gitar.ai/features/rules>.
