# SandboxID TT — varian LSPosed (standalone APK, no root)

> **Status: belum pernah di-build.** Varian ini ditulis lengkap tapi **belum
> pernah dikompilasi, di-build, atau diuji** di perangkat manapun. Mesin yang
> menulisnya tidak punya Gradle, Android SDK, `aapt2`, `d8`, atau `apksigner` —
> hanya toolchain host C/C++. Setiap file di `lsp/` menyebut ini di header-nya.
> **Jangan anggap ini bekerja sebelum di-build dan diuji.**

## Apa ini

Versi **LSPosed murni** dari SandboxID TT: satu APK biasa, dipasang ke perangkat,
dimuat ke dalam proses TikTok oleh LSPosed. **Mandiri — tidak butuh root, tidak
butuh modul Zygisk terpasang, tidak membaca apapun dari `/data/adb`.**

Persona dihasilkan di dalam APK ini sendiri, disimpan lewat mekanisme bawaan
LSPosed, dan dibaca oleh hook di proses TikTok. Lihat
[Cara kerjanya](#cara-kerjanya) di bawah.

> **Revisi desain (2026-10-06).** Revisi sebelumnya membaca persona dari path
> yang **harus dipublikasikan oleh sisi root modul Zygisk** — membuat varian ini
> cuma tambahan yang tak bisa berdiri sendiri, bukan modul mandiri. Itu salah
> terhadap permintaan asli. Semua dependensi itu sudah dihapus: generator persona
> sekarang ada di dalam APK ini (`PersonaGenerator.java`).

## Cara memakainya

1. Build & pasang APK (lihat [Cara build](#cara-build)).
2. Buka **SandboxID TT** dari launcher (ini app biasa dengan UI sendiri).
3. Tekan **"Generate new device persona"**.
4. Buka LSPosed Manager, aktifkan modul, pastikan scope-nya TikTok
   (`xposedscope` sudah diisi `com.zhiliaoapp.musically` di manifest).
5. **Kill TikTok** (swipe dari recent apps) lalu buka lagi. Sekarang membaca
   persona baru.

**Kenapa harus dibunuh dulu?** Hook jalan saat proses mulai; proses yang sudah
jalan sudah punya nilai lama. Varian root bisa `pm clear` otomatis; tanpa root
tidak bisa, jadi langkah 5 adalah pengguna yang melakukan-nya. UI sudah
menjelaskan ini di tombolnya sendiri.

## Cara kerjanya

Mekanisme berbagi data adalah **bawaan LSPosed**, bukan hack world-readable:

```
┌──────────────────────────────┐        ┌──────────────────────────────┐
│  APK SandboxID TT (UID kita) │        │  Proses TikTok (UID TikTok)   │
│                              │        │                               │
│  MainActivity                │        │  SandboxIdTT.handleLoadPackage│
│    │ PersonaGenerator        │        │    │ IdentityStore.load()     │
│    │ (devices.tsv di asset)  │        │    │   XSharedPreferences     │
│    ▼                         │        │    ▼                         │
│  SharedPreferences           │        │  persona map                 │
│    "sandboxid_persona"       │        │    │                         │
│    │                         │        │    ├── BuildHook             │
│    ▼  (LSPosed redirect)     │        │    ├── PropHook              │
│  /data/misc/<uuid>/prefs/    │◀───────┤    ├── ClockHook             │
│    id.sandboxid.tt/          │  read  │    ├── TelephonyDrmHook      │
│    (rwx--x--x, UID kita)     │        │    ├── KevaHook             │
│                             │        │    └── RegisterHook         │
└──────────────────────────────┘        └──────────────────────────────┘
```

Rute ini diverifikasi dari source LSPosed sendiri:
`XSharedPreferences` memakai meta-data **`xposedsharedprefs`** untuk resolve ke
`serviceClient.getPrefsPath(packageName)` → `ConfigManager.getPrefsPath()`
(`LSPosed/daemon/.../ConfigManager.java:1095`), yang bikin direktori
`misc/prefs/<pkg>/` mode **`rwx--x--x``** lalu `chown` ke **UID app kita**
(`:1101-1109`). Jadi:

- module menulis prefs-nya sendiri seperti app biasa,
- target membaca dengan UID kita — **tidak perlu world-readable**,
- `XSharedPreferences.edit()` sengaja melempar (read-only di sisi target),
  karena target tidak boleh mengubah persona.

Ini menggantikan solusi lama (`/data/local/tmp/sandboxid/identity.prop` yang
world-readable dan harus ditulis `action.sh` pihak root) sepenuhnya.

## Generator persona (mandiri)

`PersonaGenerator.java` menggantikan pasangan `autopif.sh device` +
`sandboxid freshen` di sisi root. Satu pool, satu rumus — bukan tiga generator
yang seperti di sisi root saling berbeda.

| Sumber | Dipakai? | Alasan |
|---|---|---|
| `devices.tsv` (40 device, 8 brand) | **Ya**, dibundle sebagai asset | 15 kolom; bawa brand/SoC/marketname/release_date |
| `personas.tsv` (16 Pixel) | Tidak | subset devices.tsv dengan urutan kolom beda — memakai keduanya adalah sumber bug yang dikomentari sendiri di `autopif.sh:314-318` |
| Carrier | Default `config.hpp` (51010/Telkomsel/id/787) | Tidak ada `carrier.conf` tanpa root; UI picker bisa ditambah nanti |

**Setiap key yang rumusnya berbeda antar shell dan native, sisi native yang
dipilih** (kecuali dicaftat), karena hook runtime ditulis melawannya:

- `VBMETA_DIGEST` — deterministik `hex_from_seed(fnv1a(FINGERPRINT|SERIAL), 32)`.
  Shell memakai random hex, yang **memutuskan semua ID turunan dari seed-nya**
  (IMEI/IMSI/ICCID/MEID/Widevine + semua AppLog ID). Jangan pernah random di sini.
- `BUILD_TIME_UTC` — termasuk `+ digitSum % 37` detik; shell tidak.
- `SERIAL` — uppercase (sesuai `Build.getSerial()` hardware asli).
- `USER`/`HOST`/`RADIO` — nilai native (build farm, bukan `<brand>-build-N`).
- `MARKETNAME` — fallback ke MODEL bila kolom kosong.

**Key lifecycle** (`BOOT_COUNT`, `AGE_DAYS`, `UPTIME_*`…) dipasang dari
`gen_lifecycle` — native tidak punya equivalennya. `UPTIME_SECONDS` default **0**
(sama seperti module: opt-in, dipaksa 0 per spawn bila tidak diaktifkan).

## Cakupan vs modul Zygisk

**Hanya di varian LSPosed** (alasan utama varian ini ada):

| Permukaan | Mengapa Zygisk tidak capai |
|---|---|
| `TelephonyManager.getDeviceId/getImei/getMeid/getSubscriberId/getSimSerialNumber` | Diberikan via Binder oleh telephony service |
| `getNetworkOperator[Name]`/`getSimOperator[Name]`/`getCountryIso` | Sama |
| `MediaDrm.getPropertyByteArray("deviceUniqueId")` | Widevine L1 ID |
| `WifiInfo.getMacAddress` / `BluetoothAdapter.getAddress` | Dari service |
| `Settings.Secure.getString("android_id")` | ContentProvider |
### Register: dapat did baru (TERVERIFIKASI di perangkat)

Ini inti varian ini, dan ini yang membedakannya dari sekadar mengubah
`Build.*`. `RegisterHook` + `Rewrite` menyadap permintaan
`/service/2/device_register/` di tingkat *wire* (`X.087U.post`) dan
menulis ulang body-nya, sehingga server **mencetak did baru**, bukan
mengembalikan did yang sudah terikat.

**Hasil di perangkat** (TikTok 47.1.4, probe langsung di prosesnya):

```
request : "device_id":0, "openudid":<persona>, "clientudid":<persona>, "cdid":<persona>
response: {"device_id":7693600027890189831,"install_id":...,"new_user":1}
```

Tiga launch berturut-turut → tiga did berbeda; did lama
`7691364758716679700` muncul **nol kali** di seluruh log.

Yang harus berjalan semuanya (ditemukan eksperimen, bukan tebakan):

1. Body tidak boleh membawa did yang pernah diregister
   (`device_id`/`install_id`/`bd_did` → `0`).
2. `openudid`/`clientudid`/`cdid` harus nilai yang **belum pernah** dilihat
   server. Nilai yang sudah diregister sekali terikat selamanya ke did itu —
   respons hanya akan mengulangnya. Persona generator menurunkan ketiganya dari
   seed, jadi Generate = set nilai baru.
3. Register harus boleh jalan lagi. Ada gate `dr_aid`/`dr_channel`/
   `dr_install_vc` di repo KEVA `ug_install_settings_pref`; kalau ketiganya
   cocok, aplikasi memutar-ulang respons yang di-cache alih-alih memanggil
   jaringan — yang terlihat persis seperti "server kenal device ini", padahal
   murni lokal. Restart proses cukup; tapi rotasi persona yang tidak mengubah
   ketiganya akan no-op diam-diam.

**Yang terbukti BUKAN pemicu** — jangan diteliti ulang: `sig_hash` (MD5
certificate APK, identik untuk semua install TikTok),
`apk_first_install_time` (di-spoof saja, hasil sama), GAID (null di device
ini), query string URL (register hanya bawa `?req_id=`).

### AppLog / KEVA

AppLog lama (`AppLogHook`, menyadap `applog.xml` lewat
`SharedPreferencesImpl`) sudah **dihapus**: file itu tidak ada di TikTok
47.1.4 — hook-nya menyadap store yang tidak pernah dibaca. Diganti
`KevaHook`, yang menyadap pembaca KEVA yang benar
(`KevaSpFastAdapter` + `X.02tJ`, keduanya live).

**Yang masih tidak terjangkau** tanpa akses file native:
`files/bd_setting/{device_id,install_id,...}` dan `files/.cdid` (dibaca native
`libbdtracker.so`), serta store MMKV. Ini perbedaan nyata terhadap Zygisk.

**Hanya di modul Zygisk:**

| Permukaan | Mengapa |
|---|---|
| `/proc` `/sys` (`cpuinfo`, `meminfo`, `version`, boot_id) | Redirect GOT libc — Xposed tidak punya API-nya |
| Raw file AppLog (`bd_setting/*`, `.cdid`) | Dibaca native |
| Hook native `__system_property_*` | `.so` app sendiri lewat Java sama sekali |
| Mount-hide, `pm clear` otomatis | Butuh root |

**Jejak tambahan:** `XposedBridge.class` **wajib** resolve di proses target.
Modul Zygisk membuat **nol** class Java di proses app — varian ini murni
**menambah** permukaan terdeteksi (cek classloader adalah pola anti-hook umum).

**Timing:** `handleLoadPackage` jalan **setelah** class load, jadi `static final`
sudah membekukan nilai asli. Modul Zygisk menambal di `postAppSpecialize`
sebelum class-init. Celah pertama yang harus diuji per-target.

## Cara build

```sh
cd lsp
./gradlew assembleRelease       # butuh Android SDK + JDK
# sign APK-nya, install seperti app biasa
```

Butuh: Android SDK (compileSdk 34), JDK 8+, Gradle (wrapper belum dibundel —
tambah `gradle/wrapper/` saat build pertama). **Tidak butuh NDK** — tidak ada
kode native di varian ini.

## Struktur

```
lsp/
├── settings.gradle.kts
├── build.gradle.kts
├── app/
│   ├── build.gradle.kts                com.android.application, minSdk 26
│   └── src/main/
│       ├── AndroidManifest.xml         xposedmodule/minversion/sharedprefs/scope
│       ├── assets/xposed_init          id.sandboxid.tt.SandboxIdTT
│       ├── assets/devices.tsv          40 device × 8 brand (disalin dari data/)
│       ├── res/values/arrays.xml       xposedscope
│       └── java/id/sandboxid/tt/
│           ├── MainActivity.java       UI: tombol Generate (1-klik)
│           ├── PersonaGenerator.java   generator persona mandiri
│           ├── SandboxIdTT.java        entry point; gate packageName
│           ├── IdentityStore.java      XSharedPreferences ↔ prefs sendiri
│           ├── BuildHook.java          Build.*
│           ├── PropHook.java           SystemProperties.*
│           ├── PropMap.java            tabel prop (hand-maintained di sini)
│           ├── ClockHook.java          SystemClock offset
│           ├── SeedId.java             fnv1a/splitmix64/synth + AppLog ids
│           ├── KevaHook.java           baca KEVA (adapter + X.02tJ)
│           ├── RegisterHook.java       mint did baru di device_register
│           ├── Rewrite.java            patch body register (wire form)
│           └── TelephonyDrmHook.java   TelephonyManager/MediaDrm/WifiInfo/BT
│
│   └── src/test/java/id/sandboxid/tt/
│       └── RewriteTest3.java           pin rewriter di CI (JDK-only)
```

## Yang sudah diverifikasi (tanpa build)

Karena tidak bisa di-build di sini, yang diverifikasi adalah **kesetaraan
numerik** terhadap source asli — beberapa di antaranya dengan menjalankan C++
asli sebagai referensi:

| Yang dicek | Cara | Hasil |
|---|---|---|
| AppLog seed + 6 ID (did/iid/ssid/cdid/clientudid/openudid) | build C++ referensi vs model Java, input sama | **cocok persis** (seed + semua 6 ID) |
| `build_utc_from_patch` | C++ asli vs model Java, `2023-08-05`+`230817V2345` | **1690688135** keduanya |
| Konversi tanggal Hinnant (round-trip, termasuk 2024-02-29) | model Java | OK |
| `fnv1a` UTF-8 bytes vs C++ | uji ekuivalensi | cocok (sebelumnya bug: `& 0xFF` per char, hanya cocok ASCII) |
| Generator PropMap | port regex Kotlin → Python, regenerasi | 156 entri (129 identity + 27 constant), **byte-identik**. Generator-nya sudah dihapus saat decoupling; tabel 156 entri itu kini dipelihara tangan di `PropMap.java` |
| Tabel `Build.*`, `parse_blob`, `should_hide_prop`, synth telephony/DRM | bandingkan vs source native | identik |

**Yang BELUM terverifikasi:** kompilasi, resolusi method di API level manapun,
perilaku runtime di TikTok, apakah `XSharedPreferences` benar resolve di
konfigurasi ini, dan apakah UI Activity berfungsi. Semua butuh toolchain.

## Target: TikTok saja

`xposedscope` dan `SandboxIdTT.TARGET` keduanya `com.zhiliaoapp.musically`.
Varian lain (Lite `com.zhiliaoapp.musically.go`, regional
`com.ss.android.ugc.trill`, Douyin `com.ss.android.ugc.aweme`) — ganti string di
kedua tempat.
