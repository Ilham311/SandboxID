---
title: "Framework CLI must use safe FDs"
description: "Catch settings/pm/am/cmd calls that leave std FDs on the inherited pty/pipe or misplace --user"
when: "A pull request adds or edits a shell invocation of settings, pm, am, or cmd, OR a native run_framework/run_bin call site in jni/*.cpp"
actions: "Post a review comment checking the call keeps its std FDs off the inherited pty/pipe (via _fw_run, native run_framework, or its own </dev/null >/dev/null 2>&1), and that any --user 0 appears AFTER the verb; explain the FAILED_TRANSACTION risk if not"
---

# Framework CLI safety

`cmd(1)` forwards the caller's std FDs to `system_server` over binder. From the
action path those FDs are a pty/pipe or a `/data/adb` file the SELinux domain
forbids, so an un-redirected call fails intermittently with
`FAILED_TRANSACTION` (documented at `helpers.sh:90-95`).

When this rule matches, confirm the new invocation:

- Keeps the call's std FDs off the inherited pty/pipe and pointed at `/dev/null`
  — normally via **`_fw_run`** (`helpers.sh:96-104`) or native `run_framework`
  (`jni/sandboxid.cpp:148`). A raw call is acceptable when it already does this
  itself: a mutating call ending in `</dev/null >/dev/null 2>&1` (e.g.
  `rotate_ids.sh:54`, `action.sh:19`), or a `settings get`/read that captures
  stdout in `$(…)` and still redirects stdin+stderr `</dev/null 2>/dev/null`
  (`rotate_ids.sh:264-266`) — the latter cannot use `_fw_run`, which discards
  stdout. Flag only a call that leaves stdin/stderr (or an uncaptured stdout) on
  the inherited FD.
- Places **`--user 0` after the verb** (`settings put --user 0 …`,
  `pm clear --user 0 …`, `am force-stop --user 0 …`).
- Stays POSIX `sh` (no bashisms) and will pass `shellcheck -S warning`.
