---
title: "Framework CLI must use safe FDs"
description: "Catch settings/pm/am/cmd calls that skip /dev/null FDs or misplace --user"
when: "A pull request adds or edits a shell invocation of settings, pm, am, or cmd"
actions: "Post a review comment checking the call routes through _fw_run (helpers.sh) or native run_framework with std FDs redirected to /dev/null, and that any --user 0 appears AFTER the verb; explain the FAILED_TRANSACTION risk if not"
---

# Framework CLI safety

`cmd(1)` forwards the caller's std FDs to `system_server` over binder. From the
action path those FDs are a pty/pipe or a `/data/adb` file the SELinux domain
forbids, so an un-redirected call fails intermittently with
`FAILED_TRANSACTION` (documented at `helpers.sh:90-95`).

When this rule matches, confirm the new invocation:

- Runs through **`_fw_run`** (or native `run_framework`), i.e. with
  `</dev/null >/dev/null 2>&1`; a raw `settings`/`pm`/`am`/`cmd` call is a defect.
- Places **`--user 0` after the verb** (`settings put --user 0 …`,
  `pm clear --user 0 …`, `am force-stop --user 0 …`).
- Stays POSIX `sh` (no bashisms) and will pass `shellcheck -S warning`.
