# Ternak TT v1.2.0 — Overlay Bundle

**Konteks**: Sandbox tempat aku kerja kehilangan filesystem + koneksi internet di tengah proses build v1.2.0. Repo `Ilham311/Tt` di GitHub-mu tetap jadi source of truth (masih v1.1.8). Bundle ini isinya **hanya file-file yang berubah untuk v1.2.0** — kamu overlay di atas checkout lokal Ternak TT-mu, commit, push.

## Cara pakai

```bash
# 1. Extract overlay ini ke /tmp
unzip -o ternak-tt-v1.2.0-overlay.zip -d /tmp/tt-v1.2.0

# 2. Masuk ke checkout Tt lokal-mu (yang sudah punya v1.1.8)
cd /path/to/your/Tt

# 3. Jalankan apply script
bash /tmp/tt-v1.2.0/apply-v1.2.0.sh /tmp/tt-v1.2.0

# 4. Verify
git status
git diff --stat

# 5. Commit + push
git add -A
git commit -m 'chore(release): v1.2.0 — ShadowHook AAR + patchelf SONAME'
git push
```

GitHub Actions akan otomatis tag `v1.2.0`, build, dan publish release.

## Isi overlay

| File | Aksi |
|------|------|
| `fetch_lsplant.sh` | **Full rewrite** (tambah ShadowHook AAR fetch + patchelf SONAME rewrite) |
| `jni/CMakeLists.txt` | **Full rewrite** (buang `add_subdirectory shadowhook`, ganti pakai IMPORTED SHARED) |
| `build.sh` | **Full rewrite** (buang shadowhook-build find, ganti pakai `prebuilt/shadowhook/`) |
| `.github/workflows/build.yml` | **Full rewrite** (tambah step install patchelf, rename step Path B fetch) |
| `module.prop` | Bump ke `v1.2.0` / `1200` |
| `update.json` | Bump ke `v1.2.0` |
| `customize.sh` | **Patch** (via apply script) — update ui_print header ke v1.2.0 |
| `CHANGELOG.md` | **Prepend** (via apply script) — tambah section v1.2.0 di atas v1.1.9 |

## Kenapa overlay, bukan zip full?

Sandbox filesystem-nya reset di tengah kerjaan, dan sandbox tidak punya internet keluar jadi tidak bisa `git clone` repo-mu untuk reconstruct baseline. File-file source code besar (`main.cpp`, `companion.cpp`, `ternak-tt.cpp`, `java_hooks.cpp`, `java_hooks.hpp`, `pool_tt.hpp`, `TernakHookHelper.java`, `service.sh`, `action.sh`, `post-fs-data.sh`, `summarize.sh`, `README.md`, `LICENSE`, `target.txt`, `.gitignore`, `prebuilt/.gitkeep`) tidak berubah dari v1.1.8, jadi tidak perlu di-ship ulang.

Overlay approach juga jauh lebih safe — kalau ada file lain di repo-mu yang aku tidak tahu (misal kamu tambah sesuatu manual), file itu tidak akan tertimpa.

## Ringkasan perubahan v1.2.0

Baca `CHANGELOG-v1.2.0.md` untuk detail lengkap. TL;DR:

1. **ShadowHook sekarang dikonsumsi sebagai AAR resmi dari Maven Central** (`com.bytedance.android:shadowhook:2.0.1`), sesuai cara ByteDance officially publish di `doc/manual.md`. Tidak ada compile dari source lagi → tidak ada `-Werror` cascade lagi.
2. **SONAME rewrite via patchelf** untuk hindari collision dengan `libshadowhook.so` bawaan TikTok/Douyin.
3. **ShadowHook versi 2.0.1** (Jun 2026) menggantikan v1.0.9 (Jan 2024) yang sumbernya kita clone sebelumnya. ByteDance jamin ABI backward compat, jadi call ke `shadowhook_hook_func_addr` dari code kita tetap jalan.
4. **Build lebih cepat** karena tidak compile ~120 CU ShadowHook C files.

Kalau CI v1.2.0 masih gagal (semoga tidak), kirim log-nya dan aku diagnose lagi.
