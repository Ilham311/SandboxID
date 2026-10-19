# SandboxID TT — varian LSPosed (standalone APK, no root)

> **Status: sudah di-build dan diuji di perangkat.** Dikompilasi dengan
> `javac` + `d8` + `aapt2` + `apksigner` (tanpa Gradle), dipasang dan dijalankan
> di Redmi alioth / HyperOS 3.0.3 / KernelSU + Vector. Satu klik tombol
> **New device persona** terbukti menghasilkan did baru:
>
> ```
> reset: persona baru: POCO 2311DRK48G openudid=e5adf87665bef0bd
> PurgeHook: removed 23 identity store(s) for persona 1792384334104
> reported did 7698230114438399540 (new_user=1)
> reset: did baru: 7698230114438399540  install_id: 7698231974641862401  new_user:1
> ```
>
> Jalur **tanpa root** (`PurgeHook` + `SelfRestart`) yang teruji di perangkat:
> uid modul tidak ada di `/data/adb/ksu/.allowlist`, sehingga `su` tidak
> terlihat dari proses app dan `RootShell.available()` bernilai false. Jalur
> **root** (`pm clear` + `am force-stop`) sudah ditulis tapi **belum pernah
> dieksekusi di perangkat** — lihat [Satu klik](#satu-klik-new-device-persona).

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
3. Buka LSPosed Manager, aktifkan modul, pastikan scope-nya TikTok
   (`xposedscope` sudah diisi `com.zhiliaoapp.musically` di manifest).
4. Tekan **"New device persona (one click)"**. Selesai — did baru muncul di
   layar.

Tombol kedua, **"Rotate persona only"**, hanya merotasi persona tanpa menghapus
data. Ini jalur cadangan; did **tidak** akan berganti, dan UI mengatakannya.

## Satu klik (new device persona)

`ResetFlow.run()` menjalankan seluruh prosedur reset dari dalam activity modul:

| # | Langkah | Root | Tanpa root |
|---|---|---|---|
| 1 | Generate persona + simpan | `PersonaGenerator.generate` + `IdentityStore.save` | sama |
| 2 | Hapus store identitas | `pm clear` (+ `pm grant` ulang) | `PurgeHook` di proses TikTok |
| 3 | Matikan proses TikTok | `am force-stop` | `SelfRestart` (receiver → `killProcess`) |
| 4 | Buka TikTok | `startActivity` | sama |
| 5 | Baca did yang diterbitkan | `awaitIssued` (baca `applog_stats.xml`) | `awaitBroadcast` (broadcast dari `RegisterHook`) |

Jalur yang dipilih ditentukan oleh `RootShell.available()` — `su -c id` harus
mengembalikan `uid=0`. Di perangkat uji hasilnya false (uid modul tidak ada di
KernelSU allowlist), jadi yang berjalan adalah kolom kanan.

**Kenapa harus dari activity, bukan service.** Android 10+ membatalkan
pembukaan activity dari latar belakang. Percobaan lewat `PersonaService`
menghasilkan `Abort background activity starts from 10306` di logcat, sehingga
TikTok tidak pernah dibuka dan tidak ada did. Activity modul yang sedang tampil
tidak terkena batasan itu.

**Kenapa prosesnya harus mati dulu.** Hook dipasang sekali per proses, dan
`PurgeHook` hanya jalan saat start. Selama TikTok hidup, reset berikutnya
merotasi persona lalu tidak meregister apa pun. Terukur: pid tidak berubah
melalui tiga reset berturut-turut.

**Kenapa `PurgeHook` cukup tanpa root.** `handleLoadPackage` dipanggil sebelum
`Application.onCreate`, jadi menghapus store di sana membuat KEVA dan TTNet
membuka store yang belum ada. Yang dihapus: `shared_prefs/applog_stats.xml`,
`files/keva/repo/{ug_install_settings_pref_i18n,pns_common_id_mini,multi_process_config,push_multi_process_config}`,
`app_webview/Default/Cookies`, `files/tt_net_config.config`.

**Kenapa `SelfRestart` tidak bisa disalahgunakan.** Action-nya publik, tapi
receiver menolak permintaan tanpa `restart_key` — kunci stabil yang hanya ada di
direktori prefs LSPosed (`rwx--x--x`, milik uid modul). Aplikasi lain tidak bisa
membacanya.

**Tanpa membuka UI** (misalnya dari skrip):

```sh
am start -n id.sandboxid.tt/.MainActivity --ez reset true
```

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
| Mount-hide | Butuh root |

`pm clear` otomatis **sudah ada** di varian ini (`ResetFlow`), dengan jalur
tanpa root (`PurgeHook`) sebagai pengganti saat `su` tidak tersedia.

**Jejak tambahan:** `XposedBridge.class` **wajib** resolve di proses target.
Modul Zygisk membuat **nol** class Java di proses app — varian ini murni
**menambah** permukaan terdeteksi (cek classloader adalah pola anti-hook umum).

**Timing:** `handleLoadPackage` jalan **setelah** class load, jadi `static final`
sudah membekukan nilai asli. Modul Zygisk menambal di `postAppSpecialize`
sebelum class-init. Diuji pada TikTok 47.1.4: nilai yang dibaca TikTok lewat
`Build.*`/`SystemProperties.*` sudah ter-spoof dengan benar, jadi celah ini tidak
berdampak pada jalur yang dipakai.

## Cara build

Dua jalur. Yang dipakai untuk menguji varian ini adalah jalur **tanpa Gradle**,
karena mesin ujinya (Termux di perangkat) tidak punya Android SDK lengkap —
hanya `javac`, `d8`, `aapt2`, `apksigner`, dan `android.jar`.

### Tanpa Gradle (yang dipakai)

```sh
# 1. Stub Xposed API + seluruh source app
javac -source 8 -target 8 -bootclasspath "$ANDROID_JAR" \
      -d classes $(find lsp/api/src lsp/app/src/main/java -name '*.java')

# 2. dex (d8 butuh min-api; module ini minSdk 26)
d8 --min-api 26 --output dex/ $(find classes -name '*.class')

# 3. resource + manifest
aapt2 link -o apk/base.apk -I "$ANDROID_JAR" \
      --manifest AndroidManifest.build.xml \
      -R compiled/*.flat --java gen/

# 4. masukkan classes.dex + assets, lalu sign
zip -j apk/base.apk dex/classes.dex
apksigner sign --ks keystore.jks apk/base.apk
```

`ANDROID_JAR` di perangkat uji:
`/data/data/com.termux/files/usr/share/java/android.jar`.

### Dengan Gradle

```sh
cd lsp
./gradlew assembleRelease       # butuh Android SDK + JDK
```

Butuh Android SDK (compileSdk 34), JDK 8+, Gradle (wrapper belum dibundel).
**Tidak butuh NDK** — tidak ada kode native di varian ini.

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
│           ├── MainActivity.java       UI: New device (1-klik) + rotate
│           ├── ResetFlow.java          orkestrasi reset (root / tanpa root)
│           ├── RootShell.java          `su -c`, ketiadaan root = hasil normal
│           ├── PurgeHook.java          hapus store identitas dari proses target
│           ├── SelfRestart.java        receiver: purge lalu killProcess
│           ├── PersonaService.java     entry point headless (GENERATE/RESET)
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

## Yang sudah diverifikasi

Kesetaraan numerik terhadap source asli — beberapa di antaranya dengan
menjalankan C++ asli sebagai referensi:

| Yang dicek | Cara | Hasil |
|---|---|---|
| AppLog seed + 6 ID (did/iid/ssid/cdid/clientudid/openudid) | build C++ referensi vs model Java, input sama | **cocok persis** (seed + semua 6 ID) |
| `build_utc_from_patch` | C++ asli vs model Java, `2023-08-05`+`230817V2345` | **1690688135** keduanya |
| Konversi tanggal Hinnant (round-trip, termasuk 2024-02-29) | model Java | OK |
| `fnv1a` UTF-8 bytes vs C++ | uji ekuivalensi | cocok (sebelumnya bug: `& 0xFF` per char, hanya cocok ASCII) |
| Generator PropMap | port regex Kotlin → Python, regenerasi | 156 entri (129 identity + 27 constant), **byte-identik**. Generator-nya sudah dihapus saat decoupling; tabel 156 entri itu kini dipelihara tangan di `PropMap.java` |
| Tabel `Build.*`, `parse_blob`, `should_hide_prop`, synth telephony/DRM | bandingkan vs source native | identik |

**Sudah terverifikasi di perangkat:** kompilasi, pemasangan, resolusi
`XSharedPreferences` di proses target, seluruh hook terpasang, UI berfungsi, dan
satu klik menghasilkan did baru lewat jalur tanpa root.

**Belum terverifikasi:** jalur root (`RootShell` tidak pernah mendapat `su` di
perangkat uji), perilaku di versi TikTok selain 47.1.4, dan perangkat dengan ROM
yang tidak memblokir background activity start (jalur `PersonaService` mungkin
berhasil di sana).

## Target: TikTok saja

`xposedscope` dan `SandboxIdTT.TARGET` keduanya `com.zhiliaoapp.musically`.
Varian lain (Lite `com.zhiliaoapp.musically.go`, regional
`com.ss.android.ugc.trill`, Douyin `com.ss.android.ugc.aweme`) — ganti string di
kedua tempat.
