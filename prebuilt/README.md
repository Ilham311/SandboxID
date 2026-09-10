# prebuilt/

Vendored third-party binaries bundled into the module ZIP.

## resetprop-rs

- **What:** Rust reimplementation of Magisk's `resetprop`, used by the native
  `sandboxid` CLI (`jni/config.hpp` → `RESETPROP`) to set system properties
  when the Magisk `resetprop` applet is not on `PATH`.
- **Architecture:** `arm64-v8a` (aarch64) **only**. ELF PIE, built with NDK
  r26d for Android API 26, stripped. It will **not** run on `armeabi-v7a`,
  `x86`, or `x86_64` devices — on those ABIs `customize.sh` removes it and the
  shell layer falls back to the Magisk `resetprop` applet (`helpers.sh:rp_set`).
- **Integrity (C1):** the SHA-256 is pinned in `resetprop-rs.sha256`. Both
  `build.sh` (package time) and `customize.sh` (install time) verify the binary
  against this file and refuse to ship / install a mismatched blob. This blocks
  a silently-swapped or corrupted binary from reaching a rooted device.

### Verify manually

    sha256sum -c prebuilt/resetprop-rs.sha256

### Regenerate the checksum after an intentional update

    sha256sum prebuilt/resetprop-rs | sed 's# .*/# #' > prebuilt/resetprop-rs.sha256
    # then confirm: sha256sum -c prebuilt/resetprop-rs.sha256

Update this note (and the `.sha256`) in the same commit whenever the binary is
replaced, and record the upstream source/commit the binary was built from.
