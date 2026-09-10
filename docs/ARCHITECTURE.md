# Arsitektur SandboxID

Dokumen ini menjelaskan cara kerja SandboxID: bagaimana berkas ditata di
repositori, mengapa layout terpasang berbeda dengan layout repo, ketiga
komponen yang saling bekerja sama, dan alur data dari satu tap tombol
**Action** sampai identitas perangkat siap dipakai.

> Ringkas: SandboxID adalah modul Magisk/KernelSU/APatch untuk riset privasi
> identifier Android. Ia menyatukan **tiga lapisan** yang selalu melaporkan
> nilai yang sama untuk satu persona: hook Zygisk pra-zygote, proses companion
> ber-root, dan lapisan CLI/shell.

---

## 1. Struktur repositori

Sejak refactor, berkas ditata per-fungsi agar mudah dipahami — root repo tidak
lagi menumpuk puluhan berkas:

```
SandboxID/
├── build.sh              # Orkestrator build: kompilasi .so per-ABI + kemas ZIP
├── module.prop           # Manifest modul (dibaca framework & CI) — WAJIB di root
├── update.json           # Metadata update OTA — WAJIB di root
├── jni/                  # Sumber C++ (Zygisk + companion + CLI) & header
│   ├── main.cpp          # Modul Zygisk (hook Build.*/SystemProperties/native read)
│   ├── companion.cpp     # Daemon ber-root (identity blob, bind-mount, hide)
│   ├── sandboxid.cpp     # Biner CLI (freshen/apply-boot/status/…)
│   ├── config.hpp        # Konstanta path + protokol soket (KONTRAK bersama)
│   ├── sbx_*.hpp         # Helper header (native_read, carrier, mountinfo, lsplant)
│   └── zygisk.hpp        # Header Zygisk API — VENDOR, jangan diubah
├── scripts/
│   ├── lifecycle/        # Dipanggil framework: customize, post-fs-data, service, action
│   ├── lib/              # helpers.sh — pustaka shell bersama
│   ├── identity/         # rotate_ids.sh (rotasi ID) + autopif.sh (pool persona)
│   └── debug/            # summarize.sh, selftest.sh
├── data/                 # Data referensi: personas/devices/carriers.tsv, target.txt
├── webroot/              # WebUI (KernelSU/APatch)
├── prebuilt/             # Biner prebuilt (resetprop-rs) + checksum
├── tests/                # Unit test host (carrier_test, native_read_test)
├── tools/                # validate.sh — cermin CI lokal
└── docs/                 # Dokumen ini
```

---

## 2. Repo ≠ layout terpasang (mekanisme "flatten")

Framework Magisk/KSU/APatch mengharapkan **semua berkas modul berada DATAR di
root modul terpasang** (`/data/adb/modules/sandboxid/`): `module.prop`,
`customize.sh`, `service.sh`, `action.sh`, `personas.tsv`, `zygisk/<abi>.so`,
`bin/`, dst. Path absolut di `jni/config.hpp` (mis. `IDENTITY_FILE`,
`TARGET_FILE`, `PERSONAS_FILE`) dan referensi `$MODDIR/<nama>` di skrip shell
juga menganggap layout datar itu.

Karena itu **repo boleh rapi berfolder, sementara `build.sh` "meratakan"
kembali** seluruh berkas terpilih ke root paket (`$PKG/`) saat build:

```
repo (berfolder)                     paket terpasang (datar)
scripts/lifecycle/service.sh   ──►   /data/adb/modules/sandboxid/service.sh
scripts/identity/rotate_ids.sh ──►   /data/adb/modules/sandboxid/rotate_ids.sh
scripts/lib/helpers.sh         ──►   /data/adb/modules/sandboxid/helpers.sh
data/personas.tsv              ──►   /data/adb/modules/sandboxid/personas.tsv
jni/<abi>/libsandboxid.so      ──►   /data/adb/modules/sandboxid/zygisk/<abi>.so
```

Konsekuensi penting: **perilaku runtime identik** dengan sebelum refactor —
tidak ada path runtime yang berubah. Yang berubah hanya tata letak sumber.
Kalau menambah berkas baru yang harus ikut terpasang, tambahkan barisnya di
blok pengemasan `build.sh` (fungsi `build_variant`).

---

## 3. Tiga komponen & alur data (runtime)

```
        ┌─────────────────────────────────────────────────────────────┐
        │                        zygote (Android)                      │
        │   memuat libsandboxid.so ke SETIAP proses aplikasi           │
        └───────────────┬─────────────────────────────────────────────┘
                        │ preAppSpecialize / postAppSpecialize
                        ▼
   ┌───────────────────────────────┐   soket Zygisk    ┌────────────────────────┐
   │  (1) Modul Zygisk  main.cpp   │◄─────────────────►│ (2) Companion (root)   │
   │  - connectCompanion()         │  CMD_GET_IDENTITY │     companion.cpp      │
   │  - parse identity blob        │  CMD_DO_MOUNTS    │  - is_target()         │
   │  - hook Build.*/SystemProps   │  CMD_DO_HIDE      │  - baca identity.prop  │
   │  - hook native read (L9)      │                   │  - bind-mount build.prop│
   └───────────────────────────────┘                   │  - sembunyikan jejak   │
                        ▲                               └───────────┬────────────┘
                        │ membaca file yang sama                    │ membaca
                        │                                           ▼
   ┌────────────────────┴───────────────────────────────────────────────────────┐
   │  (3) Lapisan CLI/shell — biner `sandboxid` (sandboxid.cpp) + skrip .sh       │
   │  menulis identity.prop, personas.tsv, target.txt, carrier.conf, dll.         │
   └──────────────────────────────────────────────────────────────────────────────┘
```

**(1) Modul Zygisk — `jni/main.cpp`.** Dimuat ke tiap proses aplikasi sebelum
spesialisasi. Di `preAppSpecialize` ia menanyakan companion apakah paket ini
target (`CMD_GET_IDENTITY` + nama paket) dan menerima *identity blob*
(daftar `KEY=VALUE`). Jika bukan target (balasan panjang 0) modul melepas diri
(`unload`). Jika target, di `postAppSpecialize` blob diurai ke `g_identity` lalu
dipasang berlapis: field Java `Build.*`, `SystemProperties.native_get*` (L2/L7),
`clock_gettime` uptime (L8), dan pembaca native libc
`__system_property_get/read/read_callback` + `open/openat/fopen` (L9). Terakhir
ia meminta companion melakukan bind-mount (`CMD_DO_MOUNTS`) dan opsional hide
(`CMD_DO_HIDE`).

**(2) Companion ber-root — `jni/companion.cpp`.** Entry `sandboxid_companion()`
melayani soket dari modul Zygisk. `CMD_GET_IDENTITY`: cek `is_target()` terhadap
`target.txt`, baca `identity.prop` (dengan *seed on-demand* bila kosong), lalu
kirim blob. `CMD_DO_MOUNTS`: fork ke mount-namespace pemanggil dan bind-mount
pohon `build.prop` sintetis sehingga pembaca berbasis file melihat nilai yang
konsisten. `CMD_DO_HIDE`: lepaskan jejak mount/akar.

**(3) Lapisan CLI/shell — `jni/sandboxid.cpp` + `scripts/`.** Biner `sandboxid`
menghasilkan persona (`pick_persona`/`derive_identity`/`gen_identity`),
menerapkan properti native (`apply_native`), dan membuat berkas mount
(`generate_mount_files`). Skrip shell mengorkestrasi biner ini dan menyinkronkan
ID lain (SSAID, GAID, MAC WiFi/BT, nama perangkat) via `rotate_ids.sh`.

### Protokol soket (didefinisikan di `jni/config.hpp` — KONTRAK)

| Perintah | Nilai | Arah | Muatan |
|----------|-------|------|--------|
| `CMD_GET_IDENTITY` | 2 | Zygisk → companion | `u16` panjang paket + nama paket → balasan `u32` panjang blob + blob |
| `CMD_DO_MOUNTS`    | 3 | Zygisk → companion | `u32` pid → balasan `u32` jumlah mount |
| `CMD_DO_HIDE`      | 4 | Zygisk → companion | `u32` pid → balasan `u32` jumlah dilepas |

Batas blob identitas: `MAX_IDENTITY_BLOB = 64 KiB`.

---

## 4. Alur 1-CLICK ACTION → identitas siap pakai

Saat pengguna menekan tombol **Action** di manajer modul, framework menjalankan
`action.sh` (di root modul terpasang). Urutannya:

1. **Acak perangkat** — `autopif.sh device` memilih persona perangkat baru dan
   menulis `device.identity`.
2. **Terapkan** — salin `device.identity` → `identity.prop` (backup dulu),
   `sandboxid unlock` → `sandboxid apply-boot` (terapkan properti native) →
   `sandboxid lock`. Bila gagal, jatuh ke cadangan `sandboxid freshen`
   (persona Pixel bawaan).
3. **Reset aplikasi target** — untuk tiap baris di `target.txt`: `force-stop` +
   `pm clear` agar aplikasi membaca ulang identitas baru.
4. **Rotasi ID lain** — `rotate_ids.sh all` menyegarkan SSAID, GAID, MAC
   WiFi/BT, nama perangkat, boot count, dan AppLog.
5. **Ringkasan** — tampilkan persona aktif (BRAND/MODEL/DEVICE/RELEASE/…) dan,
   pada varian debug, kumpulkan artefak diagnostik.

Setelah langkah ini, membuka ulang aplikasi target sudah menampilkan identitas
perangkat yang baru — inilah kontrak **1 CLICK ACTION → PROFIL/IDENTITAS SIAP
PAKAI** yang harus dijaga tetap utuh.

---

## 5. Tanggung jawab per-berkas

| Berkas | Peran |
|--------|-------|
| `jni/main.cpp` | Modul Zygisk: hook Build.*, SystemProperties, uptime, native read |
| `jni/companion.cpp` | Daemon root: identity blob, bind-mount, hide, seed on-demand |
| `jni/sandboxid.cpp` | CLI: freshen/apply-boot/status/lock/unlock/seed/targets/applog-ids |
| `jni/config.hpp` | Konstanta path & protokol soket (kontrak bersama tiga komponen) |
| `scripts/lifecycle/customize.sh` | Dipanggil saat instalasi (set izin, pertahankan config lama) |
| `scripts/lifecycle/service.sh` | Dipanggil saat boot (re-apply properti native) |
| `scripts/lifecycle/post-fs-data.sh` | Tahap awal boot |
| `scripts/lifecycle/action.sh` | Tombol Action: alur 1-klik di atas |
| `scripts/lib/helpers.sh` | Pustaka shell bersama (resolusi biner, identity_get/persist, dll.) |
| `scripts/identity/rotate_ids.sh` | Rotasi SSAID/GAID/MAC/nama/AppLog |
| `scripts/identity/autopif.sh` | Segarkan/pilih pool persona perangkat |
| `scripts/debug/{summarize,selftest}.sh` | Diagnostik |
| `data/*.tsv`, `data/target.txt` | Data referensi & daftar aplikasi target |
| `build.sh` | Kompilasi per-ABI + kemas (flatten) ke ZIP |
| `tools/validate.sh` | Cermin CI lokal (syntax, unit test, shellcheck) |

---

## 6. Kontrak yang TIDAK boleh diubah

Agar refactor tetap aman, hal-hal berikut adalah kontrak eksternal — mengubahnya
memutus kompatibilitas atau runtime:

- **String key properti Android** (`ro.build.fingerprint`, dll.) dan **key
  identitas** (`MODEL`, `BRAND`, `FINGERPRINT`, …).
- **Nama/kelas/signature JNI** (`android/os/Build`, `native_get`, …) dan **nama
  simbol libc** yang di-hook (`__system_property_get`, `openat`, …).
- **String perintah CLI** (`freshen`, `apply-boot`, `seed`, `lock`, `unlock`,
  `status`, `rollback`, `targets`, `applog-ids`).
- **Simbol yang diekspor**: `sandboxid_companion`, entry `REGISTER_ZYGISK_*`.
- **Konstanta path di `jni/config.hpp`** (semua absolut ke root modul datar).
- **`jni/zygisk.hpp`** — vendor upstream, checksum-pinned di `build.sh`.

Penamaan **internal** (variabel/fungsi lokal satu berkas) bebas diperjelas —
mis. hook native di `main.cpp` kini bernama `sbx_sysprop_get/read/read_cb`
(dulu `sbx_spg/spr/sprcb`).
