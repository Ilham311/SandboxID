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
- **Runtime fallback:** this binary is optional. If it is not bundled or cannot
  be verified at install time, it is removed/refused and SandboxID requires a
  working `resetprop` or `resetprop-rs` command from `PATH`. There is no claim
  that every root manager provides that fallback; Action preflight reports it.
- **Integrity and license:** the SHA-256 is pinned in `resetprop-rs.sha256`, and
  the upstream MIT notice is retained in `resetprop-rs.LICENSE`. Packaging fails
  if any member of that set is missing, no `sha256sum` verifier exists, or
  verification fails. Installation repeats these checks and removes the bundled
  backend on any incomplete, unverifiable, or mismatched state.

### Upstream provenance

The tracked binary is byte-for-byte identical to the
[`resetprop-arm64-v8a`](https://github.com/Enginex0/resetprop-rs/releases/tag/v0.6.0)
asset published for upstream `Enginex0/resetprop-rs` release `v0.6.0` (GitHub
asset ID `451371154`): 477,256 bytes with SHA-256
`6d82fc8e92089ce24226636ec4d24deb15655d7a7473d37d24fa4ae53588c1df`.
The annotated tag targets source commit
[`084fce0f212a5f0bc17c48b8e33d8d3bbe04205e`](https://github.com/Enginex0/resetprop-rs/commit/084fce0f212a5f0bc17c48b8e33d8d3bbe04205e),
and upstream Actions run
[`27772967636`](https://github.com/Enginex0/resetprop-rs/actions/runs/27772967636)
built and published that release. The ELF metadata independently identifies an
AArch64 PIE targeting Android API 26, NDK r26d build 11579264, Clang/LLD 17.0.2
(r487747e), and rustc 1.96.0 (`ac68faa20`).

This establishes exact release-asset identity, not end-to-end cryptographic or
reproducible-build provenance: the upstream tag/commit are unsigned, release
assets are mutable, and the release has no signature, SLSA attestation, SBOM,
or pinned GitHub Actions references.

### Verify manually

    (cd prebuilt && sha256sum -c resetprop-rs.sha256)

### Regenerate the checksum after an intentional update

    sha256sum prebuilt/resetprop-rs | sed 's# .*/# #' > prebuilt/resetprop-rs.sha256
    # then confirm: (cd prebuilt && sha256sum -c resetprop-rs.sha256)

Update this note (and the `.sha256`) in the same commit whenever the binary is
replaced, and record the upstream source/commit the binary was built from.
