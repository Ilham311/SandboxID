---
title: "Native change needs rebuild + reflash"
description: "Remind that jni/ and build changes can't be runtime-verified in the PR"
when: "A pull request modifies any file under jni/ (*.cpp, *.hpp, *.java), jni/CMakeLists.txt, or build.sh"
actions: "Post one comment noting that native changes take effect only after a CI rebuild + reflash + reboot, so no runtime symptom can be verified from the diff alone; ask the author to confirm the change was built and tested on-device, and to check the CLOCK_MONOTONIC / exemptFd / no-Werror invariants in .gitar/review/20-native-and-hooks.md"
---

# Native change — ship-path reminder

The native layer is compiled per-ABI and only runs after the module is reflashed
and the device rebooted. A green PR does **not** mean the runtime behaviour was
verified.

When this rule matches, remind the author to:

- Confirm the change actually **builds** for all four ABIs
  (`arm64-v8a armeabi-v7a x86_64 x86`) and was flashed + reboot-tested on device
  if it claims to fix a runtime symptom.
- Re-check the native invariants: clock virtualization offsets **`CLOCK_BOOTTIME`
  only** (never `CLOCK_MONOTONIC`); code that relies on `exemptFd` must degrade
  gracefully; the main module stays `-fno-exceptions -fno-rtti`; and there is
  **no `-Werror`**, so any new `-Wall`/`-Wextra` warning must be treated as a
  defect, not ignored.
- Keep any native-printed strings that the WebUI parses in sync (see the
  parse-token rule).
