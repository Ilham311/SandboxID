# prebuilt/

Vendored third-party binaries bundled into the module ZIP.

## resetprop-rs v0.6.0

SandboxID uses only the bundled `resetprop-rs`; it never falls back to Magisk's
`resetprop`, a PATH binary, or `setprop`. The four binaries are official assets
from <https://github.com/Enginex0/resetprop-rs/releases/tag/v0.6.0>:

- `resetprop-arm64-v8a`
- `resetprop-armeabi-v7a`
- `resetprop-x86_64`
- `resetprop-x86`

`resetprop-rs.sha256` pins the SHA-256 digests published by GitHub for that
release. `build.sh` requires and verifies all four assets before packaging.
`customize.sh` verifies the selected device ABI again, renames it to
`bin/resetprop-rs`, and aborts installation on a missing or mismatched binary.

Verify the vendored set manually:

    (cd prebuilt && sha256sum -c resetprop-rs.sha256)

When upgrading, replace all four assets and the manifest together, then update
the version and upstream release link in this file.
