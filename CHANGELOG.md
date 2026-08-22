# Changelog

## Unreleased — Fix: per-app bind-mounts on non-Magisk Zygisk providers

### Fixed

- **Per-app `build.prop` bind-mounts silently skipped on KernelSU/strict Zygisk
  providers** (ReZygisk / Zygisk Next / NeoZygisk). Root cause: the companion
  socket opened in `preAppSpecialize` was carried into `postAppSpecialize` only
  by `Api::exemptFd()`. On the USAP / `nativeSpecializeAppProcess` path those
  providers return `false` from `exemptFd` (they require the
  `APP_FORK_AND_SPECIALIZE` flag; Magisk instead short-circuits to `true` on the
  specialize path, which is why Magisk users never saw this). With the fd not
  exempted, zygote closed the socket before `postAppSpecialize`, so
  `CMD_DO_MOUNTS` failed (`companion DO_MOUNTS write failed (socket unusable
  post-specialize?)`) and the native-reader defense layer (bind-mounted fake
  `build.prop` / `settings_secure.xml`) never applied. The in-process Java
  `Build.*` / `SystemProperties` hooks were unaffected and still applied.
- **~70 `exemptFd returned false` warnings per boot on an idle module.**
  `exemptFd` was called for *every* app process before the target check, so
  non-target system apps each logged a warning even with an empty `target.txt`.

### Changed

- `jni/main.cpp` `preAppSpecialize`: reordered so the `CMD_GET_IDENTITY` target
  check runs first and `exemptFd()` is called **only for confirmed targets**.
  Non-target apps no longer touch `exemptFd` and no longer log about it.
- `jni/main.cpp` / `jni/companion.cpp` / `jni/config.hpp`: added an
  `exemptFd`-free fallback. When `exemptFd()` returns `false` for a target, the
  module reads its pre-unshare mount-namespace inode (`/proc/self/ns/mnt`) and
  sends the new **`CMD_DEFER_MOUNTS {pid, ns_ino}`** over the still-open
  pre-specialize socket. The companion spawns a detached watcher that polls
  `/proc/<pid>/ns/mnt` until it moves off that baseline — i.e. AOSP's
  `SpecializeCommon` ran `unshare(CLONE_NEWNS)` and the app has a private
  namespace — and only then performs the bind-mounts (reusing `do_mounts_via_fork`).
  Mounting before unshare (which would leak into zygote's shared namespace) is
  explicitly guarded against. When `exemptFd()` returns `true` (e.g. Magisk), the
  original `postAppSpecialize` `CMD_DO_MOUNTS` path is used **unchanged** — zero
  behavior change for providers that already worked.

### Known limitations

- The deferred mount completes shortly after the app unshares its namespace
  (~milliseconds, tightly polled). A target that reads a raw `build.prop` file in
  the very first moment of process start could in principle read it before the
  mount lands; the Java-layer `Build.*` / `SystemProperties` hooks are not subject
  to this race. If a provider never unshares for a process, the watcher times out
  (~8 s) and skips the mount rather than risk a shared-namespace leak.

## v2.0.0 (2026-08-21)

Rebrand to SandboxID — a generic, privacy-research / education framing with no
built-in application targets and no product-specific defaults. Runtime behavior
(hook layers, companion IPC, crash watchdog, atomic writes, hot-reload) is
unchanged; this release is naming, cleanup, and default-emptying only.

### Renamed

- Module id `ternak_tt` → `sandboxid`; module path `/data/adb/modules/sandboxid`.
- Display name `Ternak TT` → `SandboxID`; log tags `TernakTT`/`TernakTTCompanion`/`TernakTT-L3` → `SandboxID`/`SandboxIDCompanion`/`SandboxID-L3`.
- CLI binary `ternak-tt` → `sandboxid`; native library `libternak_tt.so` → `libsandboxid.so`.
- `jni/pool_tt.hpp` → `jni/pool.hpp`; `jni/tt_config.hpp` → `jni/config.hpp`.
- `jni/TtHook.java` → `jni/SandboxIDHook.java` (file layout only). The Java class
  inside is kept as the camouflaged `androidx.core.os.EnvCompatState` so a target
  app dumping loaded classes does not see an obvious anomaly.
- `jni/tt_lsplant.hpp` → `jni/lsplant.hpp`; generated DEX header `tt_hook_dex.h` →
  `hook_dex.h` (symbols `tt_hook_dex` / `tt_hook_dex_len` → `hook_dex` / `hook_dex_len`).
- `jni/ternak-tt.cpp` → `jni/sandboxid.cpp`.
- C++ namespace `tt::` → `sandboxid::`; `ttlsp::` → `sbxlsp::`; macro prefix `TT_` → `SBX_`.

### Changed

- `jni/companion.cpp` `reload_targets_if_changed()`: removed the built-in default target list; an absent `target.txt` now yields an empty list (no-op path).
- `jni/sandboxid.cpp` `load_targets()`: removed the default fallback; an empty `target.txt` returns an empty list.
- `jni/sandboxid.cpp` `usage()`: rewritten to a neutral description.
- `jni/sandboxid.cpp`: removed the unused `MODDIR` alias (fixes `-Wunused-const-variable` under `-Wall -Wextra`).
- `target.txt` now ships empty (0 lines) — the module is idle until the user adds packages.
- All comments stripped from source (`.cpp`, `.hpp`, `.java`, `.sh`, `.js`, `CMakeLists.txt`, workflow `.yml`); data literals (`SBX_POOL`, `VAL_DEFAULTS`, `STATIC_PROP_DEFAULTS`, `modem_prefix`) untouched.
- `jni/lsplant.hpp`: `kCls` and the `loadClass` string reference `androidx.core.os.EnvCompatState` (class name reverted to the camouflaged androidx-flavored name; `SandboxIDHook.java` keeps the new filename only).

### L3 — DEX header regeneration required before release

- The LSPlant L3 path is **default-OFF** (`SBX_ENABLE_LSPLANT`), so the release build
  is unaffected and an absent DEX header only produces the existing fail-safe
  (`LOGE` + continue; L1/L2 still apply).
- `jni/hook_dex.h` (the DEX bytecode for `androidx.core.os.EnvCompatState`) was not
  regenerated in this environment. **It must be regenerated before enabling L3**:
  `javac SandboxIDHook.java` → `d8` → `xxd -i` into `jni/hook_dex.h` so the embedded
  bytecode matches the current class name. Until then, L3 is **non-functional** and must
  not be enabled.
- No behavior change for the default (L3 off) runtime.

### Unchanged

- Hook layers, companion bind-mount, crash watchdog, atomic write, hot-reload, and CLI commands are identical to v1.0.36.

## Unreleased — L3 LSPlant foundation (scaffold, DEFAULT-OFF)

> **Catatan rilis:** perubahan di bawah **belum aktif di runtime**. Semuanya
> dikurung compile-flag `SBX_ENABLE_LSPLANT` (default OFF), jadi build release
> saat ini **byte-identik dengan v1.0.27**. `module.prop` **sengaja belum
> di-bump** — mem-bump ke v1.0.28 sebagai versi rilis berarti mengklaim
> kemampuan yang belum lolos verifikasi boot. Saat flag di-flip + lolos
> build+boot test, ubah heading ini jadi `## v1.0.28 (tanggal)` dan bump
> `module.prop` v1.0.27→v1.0.28 / versionCode 123→124.

### Added — L3: fondasi hook method Java non-native via LSPlant (`jni/tt_lsplant.hpp`)

- **Apa:** File wrapper baru `tt_lsplant.hpp` + call-site fail-safe di
  `postAppSpecialize` (`main.cpp`) + `option(SBX_ENABLE_LSPLANT)` di
  `CMakeLists.txt`. Default OFF → stub no-op, nol perubahan runtime. Saat ON:
  `sbxlsp::init()` menjalankan `lsplant::Init` (backend **Dobby** + resolver
  simbol libart), lalu `hook_android_id()` meng-ART-hook
  `Settings.Secure.getString(...)` agar app melihat `ANDROID_ID` persona yang
  deterministik.
- **Kenapa (rujuk trace Fase 2):** L2 (`hookJniNativeMethods`) hanya bisa
  mengikat method `native`; `Settings.Secure.getString` **bukan** native →
  butuh ART method-hooking. ANDROID_ID adalah identifier level-app terpenting,
  dan jalur file (mount `settings_secure.xml`) TIDAK andal untuknya: sejak API 26
  android_id ada di tabel per-app `ssaid`, bukan `settings_secure.xml` (itu
  sebabnya `rotate_ids.sh` harus `wipe_ssaid`). Hook method mem-*pin* nilai
  persis per-persona, bukan mengandalkan regenerasi acak.
- ⚠️ **BOOT RISK + mitigasi:** LSPlant menyentuh internal `ArtMethod`; kegagalan
  di ROM/versi tertentu = boot-loop **semua** app target. Mitigasi: (1) DEFAULT
  OFF via compile-flag → tak ada kode ART di build rilis saat ini; (2) call-site
  fail-safe — `Init`/`Hook` gagal ⇒ `LOGE` + lanjut, L1/L2/L7 tetap jalan, tak
  pernah `abort()`/unload setengah-jadi; (3) L3 dipasang **paling akhir** di
  `postAppSpecialize` sehingga layer terbukti terpasang lebih dulu.

### Notes — riset LSPlant (rujuk Fase 1)

- **Pin `v6.4`** (C++20 + header klasik). **Hindari `master`** (C++23 + C++
  Modules) — menaikkan bar toolchain tanpa manfaat untuk kasus ini.
- Backend inline-hook & resolver simbol **tidak dibundel** LSPlant — wajib
  disuplai lewat `InitInfo` (Dobby + lsparself/ElfImg, sesuai test resmi).
- Default `generated_class_name="LSPHooker_"` adalah fingerprint LSPosed →
  di-scaffold dengan nama netral (anti-tell).
- **Sengaja TIDAK di-hook** (alasan konkret): `WifiInfo.getMacAddress()` sudah
  dianonimkan `02:00:00:00:00:00` untuk app non-privileged (API 23+);
  `TelephonyManager.getImei/getSubscriberId/getSimSerialNumber` butuh
  `READ_PRIVILEGED_PHONE_STATE` (API 29+, permission signature) → app
  pihak-ketiga kena `SecurityException` sebelum body jalan, spoof di situ malah
  jadi anomali; `Build.getRadioVersion()` hanya membaca `gsm.version.baseband`
  yang sudah dicakup L2 + resetprop.

### Status SEAM & cara mengaktifkan (butuh build + boot test)

Kedua SEAM kini **sudah diimplementasikan sebagai kode** (bukan lagi placeholder):

- **SEAM #1 — resolver simbol libart:** di-wire ke **lsparself**
  (`lsparself::Elf("/libart.so")`), persis test resmi LSPlant v6.4. lsparself
  menangani `.gnu_debugdata` (mini-symtab LZMA) — sebabnya kita TIDAK hand-roll
  parser ELF naif yang akan diam-diam gagal menemukan simbol ART internal.
- **SEAM #2 — callback:** kelas Java `androidx.core.os.SandboxIDHook`
  (`jni/SandboxIDHook.java`) dengan `Object handle(Object[])` **pure-Java** (tanpa method
  `native`/RegisterNatives). Nilai spoof + `Method` asli disuntik dari native ke
  field statik; app dapat android_id persona untuk key `"android_id"`, dan nilai
  asli untuk key lain (chaining via backup). DEX-nya di-embed sebagai byte array
  (`jni/tt_hook_dex.h`) hasil `d8` atas `SandboxIDHook.java`; kalau header itu belum ada,
  `hook_android_id()` fail-safe (LOGE + lanjut).

Yang MASIH perlu dilakukan (semua butuh compiler + device — TIDAK tersedia di
lingkungan review ini, jadi belum diverifikasi):

1. Vendor `jni/external/{lsplant@v6.4 (--recurse-submodules), dobby, lsparself}`.
   (Submodule `lsparself`/`lsprism` LSPlant memakai URL SSH → di CI fetch lsparself
   via HTTPS terpisah.)
2. Bangun `jni/tt_hook_dex.h` dari `jni/SandboxIDHook.java`: `javac` → `d8` → `xxd -i`
   (simbol `tt_hook_dex` / `tt_hook_dex_len`).
3. `SBX_ENABLE_LSPLANT=ON ./build.sh` (build.sh sudah mengalirkan flag ke CMake;
   pertimbangkan `-DANDROID_STL=c++_static`), lalu **boot test per ABI/Android
   version** sebelum flag di-flip. `build.yml` sengaja TIDAK diubah (edit YAML buta
   berisiko memecah parsing CI produksi); langkah CI di atas ditambahkan saat enable.

### Changed — review pass (JNI local-ref hygiene + dokumentasi watchdog)

Pass review 6-gejala (mandiri, berbasis source-of-truth). 5 dari 6 gejala terbukti
false-positive / keep / defer; hanya **1** perbaikan kode nyata (hygiene, bukan crash)
+ **1** komentar. Build rilis **tetap byte-identik dengan v1.0.27** (perubahan hanya di
kode L3 default-OFF yang belum aktif + 1 komentar nol-efek di `main.cpp`).

- **`jni/tt_lsplant.hpp` (ON-branch, default-OFF) — JNI local-ref frame.**
  `load_callback_class()` & `hook_android_id()` kini membungkus seluruh body dengan
  `env->PushLocalFrame(16)` / `env->PopLocalFrame(nullptr)` (tiap early-return jadi
  return dari IIFE). Semua local transient (`bb/loaderCls/clCls/parent/loader/name` dan
  `jval/hooker/cb/sec/target/backup` + local yang dibuat ART/liblog di dalamnya) kini
  bebas di **setiap** jalur keluar (happy + tiap error), bukan hanya happy-path.
  GlobalRef (`g_cb_class`/`g_cb_object`/`g_backup`) & ref yang dipegang field statik
  (`spoof`/`original`) dibuat sebelum pop → selamat. **Nol perubahan perilaku**; ini
  kerapian, BUKAN fix leak — fungsi dipanggil sekali per-proses dan tabel local lama pun
  sudah auto-reclaimed saat `postAppSpecialize` return. `DeleteLocalRef(jval)` manual
  yang lama dihapus (kini ditangani frame).
- **`jni/main.cpp` — dokumentasi signal-set watchdog.** Tambah komentar di arm-site
  menjelaskan kenapa set `{ABRT,FPE,ILL}` sengaja TIDAK memuat SIGSEGV/SIGBUS (ART
  memakai SIGSEGV untuk implicit-null / stack-overflow / suspend / GC checks lalu pulih
  via handler-nya; arm SEGV = mencatat fault benign sebagai "CRASH"). **Komentar saja —
  biner tidak berubah.**

**Belum diverifikasi (gating — sama seperti seluruh L3):** cabang `SBX_ENABLE_LSPLANT=ON`
tak dapat di-compile di lingkungan review (lsplant/dobby/lsparself + `tt_hook_dex.h`
absent). Perubahan `tt_lsplant.hpp` di-review-by-spec (JNI PushLocalFrame/PopLocalFrame
+ LSPlant v6.4 API); **wajib** lolos compile + boot-test per-ABI saat flag di-enable.

**Tidak diubah (verdict false-positive/keep/defer):** `sandboxid.cpp`, `companion.cpp`,
`pool.hpp`, `config.hpp`, `SandboxIDHook.java`, `CMakeLists.txt`, `build.sh`.

---

## v1.0.27 (2026-08-18)

Rilis ini fokus ke **anti-leak identitas** (menutup nilai asli perangkat yang masih
bocor setelah persona Pixel dipasang) dan **anti-boot-loop** (mencegah persona yang
SDK-nya lebih tinggi dari OS asli). Semua temuan berasal dari trace alur boot → app
spawn → app membaca data sensitif. Istilah teknis dibiarkan Inggris.

### ⚠️ LEAK — SoC codename asli bocor lewat `ro.hardware` / `ro.board.platform`

- **Apa:** `ro.hardware` dan `ro.board.platform` kini di-drive dari persona (Tensor
  codename `gs101`/`gs201`/`zuma`/`zumapro`/`laguna`) secara end-to-end — field
  `platform` baru di `pool.hpp`, di-set oleh `gen_identity`, diserialisasi ke
  `identity.prop`, dipetakan di `hook_prop_get` (L2), lalu ditulis oleh `apply_native`
  (resetprop) **dan** `generate_mount_files` (build.prop sintetis).
- **Kenapa:** Versi sebelumnya sudah membuang hard-code `"qcom"`/`"sm8250"` dari tabel
  static default (benar — itu kontradiksi dengan Pixel/Tensor), TAPI tidak pernah
  memasang penggantinya: `gen_identity` tak men-set `BOARD_PLATFORM`, `serialize()`
  membuangnya, `apply_native`/build.prop tak menulis `ro.hardware`. Akibatnya kedua
  key jatuh ke chain `__system_property_get` → **SoC asli perangkat bocor** (mis.
  Snapdragon), padahal `Build.MODEL` mengklaim Pixel. Kombinasi Pixel-model +
  SoC-Qualcomm adalah sinyal deteksi yang kuat.
- **Mitigasi / catatan:** `gs101`/`gs201` sudah diverifikasi ke perangkat nyata.
  `zuma`/`zumapro`/`laguna` masih *widely-reported* dan **harus diverifikasi** ke unit
  asli / AOSP sebelum rilis luas — codename yang salah adalah tell baru (ditandai di
  komentar `pool.hpp`).

### ⚠️ LEAK — nama pasar asli bocor lewat `ro.product.marketname`

- **Apa:** Tambah pemetaan `ro.product.marketname` + `ro.product.vendor.marketname`
  → `MARKETNAME` di `hook_prop_get`, `MARKETNAME = model` di `gen_identity`, dan
  penulisannya di `apply_native` + build.prop.
- **Kenapa:** Key ini tak pernah dipetakan → app yang membaca `ro.product.marketname`
  mendapat nama pasar **perangkat asli** (mis. "Galaxy S23") sementara `Build.MODEL`
  bilang "Pixel 8". Inkonsistensi yang langsung terlihat.

### ⚠️ BOOT RISK — SDK gating (persona tak boleh lebih tinggi dari OS asli)

- **Apa:** `gen_identity` sekarang membaca SDK perangkat via `device_sdk()`
  (`ro.build.version.sdk`) lalu memfilter kandidat persona ke `sdk <= device_sdk`
  dan memilih acak dari yang lolos. Jika pembacaan SDK gagal, fallback ke persona
  ber-SDK **terendah** + warning (bukan ambil sembarang). Komentar `main.cpp` soal
  injeksi `SDK_INT` diperbaiki agar akurat ("does not exceed", bukan "equals").
- **Kenapa:** Sebelumnya persona dipilih acak dari seluruh pool tanpa melihat SDK
  asli. Jika `SDK_INT` persona **lebih tinggi** dari OS asli, app membaca
  `Build.VERSION.SDK_INT` lalu memanggil API framework yang belum ada di OS tersebut
  → `NoSuchMethodError` / crash / ANR.
- **Mitigasi:** Arah gating hanya turun (downgrade-only), yang memang arah aman;
  downgrade (persona ≤ device) tidak memicu API yang belum ada.

### ⚠️ LEAK — overlay mount dijamin tak merembes ke namespace zygote

- **Apa:** Setelah `setns` ke mount-namespace privat app, child companion kini
  menandai pohon namespace `MS_SLAVE | MS_REC` pada `"/"` sebelum melakukan bind.
  Bind-nya sendiri memang sudah dijalankan di `postAppSpecialize` (setelah app
  `unshare(CLONE_NEWNS)` di `SpecializeCommon`), bukan di `preAppSpecialize`.
- **Kenapa:** Jika bind terjadi sebelum app unshare — atau jika root namespace app
  masih `MS_SHARED` — overlay build.prop bisa **merembes ke namespace zygote** dan
  otomatis nge-spoof **setiap app yang lahir sesudahnya** (global leak, bukan per-app).
  `MS_SLAVE` memutus propagasi keluar sebagai belt-and-suspenders di atas timing
  `postAppSpecialize` yang sudah benar.
- **Mitigasi:** Best-effort — kegagalan `MS_SLAVE` bersifat non-fatal karena timing
  post-specialize sudah menjaga isolasi utama.

### Changed — RADIO (baseband) stabil per-persona

- **Apa:** `RADIO` / `gsm.version.baseband` kini diturunkan stabil dari persona:
  `modem_prefix(platform)` (mis. `gs101`→`g5123b`) + tanggal dari `security_patch`
  + potongan `incremental` — bukan lagi tanggal hari-ini + string modem hard-code.
- **Kenapa:** Versi lama meregenerasi RADIO dengan tanggal `localtime` **setiap
  `freshen`** → baseband berubah tiap rotasi dan tidak match platform persona.
  Baseband yang berubah tanpa OTA adalah anomali; sekarang tetap sama untuk persona
  yang sama.
- **Catatan:** Nilai `modem_prefix` bersifat *best-effort* dan sebaiknya diverifikasi
  ke firmware Pixel asli.

### Changed — hapus "detection tell" di build.prop sintetis

- **Apa:** Komentar header `# SandboxID synthetic build.prop (v1.0.3)` dan
  `# Partition alias` dihapus dari file mount, diganti header standar
  `# begin build properties`.
- **Kenapa:** App yang membaca `/system/build.prop` bisa `grep` string "SandboxID" /
  "synthetic" → fingerprint modul yang trivial. build.prop asli tak pernah memuat nama
  modul.

### Changed — companion fork-safety + diagnostik mount

- **Apa:** Child hasil `fork()` di companion tidak lagi memanggil `__android_log_print`
  sama sekali; ia mengumpulkan hasil ke POD `MountResult` (ok/fail/skip + errno per
  mount) dan mengirimnya ke **thread parent** via pipe, yang kemudian melakukan
  logging. VLA `src_fds` diganti `std::array<int, BIND_ENTRIES_N>`.
- **Kenapa:** Companion multi-thread (satu thread per `connectCompanion`). Memanggil
  liblog di child setelah `fork()` berisiko **deadlock** karena mutex liblog dapat
  terwarisi dalam keadaan terkunci (fork tak menyalin thread pemegang lock). VLA
  berukuran non-konstan juga non-standar padahal jumlah bind entry compile-time
  constant.
- **Bonus:** errno per-mount (`EPERM`=SELinux/caps, `EINVAL`=flags, `ENOENT`=target
  tak ada) kini di-log parent untuk memudahkan diagnosa bind gagal.

### Changed — single source of truth (`config.hpp`)

- **Apa:** Enum `Cmd`, path, tabel `BindEntry`, dan fallback properti dipindah ke
  `config.hpp`; duplikatnya di `main.cpp`/`companion.cpp` dihapus. Ditambah path
  `IDENTITY_BAK`/`MODE_FILE`/`RESETPROP` dan array `MOUNT_PARTS[]` yang dipakai CLI.
- **Kenapa:** Sebelumnya `main.cpp` mendaftar 6 bind entry sementara `companion.cpp`
  9 — **drift** yang membuat overlay tidak konsisten. Satu sumber menghapus kelas bug
  ini dan membuat data auditable di satu tempat.

### Fixed — identity.prop kosong saat spawn (race dengan `seed`)

- **Apa:** `CMD_GET_IDENTITY` di companion kini me-retry baca `identity.prop` 3×100ms
  bila kosong (file sedang mid-replace / seed telat), lalu `LOGE` pesan actionable
  ("run `sandboxid seed`/`freshen`"). Balasan kosong tetap fail-safe (app dianggap
  non-target, tidak crash).
- **Kenapa:** `identity.prop` seharusnya sudah ada sebelum zygote (post-fs-data
  `seed`), tapi jika seed gagal/telat app menerima blob kosong tanpa jejak. Retry +
  LOGE menutup jendela race kecil dan membuat kegagalan terlihat di logcat.

---

## v1.0.19 (2026-07-27)

### Action button is now 1-tap ready
- `action.sh` now runs **`bin/sandboxid freshen` → `rotate_ids.sh all`** in sequence.
- Before v1.0.19 you had to `sh rotate_ids.sh all` manually after tapping Action. That step is gone — one tap in KernelSU/Magisk = fresh persona applied end-to-end.

### New: `rotate_ids.sh` + `helpers.sh`
- **helpers.sh** — shared shell library: `log_*`, refcounted `se_permissive`/`se_restore`, `get_users`, `generate_uuid`, `generate_mac`, `settings_put`, `rp_set`, `force_stop`, `identity_get`, `identity_persist`, `backup_rotate`.
- **rotate_ids.sh** — CLI dispatcher with `all` / `safe` / `ssaid` / `gaid` / `wlan-mac` / `bt-mac` / `device-name` / `status` / `help`.

### Design fix: `device_name` now consistent with hook persona
- Old `randomize_device_name` picked a random Brand+Model from a hardcoded list (`"Galaxy S24-427"`). That name clashed with the persona chosen by the L1/L2 hooks (which reads `MODEL` from `identity.prop` — e.g. `Pixel 8`).
- New `sync_device_name` reads `MODEL` from `identity.prop` and applies **that same value** to `settings put global device_name`, `settings put global bluetooth_name`, `persist.bluetooth.adaptername`, and rewrites `Name = ` in `bt_config.conf`. Hook layer and shell layer now report identical values.
- Optional override: `BLUETOOTH_NAME` key in `identity.prop` (if set, wins over `MODEL`).

### New: Bluetooth MAC rotation
- `rotate_bluetooth_mac()` writes `persist.service.bdroid.bdaddr`, `persist.sys.bt.bdaddr`, `persist.bluetooth.bdaddr`, `bluetooth.device.mac.address`, `ro.boot.btmacaddr`.
- Rewrites `Address = ` line in `/data/misc/{bluedroid,bluetooth}/bt_config.conf` and `/data/vendor/bluetooth/bt_config.conf`. If missing, injects under `[Adapter]` section.
- MAC persisted to `identity.prop` as `BLUETOOTH_ADDR` for reboot-idempotent state.

### Identity persistence
- `rotate_ids.sh` now writes `WIFI_MAC`, `BLUETOOTH_ADDR`, `BLUETOOTH_NAME` back into `identity.prop` via `identity_persist()` (atomic awk upsert). Next `freshen` preserves them; next `rotate all` reads them back — same value across reboots until an explicit re-rotate.

### Backup rotation
- All destructive ops (`wipe_ssaid`, `set_gaid_value`, `randomize_wlan_mac`, `rotate_bluetooth_mac`, `sync_device_name`) copy the target file into `$MODDIR/backups/` first.
- `backup_rotate PREFIX KEEP` prunes older files, keeping the newest N (defaults: SSAID/BT-config = 10, WifiConfigStore = 5).

### GAID hardening
- Now reads `GOOGLE_AID` from `identity.prop` (populated by `freshen`) instead of always regenerating. If missing, generates + persists.
- Guards against GMS not installed — writes to `Settings.Global` and returns without touching the shared_prefs dir.
- Optional `chcon` re-labels `adid_settings.xml` from parent directory context if `chcon` is available.

### Full flow (1-tap)
1. `bin/sandboxid freshen` — rolls `MODEL`, `DEVICE`, `BRAND`, `SERIAL`, `ANDROID_ID`, `GOOGLE_AID`, etc.
2. `wipe_ssaid` — deletes `settings_ssaid.xml` per user (needs reboot to regenerate).
3. `set_gaid_value` — syncs `GOOGLE_AID` to `Settings.Global.advertising_id` + GMS `adid_settings.xml`.
4. `randomize_wlan_mac` — wlan0 MAC + wipes `WifiConfigStore.xml`.
5. `rotate_bluetooth_mac` — BT adapter MAC + `bt_config.conf` Address.
6. `sync_device_name` — device_name/bluetooth_name/`bt_config.conf` Name = `identity.prop` MODEL.

---

# Changelog

All notable changes to SandboxID are recorded here. The GitHub Actions workflow
reads the matching `## vX.Y.Z` section to build `release_notes.md` automatically
on every release.

Format loosely follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
and [Semantic Versioning](https://semver.org/).

---

## v1.0.18

### Added

- **L2 native_get static defaults map** — 13 new keys that leaked in real-world sessions now return safe generic values instead of the device's real value: `gsm.operator.isroaming` ("false"), `ro.zygote` ("zygote64_32"), `ro.hardware` ("qcom"), `ro.board.platform` ("sm8250"), `ro.dalvik.vm.native.bridge` ("0"), `ro.allow.mock.location` ("0"), `dalvik.vm.isa.arm64.variant` / `.features`, `dalvik.vm.isa.arm.variant` / `.features`, `dalvik.vm.heapsize` ("512m"), `ro.build.version.preview_sdk` ("0"), `persist.radio.multisim.config` fallback ("ss"). Logged as `L2 SPOOF-STATIC` so they can be counted separately.
- **L2 identity-typed additions**: `ro.build.user`, `ro.build.host`, `ro.build.tags`, `ro.build.type` now hook via native_get (previously only spoofed via resetprop on `apply-boot`).
- **L7-SPB additions** (3 keys, from real leak trace): `persist.sys.activity_anim_perf_override` (was leaking 114× per session), `persist.sys.lmk.reportkills`, `debug.layout`.
- **L7-SPI addition**: `debug.adservices.binder_timeout` = 10000.

### Fixed

- **`summarize.sh` “Target packages seen” section was blank** even when the companion accepted target spawns. Root cause: the regex captured `pkg=` from every log line including REJECTs, then sort/uniq buried target matches. Now split into two sections: **“Target packages seen (ACCEPTED by companion)”** parsed from `ACCEPT pkg='...'` lines only, and **“All packages spawned (top 20, incl. rejected)”** for whitelist tuning context.

### Unchanged

- Companion bind-mount errno=2 seen on some post-`freshen` spawns is under investigation (write path is already atomic via `rename(2)`; likely mount(2) fails post-setns because target NS has an existing overlay). Added no runtime change in v1.0.18; richer per-mount diagnostic planned for v1.0.19.

### Impact

- Expected LEAK count in next debug session: ~5 (from 182 in v1.0.17). Remaining leaks: rare device-specific props not yet catalogued.

---

## v1.0.17

### Changed

- Migrated repository from `diru768/sandboxid` to `Ilham311/sandboxid`. All README badges, install links, `git clone` URL, `module.prop` `updateJson`, and `update.json` seed now point to the new repo.
- `LICENSE` copyright reassigned to `Ilham311` (MIT).

### Fixed

- Placeholder text like `<ts>`, `<part>`, `<timestamp>` were being silently stripped by Markdown / web upload paths, leaving broken filenames such as `session-.log` in README, CHANGELOG, and `customize.sh`. All placeholders replaced with concrete stripping-proof forms: `session-YYYYMMDD-HHMMSS.log`, `summary-YYYYMMDD-HHMMSS.txt`, `/{partition}/etc/build.prop`.
- `module.prop` author reassigned from `diru768` to `Ilham311`.
- `customize.sh` install banner bumped from v1.0.15 to v1.0.17 (was 2 versions stale).

### Unchanged

- No functional / runtime behavior changes. Zygisk hook layers, companion IPC protocol, `target.txt` whitelist, crash watchdog, and summarizer all identical to v1.0.16. Safe to flash over v1.0.16 without state reset.

---

## v1.0.16

### Changed

- All source files (`.cpp`, `.hpp`, `.sh`, `CMakeLists.txt`, workflow YAML) stripped of comments for cleaner distribution. Total source size reduced ~22%.
- README fully rewritten with centered header, 5 status badges, table of contents, ASCII architecture diagram (boot flow + per-app-spawn flow), Requirements table, Command reference table, Env override table, and dedicated **Auto-release pipeline** section.
- New `CHANGELOG.md` (this file) added as canonical release history in Keep-a-Changelog format.

### Added

- `.github/workflows/build.yml` fully automated: reads `module.prop` → auto-bumps patch on tag collision → builds both variants → auto-generates `release_notes.md` from `CHANGELOG.md` section + git log since prev tag → auto-generates `update.json` from `module.prop` + repo slug → commits refreshed `update.json` + synced `module.prop` back to `main` → publishes GitHub Release. **Zero manual input.**
- `release_notes.md` and `update.json` are now build artifacts / release assets, no longer committed manually.

---

## v1.0.15

### Added

- **Runtime whitelist `target.txt`** at `/data/adb/modules/sandboxid/target.txt`. Add / remove target packages without rebuilding.
- Companion loads and **hot-reloads** `target.txt` on mtime change (next app spawn picks up edits).
- New CLI subcommand `sandboxid targets` to dump the active whitelist.
- L7 `SUPPRESS` label for known log-noise keys (`log.looper.*.slow`, `debug.watson.*`) to keep summaries readable.
- `summarize.sh` now breaks SPOOF hits out by hook layer (`L1` / `L2` / `L7-SPB` / `L7-SPI` / `L7-SPL`) and counts `SUPPRESS`, `REJECT`, `ACCEPT` separately.
- `customize.sh` **preserves** existing `target.txt` across reinstalls.

### Changed

- Zygisk companion IPC protocol for `CMD_GET_IDENTITY` now includes the pkg name; companion responds with `len=0` for non-targets (single source of truth for whitelist).
- Zygisk `.so` no longer contains a hardcoded target list.
- `sandboxid.cpp` `wipe_target_data()` reads targets from `target.txt` for symmetry with the Zygisk side.

### Fixed

- Whitelist drift between the CLI (`sandboxid`) and Zygisk companion — both now share one file.

---

## v1.0.14

### Added

- `post-fs-data.sh` + new `sandboxid seed` subcommand that generates identity + mount overlay files **before** Zygisk loads, fixing the first-boot race where the first TT/Grab pid got 0/6 bind mounts.
- Android 11+ canonical partition paths in `BIND_ENTRIES` (`/odm/etc/build.prop`, `/product/etc/build.prop`, `/system_ext/etc/build.prop`) alongside the legacy paths.
- Skip counter is split into `skip_src` (module bug) vs `skip_dst` (device doesn't have that partition — expected).

### Fixed

- 3-skip on POCO F3 / MIUI-style ROMs where partition build.prop lives at `/{partition}/etc/build.prop`.

---

## v1.0.13

### Added

- **Per-type L7 spoof maps** (`sbx_bool_spoof`, `sbx_int_spoof`, `sbx_long_spoof`) consulted by the typed `native_get_*` hooks before falling back to `def`.
- Critical spoof: `sys.boot_completed = true` (previously returned `false`, breaking app boot-detection retry loops).
- L2 `debug.force_rtl` → `false`.

### Fixed

- 800+ hot L7 leaks from v1.0.12 telemetry (`sys.boot_completed`, `debug.sqlite.*`, `build.version.extensions.*`, `ro.gfx.driver_build_time`, `dalvik.vm.dexopt.secondary`, etc.).

---

## v1.0.12

### Added

- Crash watchdog rewrite: 4 signals (`SIGABRT` / `SIGFPE` / `SIGILL` / `SIGSYS`), rate-limited 3 per signal, restores `SIG_DFL` (no signal chaining).
- 18 additional L2 `native_get` spoofs (`gsm.*`, `sys.boot_completed`, `cpu.abi*`, `dalvik.vm.heapgrowthlimit`, `ro.build.characteristics`, `persist.sys.timezone`, `ro.mediacodec.*`).

### Fixed

- YAML workflow version resolution (was overwriting `module.prop` with git-describe fallback `v0.0.6`).
- `summarize.sh` MOUNT regex missing the new companion log format.

---

## v1.0.11

### Added

- Standalone `summarize.sh` that condenses a 7 MB session log into a ~10 KB chat-shareable summary.
- Action tap on debug variant now auto-produces `summary-YYYYMMDD-HHMMSS.txt`, copies `crashes.log`, and gzips the raw log to `/sdcard/Download/sandboxid-logs/`.
- Automatic pruning: keeps newest 10 summaries / crashes / raw.gz per install.

---

## v1.0.10

### Added

- **Zero-setup auto-log capture** on debug variant. `service.sh` starts a background logcat on boot into `/data/adb/modules/sandboxid/debug/session-YYYYMMDD-HHMMSS.log`, keeping the 5 newest sessions.
- Session header written to each log (module version, boot time, uptime, Android SDK, device, ABI, installed root modules).

---

## v1.0.9

### Added

- Detailed `[D]` traces for every hook layer (which key was queried, what was returned, LEAK vs SPOOF vs MISS labelling).
- Companion mount timeline traces (`setns OK`, per-bind check, ok/fail/skip breakdown).
- Crash / death / leak journal that persists across reboots.

---

## v1.0.8

### Added

- Debug variant build alongside release. Both variants are produced by `build.sh` per invocation.
- `SBX_DEBUG` compile-time flag: release strips `LOGD` calls entirely (zero cost), debug keeps them.

---

## v1.0.7

### Added

- Expanded target set to include Grab Passenger (`com.grabtaxi.passenger`) — driver/passenger apps do heavy device fingerprinting for fraud.

---

## v1.0.6

### Fixed

- Bind-mount `EINVAL` in the Zygisk-Next companion. Companion now forks a single-threaded child that `setns`es into the target's mount namespace before mounting.

---

## v1.0.5

### Added

- Zygisk companion process serving `CMD_GET_IDENTITY` and `CMD_DO_MOUNTS` over the built-in Zygisk UDS.

---

## v1.0.4

### Fixed

- `EACCES` on `mount()` from `preAppSpecialize` (CAP_SYS_ADMIN dropped). Mounting is now handled by the companion, which retains caps.

---

## v1.0.3

### Added

- Bind-mount overlay tree at `$MODPATH/mount/{system,vendor,odm,product,system_ext}/build.prop` + `settings_secure.xml`.
- `sandboxid freshen` regenerates all overlay files.

---

## v1.0.2

### Added

- `resetprop-rs` invocation from `apply-boot` and `freshen` to broadcast native property changes.

---

## v1.0.1

### Added

- Standalone CLI (`sandboxid`) with `freshen`, `status`, `rollback`, `lock`, `unlock`, `apply-boot` subcommands.

---

## v1.0.0

Initial release.

- 6-layer Java hook: `Build.*`, `SystemProperties.native_get`, `Settings.Secure.getString`, `AdvertisingIdClient.Info.getId` (stub), `WifiInfo.getMacAddress` / `getBSSID`, `TelephonyManager.getImei` / `getDeviceId` / `getSubscriberId` / `getMeid`.
- Pixel-only device pool (SDK 33–36).
- TikTok Global / Asia / Lite target packages.
