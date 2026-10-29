# Changelog

## Unreleased

### Fix: nama kelas per-build yang belum tercakup di varian LSPosed

Native memakai pasangan nama kelas yang berbeda per build, dan varian LSPosed
hanya membawa satu sisi dari tiap pasangan. Pasangannya struktural, bukan
tebakan: tiap pasangan punya set metode identik dan hanya berbeda di nama tipe
yang disebut.

**Register body rewrite.** `sbx_lsplant.hpp:1408-1420` memasang `post` pada
empat kelas — `X.087U`/`X.085T` (musically 47.1.4) dan `X.07WK`/`X.07Tq` (trill
47.0.3). Varian LSPosed hanya punya dua yang pertama, jadi pada build trill body
register tidak pernah ditulis ulang: openudid/clientudid/cdid yang dikirim ke
server adalah nilai cache lama, bukan persona. Persis kegagalan "tampak
terpasang, tidak melakukan apa-apa" yang sudah dua kali muncul di varian ini.

**Adapter KEVA.** `X.02tJ` dan `X.02t3` bukan hal yang sama dengan nama berbeda:
di trill 47.0.3, `X.02tJ` adalah enum lima metode tanpa `getString` sama sekali.
Kembarannya adalah `X.02t3` (18 metode, set identik kecuali tiga tipe parameter
yang ikut berganti nama). Varian LSPosed hanya meng-hook `X.02tJ`, sehingga pada
build trill pembacaan openudid/clientudid/cdid kembali ke nilai tersimpan.

### Fix: geometri layar varian LSPosed masih bisa dibaca dari bentuk lain

Tiga celah di hook geometri yang ditambahkan `b0ee4bf`, semuanya ditemukan dengan
membandingkan hasilnya baris per baris dengan `SandboxIDHook.java` alih-alih
menganggap "sudah ada hook" berarti "sudah benar".

**`Resources.getConfiguration()` tidak di-hook.** Configuration membawa
`screenWidthDp`/`screenHeightDp`/`densityDpi` — satuan dp, bukan piksel, dan
inilah permukaan yang dipakai aplikasi untuk menata layout. Tanpa hook ini
persona yang mengaku Pixel 9 Pro XL tetap melaporkan geometri dp panel asli
berdampingan dengan geometri piksel yang sudah dipalsukan: dua angka yang saling
bertentangan dari satu perangkat. Native membawanya sebagai spec tersendiri
(`sbx_lsplant.hpp:1204-1206`).

**Tidak ada penukaran orientasi.** Framework mengisi out-parameter dengan
dimensi panel yang sedang aktif, jadi `getSize(Point)` dalam keadaan landscape
tiba dengan lebar > tinggi. Menulis pasangan portrait persona ke situ membuat
pemanggil membaca panel portrait sementara display-nya berputar. Native menukar
di kasus itu (`SandboxIDHook.java:1053-1058`); sekarang `applyMetrics`,
`applyRect`, dan `applyConfiguration` melakukannya juga, dibaca dari bentuk
objek itu sendiri sehingga tidak perlu referensi `Display`.

**Objek hasil ditimpa di tempat.** `Resources.getConfiguration()` mengembalikan
instance Configuration milik Resources itu sendiri. Menimpanya langsung merusak
status resource aplikasi untuk semua pemanggil berikutnya, bukan hanya yang ini.
Sekarang hasilnya disalin dulu (`DisplayMetrics.setTo`, konstruktor salin
Configuration, konstruktor Point) dan salinan itulah yang dikembalikan — sama
seperti native (`SandboxIDHook.java:486-511`). Bentuk out-parameter tetap ditulis
di tempat, karena objek itu memang milik pemanggil.

### Fix: daftar accessibility service tidak ikut disembunyikan

`packageNameOf()` hanya membaca field `packageName`, yang tidak dimiliki
`AccessibilityServiceInfo` — kelas itu menamai pemiliknya lewat `getId()`
(`"pkg/.Service"`). Dan `AccessibilityManager` tidak di-hook sama sekali. Jadi
`bin.mt.plus` tetap muncul sebagai `bin.mt.plus.canary/l.…` di
`getInstalledAccessibilityServiceList` walaupun `getInstalledPackages` sudah
menyangkal keberadaannya — persis bug yang native perbaiki di `isHiddenItem()`
(`SandboxIDHook.java:883-899`). Grab membaca daftar itu dan mencatat setiap
service id di hasil scan-nya.

## v2.2.52 (2026-10-29)

### Fix: hook GSF dan advertising id di varian LSPosed tidak pernah terpasang

Dua kesalahan di commit `98edf0a`, keduanya membuat hook yang tampak terpasang
sebenarnya tidak pernah jalan.

**GSF id dibaca dari key persona yang tidak ada.** `TelephonyDrmHook` mengambil
`id.get("GSF_ID")` dan melewati hook bila null. Native tidak pernah membaca key
itu: `module.cpp:447` menurunkan nilainya dari seed
(`decimal_from_seed(seed ^ 0x4753464944)`), dan `PersonaGenerator` tidak
menulis `GSF_ID` sama sekali. Hasilnya `id.get("GSF_ID")` selalu null, sehingga
`hookGsfCursor` tidak pernah dipanggil — persis kegagalan yang commit itu
maksudkan untuk diperbaiki. Sekarang nilainya diturunkan lewat
`SeedId.gsfId(seed)`, port dari `decimal_from_seed`.

**Advertising id tidak punya hook sama sekali.** Native memasang dua spec
(`AdvertisingIdClient$Info.getId` dan `AdvertisingIdClient.getAdvertisingIdInfo`,
`sbx_lsplant.hpp:1125-1129`); varian LSPosed tidak punya keduanya. Yang kedua
yang menentukan: Grab membundel client-nya sendiri, dan
`getAdvertisingIdInfo` memanggil `b(false)` yang bind ke layanan Play services
dan melempar `IOException` sebelum `Info` mana pun ada — sehingga `getId()`
tidak pernah dipanggil dan Grab mencatat `er:109`. Sekarang `hookGaid()`
memasang keduanya, dan kelas `Info` diambil dari
`p.method.getReturnType()` (di-resolve oleh loader aplikasi, satu-satunya yang
bisa melihat kelas di DEX Grab), bukan `Class.forName` yang akan mencari di
loader modul.

Keduanya diturunkan dari seed yang sama dengan native, dan diverifikasi
byte-identik: `uuid_from_seed`/`decimal_from_seed` dikompilasi dari
`native/native_read.cpp` lalu dibandingkan dengan port Java untuk enam seed,
termasuk yang bit tertingginya menyala. `SeedIdTest` naik dari 144 ke 157
check, semuanya lulus.

### Fix: varian LSPosed tidak menjawab query GSF id

Grab membaca GSF id lewat query `content://com.google.android.gsf.gservices` untuk
baris `android_id`, lalu `Long.parseLong(cursor.getString(1))`. Di perangkat yang
tabel gservices-nya tidak punya baris itu (belum pernah ada akun Google), parse
gagal dan Grab mencatat `gs:"er:103"`. Modul Zygisk menjawab query itu dengan
cursor sintetis (`M_GSF_CURSOR`); varian LSPosed tidak, sehingga id-nya tetap
kosong.

Gating-nya ketat dan sama dengan native: hanya URI gservices, dan hanya bila
`android_id` ada di antara key yang diminta. Menjawab key lain dengan baris ini
akan merusak pembacaan yang tidak berhubungan — Grab memang membaca key lain
lewat provider yang sama.

Indeks argumennya berbeda satu dari native: Xposed tidak menyertakan receiver,
jadi `selectionArgs` ada di `args[3]`, bukan `args[4]`.

### Fix: varian LSPosed tidak menyembunyikan tool root dan paket vendor asing

Modul Zygisk membawa delapan spec `ApplicationPackageManager`
(`getInstalledPackages`/`getInstalledApplications`/`getPackageInfo`/
`getApplicationInfo`, bentuk int-flags dan bentuk flags-object API 33) plus
`HIDDEN_PACKAGES` dan carve-out `vendor_keep` di `module.cpp:415-431`. Varian
LSPosed tidak punya satu pun, sehingga aplikasi bisa melihat Magisk, KernelSU,
LSPosed Manager, Termux, dan MT Manager terpasang di perangkat yang mengaku
Pixel stok.

Dua arah, dan varian perlu keduanya atau lebih buruk daripada tidak sama sekali:
tool root harus tidak terlihat, tetapi paket vendor milik persona **harus tetap
terlihat** — persona Xiaomi yang ROM-nya tampak tercopot juga merupakan tell.
`SeedId.vendorKeep()` meniru carve-out native: Xiaomi/Redmi/POCO mempertahankan
paket MIUI, OPPO/OnePlus/realme mempertahankan ColorOS, persona lain tidak
mempertahankan apa pun.

`getPackageInfo`/`getApplicationInfo` melempar `NameNotFoundException` untuk
paket tersembunyi — jawaban yang sama dengan paket yang memang tidak terpasang,
jadi penyembunyian tidak bisa dibedakan dari ketiadaan.

### Fix: stub API LSPosed kurang `findClass` dan `hookAllMethods`

`lsp/api/` adalah stub kompilasi untuk API Xposed, dan dua anggota API nyata
tidak ada di sana: `XposedHelpers.findClass(String, ClassLoader)` dan
`XposedBridge.hookAllMethods(Class, String, XC_MethodHook)`. Keduanya dipakai
hook baru di atas, dan ketiadaannya menggagalkan `lsp-apk` di CI
(`cannot find symbol`). Signature-nya disamakan dengan API asli, bukan sekadar
dibuat lolos kompilasi.

`XC_MethodHook.MethodHookParam` juga mendapat `method` (bertipe
`java.lang.reflect.Member`), yang dipakai untuk membedakan `Display.getWidth`
dari `getHeight` di satu callback yang sama.

### Fix: varian LSPosed tidak punya hook resolusi layar sama sekali

Modul Zygisk membawa tujuh spec untuk geometri layar
(`Resources.getDisplayMetrics`, `Display.getMetrics`/`getRealMetrics`/
`getSize`/`getRealSize`/`getWidth`/`getHeight`, `WindowMetrics.getBounds`) plus
tabel model→resolusi di `module.cpp:450-503`. Varian LSPosed **tidak punya satu
pun**. Akibatnya persona yang mengaku Pixel 9 Pro XL tetap melaporkan panel asli
1080x2400 — kontradiksi yang bisa dilihat SDK mana pun tanpa akses istimewa,
karena `Build.MODEL` dan `Display.getRealSize()` ada di API publik yang sama.

Ditambahkan `SeedId.screenGeometry()` (port tabel native, dengan override
persona `SCREEN_WIDTH`/`SCREEN_HEIGHT`/`LCD_DENSITY`/`SCREEN_XDPI`/`SCREEN_YDPI`
yang hanya berlaku bila positif dan valid) dan `hookDisplayMetrics()` yang
menangani ketiga bentuk API: nilai kembalian `DisplayMetrics`, out-parameter
`DisplayMetrics`/`Point`, dan skalar `getWidth`/`getHeight` yang sadar rotasi.

Urutan pencocokan model penting dan sudah ditiru dari native: `Pixel 9 Pro XL`
mengandung `Pixel 9 Pro`, dan `Pixel 9 Pro Fold` mengandung keduanya — yang
paling spesifik harus menang, atau persona Pro Fold melaporkan panel Pro XL.

### Fix: varian LSPosed melewatkan replay gate register dan rewrite User-Agent

Dua hook yang modul Zygisk punya **tidak pernah dipasang** di varian LSPosed,
padahal target varian itu (`com.zhiliaoapp.musically`) adalah build yang
keduanya tuju.

**1. Replay gate `X.088c.LJII` / `X.07XI.LJII`.** Native memasangnya sebagai
`M_FORCE_FALSE` dengan `force_index=2`. Tanpa hook ini, rotasi persona yang
tidak mengubah `dr_aid`/`dr_channel`/`dr_install_vc` dijawab dari cache lokal
dan **no-op diam-diam** — terlihat persis seperti "server mengenali perangkat
ini", padahal permintaannya tidak pernah keluar dari HP. `RegisterHook` sekarang
memasangnya lewat `hookAllMethods`, dibatasi pada bentuk argumen tepat tiga
parameter dengan dua Boolean terakhir.

Indeksnya **berbeda satu** antara kedua sisi: `args[]` native (LSPlant)
menyertakan receiver, `args[]` Xposed tidak. `force_index=2` native setara
`p.args[1]` di Xposed. Memakai indeks native apa adanya akan membalik boolean
yang salah.

**2. Rewrite User-Agent.** Native menghook `WebSettings.getDefaultUserAgent`
dan `X.04vW.LIZ` (musically; `X.04v0` di trill). Keduanya tidak ada di
LSPosed, sehingga UA tetap menyebut model/build asli. Ditambahkan sebagai
`SeedId.userAgentPair()` + `hookUa()`.

Jebakan yang sama dengan native: `PropHook` menghook
`SystemProperties.get`, jadi membaca prop "asli" dari dalam hook akan
mengembalikan nilai persona dan membuat `from == to` — no-op yang tetap
membayar satu hook. Karena itu `SandboxIdTT` menangkap
`ro.build.version.release`/`ro.product.model`/`ro.build.id` **sebelum**
`PropHook.apply()`. Native mengatasi ini dengan `sbx_raw_prop_get`; di sini
urutannya yang menjamin.

Kedua sisi kini punya helper yang sama (`user_agent_pair` di
`jni/sbx_ident_synth.hpp`, `SeedId.userAgentPair` di Java) dengan anchor nilai
yang dipin di kedua suite, jadi drift ke depan gagal di CI.

### Docs: tabel cakupan LSPosed mengklaim Zygisk tak bisa hook telephony

`lsp/README.md` menyebut TelephonyManager/MediaDrm/WifiInfo/BluetoothAdapter/
Settings.Secure sebagai permukaan "tidak bisa dicapai Zygisk" karena nilainya
datang lewat Binder. Itu salah: modul membawa LSPlant (`SBX_ENABLE_LSPLANT`,
default on) dan menghook kelimanya di dalam proses app. Log sesi perangkat
memuat baris `L3 hooked` untuk masing-masing. Untuk kelima permukaan itu kedua
varian setara; pembeda sebenarnya hanya `RegisterHook`/`Rewrite` dan
ketiadaan kebutuhan root.

## v2.2.47 (2026-10-29)

### Fix: dua spec `CellIdentityNr` yang mustahil selalu MISS

**Gejala.** Log sesi perangkat
(`debug/session-20261029-163257.log`) menunjukkan `L3 MISS
android/telephony/CellIdentityNr.getMcc()I` dan `.getMnc()I` masing-masing 20
kali — setiap kali aplikasi start. Penyebabnya bukan bug hook: kelas
`CellIdentityNr` **tidak punya** `getMcc()`/`getMnc()`. Daftar metode kelas itu
di `framework.jar` perangkat (API 35) adalah `getMccString`, `getMncString`,
`getNci`, `getNrarfcn`, `getPci`, `getTac` — varian NR tidak pernah membawa
bentuk int yang dimiliki LTE/WCDMA/GSM. Kedua spec dihapus dari
`jni/sbx_lsplant.hpp`, dan sisi LSPosed memakai daftar kelas terpisah
(`CELL_IDENTITY_INT_CLASSES`) yang tidak memuat NR.

Ini bukan kosmetik: spec yang tidak bisa resolve menggelembungkan hitungan MISS
yang dipakai diagnostik L3 untuk menyimpulkan "layout kelas berubah", sehingga
MISS sungguhan bisa tersamarkan. Spec native turun 95 → 93.

## v2.2.46 (2026-10-29)

### Fix: varian LSPosed melewatkan hook cell identity dan carrier id

**Gejala.** Perbaikan v2.2.38 ("`getMnc()` / `getMcc()` / `getSimCarrierId()`
tidak di-hook") hanya masuk ke tabel hook modul Zygisk. Varian LSPosed tidak
pernah menerimanya, jadi dengan varian itu terpasang field `"mnc"` di
`pax.track.nk.measures.network_calls` kembali berisi MNC asli — persis gejala
yang v2.2.38 tutup, muncul lagi di varian yang lain.

**Akar masalah.** `TelephonyDrmHook` hanya meng-hook `TelephonyManager`:
`getNetworkOperator`, `getSimOperator`, `getNetworkOperatorName`,
`getSimOperatorName`, `getSimCountryIso`, `getNetworkCountryIso`. Tujuh belas
spesifikasi native untuk `CellIdentityLte/Wcdma/Gsm/Nr` dan `SubscriptionInfo`
(`getMcc`/`getMnc`/`getMccString`/`getMncString`) serta
`TelephonyManager.getSimCarrierId()` tidak punya padanan sama sekali. Objek dan
API-nya berbeda: `getSimOperator()` menjawab dari `TelephonyManager`, sedangkan
MNC yang dicatat Grab dibaca dari objek cell/subscription.

**Perbaikan.** `SeedId` mendapat `operatorMcc()`/`operatorMnc()` — port
`sbxid::operator_mcc/mnc`, pemisahan posisional setelah tiga digit, dan
**kedua** bagian kosong untuk apa pun yang bukan numerik 5–6 digit. Lalu:

- `getSimCarrierId()` di-hook sebagai `Integer`, bukan `String` — metode itu
  mengembalikan `int`, dan hasil `String` akan melempar `ClassCastException` di
  dalam framework. Nilai persona yang tidak bisa di-parse dilewati, bukan
  dipasang sebagai hook yang selalu melempar. Ini padanan `ret_type=2` di
  spesifikasi native.
- `CellIdentityLte/Wcdma/Gsm/Nr` dan `SubscriptionInfo`: bentuk string
  (`getMccString`/`getMncString`, API 28+) dan bentuk int (`getMcc`/`getMnc`),
  keduanya hanya dipasang bila operator diketahui. Bentuk int dari nilai tak
  diketahui adalah `Integer.MAX_VALUE`, dan `0` adalah carrier nyata (T-Mobile
  US) — jadi mengarang angka lebih buruk daripada tidak menjawab.

**Bukti.** `SeedIdTest` naik 89 → 102 pemeriksaan; `sbx_ident_synth` 88 → 93.
Tiga belas pemeriksaan baru membandingkan `operatorMcc`/`operatorMnc` dengan
keluaran `sbxid::operator_mcc/mnc` untuk `51011`, `310260`, `510110`, `5101`
(4 digit), `5101X` (non-numerik), string kosong, dan `null`.

### Fix: tiga dari enam AppLog id berbeda antara varian Zygisk dan LSPosed

**Gejala.** Untuk persona yang sama, `SeedId.applogIds()` menghasilkan
`openudid`, `clientudid`, dan `cdid` yang berbeda dari
`sbxnr::make_applog_ids()`. `PersonaGenerator` menulis ketiganya ke persona
(`PERSONA_OPENUDID`, `PERSONA_CLIENTUDID`, `PERSONA_CDID`), lalu `RegisterHook`
mengirimnya ke `device_register`; modul Zygisk membaca persona yang sama dan
menyajikan nilai yang lain. Akibatnya persona yang sama mendaftar sebagai dua
perangkat berbeda, tergantung varian mana yang terpasang.

**Akar masalah.** `native_read.cpp:110-133` memakai salt tersendiri untuk tiap
id **dan** meng-XOR `epoch_ms` ke keenamnya. Sisi Java tertinggal di bentuk
lama:

```java
uuid(seed ^ 0x2545F4914F6CDD1DL),   // cdid       — tanpa ^ epochMs
uuid(seed ^ 0x9E3779B97F4A7C15L),   // clientudid — salt salah, tanpa ^ epochMs
hexFromSeed(seed, 8)                // openudid   — tanpa salt, tanpa epoch
```

Javadoc-nya pun menyatakan "exactly native_read.cpp:90-99 … `openudid` uses the
UN-XORed seed" — benar saat commit `e22034a`, salah sejak `55c68c8` memindahkan
native ke salt terpisah + pencampuran epoch. Tidak ada satu pun pemeriksaan di
`SeedIdTest` yang menyentuh `applogIds()`, jadi tidak ada yang menangkapnya.

Selisih terukur untuk seed `0x1234ABCD`, epoch `1700000000000`:

| id | Java (lama) | Native |
|---|---|---|
| `did` | `7130316800001884825` | sama |
| `iid` | `7130316800001862035` | sama |
| `ssid` | `7130316800001309879` | sama |
| `cdid` | `32459f40-30c6-4bc9-b181-63ce2af04480` | `4831a5e5-ae29-49e6-897e-a50292a4cb12` |
| `clientudid` | `99c21cd8-ee14-491c-a47d-c04428a4ceaa` | `383d033e-ba38-45ea-9f41-a82f20ede5ce` |
| `openudid` | `a1b19b645b16becc` | `52c5eb42a728473b` |

**Perbaikan.** `applogIds()` kini menyalin `make_applog_ids()` baris per baris,
termasuk `^ epochMs` pada ketiga id yang dulu beku, dan javadoc-nya diganti
dengan alasan yang sebenarnya (bukan nomor baris yang bergeser): server
mengikat pasangan (openudid, cookie `install_id`) secara permanen pada
pertemuan pertama, sehingga openudid yang tidak ikut berputar membuat
`rotate_ids.sh applog` menjadi no-op yang senyap di sisi server — berkas lokal
tampak berubah, tetapi `device_register` tetap mengembalikan did lama.

**Bukti.** Enam string jangkar dipatok di **dua** suite sekaligus, dari arah
berlawanan, sehingga pergeseran di salah satu sisi menggagalkan tes:

- `tests/host/sbx_pure.cpp` — `make_applog_ids(0x1234ABCD, 1700000000000)`
  dibandingkan dengan keenam literal. Suite host 131 → 137 pemeriksaan.
- `lsp/app/src/test/java/id/sandboxid/tt/SeedIdTest.java` — `SeedId.applogIds()`
  dibandingkan dengan keenam literal yang sama, plus empat pemeriksaan rotasi
  (epoch +1 harus menggeser `did`/`cdid`/`clientudid`/`openudid`), tiga
  pemeriksaan keterpisahan aliran (`did` ≠ `clientudid`, `cdid` ≠
  `clientudid`), dan dua pemeriksaan bentuk (UUID v4, openudid 16 hex).
  Suite naik 71 → 89 pemeriksaan, `ALL PASS`.

Nilai jangkar itu bukan hasil hitung tangan: diambil dengan mengompilasi dan
menjalankan `native/native_read.cpp` sendiri, lalu membandingkan keluarannya
dengan `SeedIdTest`.

**Diuji dengan persona perangkat yang sebenarnya.** Persona aktif di perangkat
uji (`FINGERPRINT=google/tegu/tegu:15/AP3A.241005.015/12580213:user/release-keys`,
`SERIAL=1baf177ca94a95fe`, `ANDROID_ID=635346526fab5cd1`,
`APPLOG_EPOCH=1793266308000`) diumpankan ke tiga pihak: CLI native yang
terpasang di perangkat, `make_applog_ids()` yang dikompilasi ulang, dan
`SeedId.applogIds()` yang sudah diperbaiki. Ketiganya sepakat. Untuk
`com.grabtaxi.passenger`:

| id | Sebelum (Java) | Sesudah = perangkat (native) |
|---|---|---|
| `openudid` | `5de2b0211b9d6ffa` | `42517712fe9ca447` |
| `clientudid` | `156c6ddf-2377-44e3-bd35-53b403fc962b` | `e3ccacdb-5d32-406b-b44c-ac16bb7e76ac` |
| `cdid` | `b11fff3b-90b0-4a69-99a8-7d19a56462d1` | `75c5286b-4776-4f06-8474-a2d81ca9c49c` |

Dengan perbaikan ini, `sandboxid applog-ids com.grabtaxi.passenger` di perangkat
dan `SeedId.applogIds()` di varian LSPosed menghasilkan baris yang identik
byte-per-byte, termasuk `did`/`iid`/`ssid` dan IMEI/MEID/MAC.

**Celah yang sama, ditutup sekalian.** Saat memeriksa ini, `imei()`, `meid()`,
dan `widevineHex()` ternyata **hanya** diuji bentuknya (panjang, semua digit,
Luhn, pola hex) — dan pemeriksaan bentuk lolos untuk seed apa pun, jadi tidak
mungkin menangkap drift antar-varian. Itu persis bagaimana tiga AppLog id di
atas lolos sekian lama. Ketiganya kini dipatok nilainya, dari kedua sisi:

| Fungsi | Nilai untuk seed `0x1234ABCD` |
|---|---|
| `synth_imei` / `SeedId.imei` | `354381107373819` |
| `synth_meid` / `SeedId.meid` | `0EE1910E29C47B` |
| `synth_widevine_hex` / `widevineHex` | `92ce2df3…0883fa81` (64 hex) |
| `synth_imsi(…, "51011")` | `510115509216614` |
| `synth_iccid(…, "51011")` | `8962767996665767591` |

Kelima nilai itu **sudah** sepakat sebelum perubahan ini — ini penjaga, bukan
perbaikan. Suite host `sbx_ident_synth` 83 → 88 pemeriksaan; `SeedIdTest`
71 → 89.

## v2.2.42 (2026-10-29)

### Fix: MAC Bluetooth tidak pernah memakai persona, dan `bluetooth_address` bocor

Tiga cacat terpisah pada permukaan Bluetooth, semuanya ditemukan dengan
membongkar 22 DEX Grab 5.430.0 dan membuktikan jalur baca yang benar-benar
dipakai aplikasi itu.

**A. `BluetoothAdapter.getAddress()` mengabaikan `BLUETOOTH_ADDR`.** `HookValues`
tidak punya field `bt_mac` sama sekali, dan `install_all()` membangun alamatnya
tanpa syarat dari seed:

```cpp
g.bt = sbx_mac_upper(sbxnr::mac_from_seed(v.seed ^ 0x424C554554ULL));
```

Jadi persona yang mendeklarasikan `BLUETOOTH_ADDR=02:91:1a:ba:67:2a` tetap
dilaporkan sebagai `02:84:86:7D:02:DC` — nilai yang tidak pernah ada di
`identity.prop` maupun di `bt_config.conf` yang ditulis `rotate_ids.sh`. Sisi
Wi-Fi sudah benar (`g.wifi` memakai `v.wifi_mac` lebih dulu); Bluetooth tidak.
Asimetri ini juga membuat kedua varian berbeda: LSPosed memang memakai
`BLUETOOTH_ADDR`.

**B. `Settings.Secure.getString(resolver, "bluetooth_address")` tidak di-hook.**
Ini permukaan yang paling penting, karena **tidak butuh izin** — berbeda dari
`getAddress()` yang melempar `SecurityException` tanpa `BLUETOOTH_CONNECT`.
Grab membaca baris itu langsung di `Lc/g/zuo;->a()` dan mengembalikannya apa
adanya; jalur itu terjangkau dari penyusun device-info Moca wallet SDK
(`Lc/g/zuo;->a()` ← `DeviceInfoUtils;->a()` ← `Lc/g/fcw;->invoke()` ←
`Lc/g/eaq;->call()`, yang punya 17 rujukan ke `Lc/g/fcw;`). Sebelum perbaikan
aplikasi menerima MAC adapter **asli** `8C:AA:CE:2C:19:D5` di perangkat uji,
sementara setiap field lain pada event yang sama menggambarkan persona.

**C. Nama perangkat tidak di-hook.** `BluetoothAdapter.getName()` dan
`Settings.Global` `bluetooth_name`/`device_name` semuanya melaporkan nama asli
`CPH2521`. HyperVerge membaca `device_name` di
`DeviceExtensionsKt;->getDeviceBasicProperties()`, dan `Lc/g/zuo;->f()` membaca
`getName()`; `sync_device_name()` di shell sudah menulis ketiganya dari
`identity.prop`, jadi lapisan hook dan lapisan shell saling bertentangan.

**Perbaikan.**

- `sbxid::pick_mac(persona, seed, salt, upper)` — satu selektor untuk kedua MAC.
  Nilai persona menang bila valid; bila tidak ada **atau tidak valid**, dipakai
  fallback turunan seed yang selalu locally-administered (`02:`), sehingga tidak
  pernah bertabrakan dengan OUI vendor nyata dan tidak pernah menyajikan MAC
  asli. Kasus huruf mengikuti permukaan platformnya: `WifiInfo.getMacAddress()`
  huruf kecil, `BluetoothAdapter.getAddress()` dan `bluetooth_address` huruf
  besar. Port Java-nya `SeedId.pickMac()` memakai algoritma yang identik.
- `HookValues` mendapat `bt_mac` dan `bt_name`; `jni/module.cpp` mengisinya dari
  `BLUETOOTH_ADDR` dan `BLUETOOTH_NAME` (fallback `MODEL`, sama seperti shell).
- Spesifikasi `Settings$Secure.getString` diubah dari `M_VALUE` satu kunci
  menjadi `M_VALUE_MULTI` dengan tabel `kSecureKeys`/`kSecureVals`
  (`android_id`, `bluetooth_address`). Ini wajib, bukan preferensi: LSPlant
  menolak hook kedua pada `ArtMethod` yang sudah di-hook, jadi dua spec terpisah
  hanya akan memasang yang pertama — persis cara adapter Keva dulu kehilangan
  `clientudid` dan `cdid`.
- Spesifikasi baru untuk `BluetoothAdapter.getName()` dan
  `Settings$Global.getString` (`bluetooth_name`, `device_name`).
- Varian LSPosed mendapat `MultiKeyReplacer` (pengganti `KeyedReplacer` yang
  hanya satu kunci) dan perbaikan yang sama. Satu cacat tambahan ikut hilang di
  sana: `hook0(cl, "android.net.wifi.WifiInfo", "getMacAddress",
  wifi.toUpperCase())` mengirim MAC Wi-Fi dalam huruf **besar**, padahal
  `WifiInfo.getMacAddress()` dan `/sys/class/net/*/address` selalu huruf kecil —
  jadi kedua varian melaporkan string yang berbeda untuk radio yang sama, dan
  yang LSPosed justru yang tidak sesuai konvensi platform.

**D. Bonus, ditemukan saat mengerjakan ini: seed LSPosed ≠ seed Zygisk.**
`TelephonyDrmHook` memakai `SeedId.personaSeed()` —
`fnv1a(FINGERPRINT|SERIAL|ANDROID_ID)` — sementara native memakai
`applog_seed()` yang **menambahkan nama paket**. Untuk persona yang terpasang di
perangkat uji keduanya menghasilkan seed yang berbeda (`0xd2977f09334b4cef` vs
`0x165cfa3d979e66d4`), sehingga IMEI yang dilaporkan berbeda
(`358765540630080` vs `358473134580916`) dan fallback MAC Bluetooth berbeda
(`02:66:76:BE:C7:BB` vs `02:84:86:7D:02:DC`). Docstring kelas itu sendiri
menjanjikan kedua varian menghasilkan identifier yang sama, jadi ini pelanggaran
kontrak. `personaSeed()` dihapus (tidak ada padanannya di native), dan
`TelephonyDrmHook.apply()` sekarang menerima paket serta memakai `applogSeed()`.

**Bukti.** `sbx_ident_synth` (suite host) naik dari 54 ke 83 pemeriksaan; `SeedIdTest`
dari 44 ke 71. Keduanya mengunci dua nilai MAC yang sama persis, jadi pergeseran di
salah satu sisi menggagalkan test di kedua tempat. Diverifikasi runtime terhadap
persona yang terpasang di perangkat uji saat audit (persona itu sudah dirotasi
sejak itu, jadi menjalankan ulang sekarang akan memberi angka lain yang sama
konsistennya):

| Permukaan | Sebelum | Sesudah |
|---|---|---|
| `WifiInfo.getMacAddress()` | `02:90:d5:57:3d:1f` ✓ | `02:90:d5:57:3d:1f` |
| `BluetoothAdapter.getAddress()` | `02:84:86:7D:02:DC` ✗ | `02:91:1A:BA:67:2A` |
| `Settings.Secure bluetooth_address` | `8C:AA:CE:2C:19:D5` ✗ (asli) | `02:91:1A:BA:67:2A` |
| `BluetoothAdapter.getName()` | `CPH2521` ✗ (asli) | `CPH2521` (persona) |
| `Settings.Global device_name` | `CPH2521` ✗ (asli) | `CPH2521` (persona) |

Catatan jujur soal jangkauan. Pada perangkat uji `BLUETOOTH_CONNECT` untuk Grab
adalah `granted=false`, jadi `getAddress()` dan `getName()` praktis inert untuk
aplikasi itu. Anotasi AOSP mengonfirmasi keduanya dijaga izin:

```java
@RequiresBluetoothConnectPermission
@RequiresPermission(allOf = {BLUETOOTH_CONNECT, LOCAL_MAC_ADDRESS})
public String getAddress() { ... }

@RequiresBluetoothConnectPermission
@RequiresPermission(BLUETOOTH_CONNECT)
public String getName() { ... }
```

(`packages/modules/Bluetooth/framework/java/android/bluetooth/BluetoothAdapter.java`,
`main`.) Anotasi itu **tidak** menegakkan apa pun saat runtime: dex
`framework-bluetooth.jar` di perangkat tidak memuat `RequiresPermission` sama
sekali (anotasi dibuang saat build), dan baik `getAddress()` maupun `getName()`
hanya meneruskan panggilan ke `IBluetoothManager` tanpa pemeriksaan in-process.
Penegakannya ada di proses Bluetooth sendiri, bukan di aplikasi:

```
AdapterService$AdapterServiceBinder->getAddress(AttributionSource)
  -> Utils->checkConnectPermissionForDataDelivery(...)   // BLUETOOTH_CONNECT
  -> Utils->enforceLocalMacAddressPermission(...)        // LOCAL_MAC_ADDRESS
AdapterService$AdapterServiceBinder->getName(AttributionSource)
  -> Utils->checkConnectPermissionForDataDelivery(...)   // BLUETOOTH_CONNECT
```

(`Utils->checkConnectPermissionForDataDelivery` adalah pembungkus tipis yang
memang mengoper `android.permission.BLUETOOTH_CONNECT` ke
`checkPermissionForDataDelivery`, jadi rantai anotasi → penegakan benar-benar
tersambung, hanya saja di proses lain.)

Itulah sebabnya hook lapisan ini tetap berarti meski izinnya tidak diberikan:
ia mengganti nilai di sisi aplikasi, sebelum binder dipanggil sama sekali,
sehingga pemeriksaan service tidak pernah melihat permintaan untuk MAC asli.
Yang **hidup tanpa izin** justru dua permukaan lain:
`Settings.Secure`'s `bluetooth_address` dan `Settings.Global`'s
`device_name`/`bluetooth_name`.

Diperiksa langsung dengan `settings get` di perangkat uji, dan hasilnya
memisahkan mana yang sudah ditutup shell dan mana yang belum:

| Baris | Nilai | Ditulis oleh |
|---|---|---|
| `secure bluetooth_address` | `8C:AA:CE:2C:19:D5` (MAC adapter **asli**) | tidak ada — inilah bocornya |
| `global device_name` | `Pixel 9a` | `sync_device_name()` |
| `global bluetooth_name` | `Pixel 9a` | `sync_device_name()` |
| `persist.bluetooth.adaptername` | `Pixel 9a` | `sync_device_name()` |

Jadi baris nama sudah benar sejak sebelum perubahan ini — yang ditambahkan hook
adalah **sumbernya** (kini persona di dalam proses, bukan properti sistem yang
bisa saja belum tersinkron), bukan nilainya. `bluetooth_address` sebaliknya
tidak pernah ditulis siapa pun: tidak ada `settings put secure bluetooth_address`
di `sh/`, karena baris itu memang dikelola proses Bluetooth sendiri. Itulah satu-
satunya permukaan Bluetooth yang benar-benar bocor, dan sekarang ditutup di
lapisan hook.

## v2.2.40 (2026-10-29)

### Fix: varian LSPosed mengarang operator yang sama — `SeedId.imsi()`/`iccid()`

**Gejala.** Cacat yang identik dengan entri operator di v2.2.38, tetapi di
varian LSPosed: `SeedId.imsi()` mengganti operator apa pun yang tidak bisa
di-parse dengan `"51010"` (Telkomsel), dan `SeedId.iccid()` sama sekali tidak
punya penjaga sehingga menghasilkan `8901…` dengan kode negara karangan `01`.
`TelephonyDrmHook` juga memberi default `"51010"` saat
`GSM_OPERATOR_NUMERIC` tidak ada, dan `PersonaGenerator` **selalu** menulis
keempat key itu — jadi setiap persona yang pernah dibuat UI membawa bug ini.

**Dibuktikan, bukan diduga.** `SeedId` murni Java tanpa dependensi Android, jadi
bisa dijalankan di JDK host. Sebelum perbaikan: `imsi(seed, "")` →
`510105509216614` (prefix Telkomsel), `iccid(seed, "")` → `8901767996665767599`
(kode negara `01`).

**Perbaikan.**

- `SeedId.imsi()` dan `iccid()` mengembalikan string **kosong** untuk operator
  yang tidak diketahui, memakai satu predikat bersama `isOperatorNumeric()`
  supaya keduanya tidak bisa menyimpang. Aturannya sama dengan sisi native:
  identifier sintesis konsisten dengan operatornya, atau kosong.
- `TelephonyDrmHook` tidak lagi memberi default. Saat persona tidak punya
  operator, ia membaca MCC+MNC **asli** lewat `android.os.SystemProperties`
  (refleksi, karena itu API tersembunyi) dan membuang koma per-slot, meniru
  `install_java_hooks()` di `jni/module.cpp`. Hook IMSI/ICCID hanya dipasang
  bila nilainya tidak kosong — hook yang menjawab `""` bukan sama dengan tidak
  ada hook, karena `getSubscriberId()` tanpa SIM mengembalikan `null`.
- `PersonaGenerator` tidak lagi menulis `GSM_OPERATOR_*` / `GSM_CARRIER_ID`.
- Suite baru `lsp/app/src/test/java/id/sandboxid/tt/SeedIdTest.java` (44
  pemeriksaan) dikunci di CI sebagai langkah "LSPosed identifier-synthesis unit
  test". Nilai harapannya diambil dari `synth_imsi()`/`synth_iccid()` native
  untuk seed yang sama, jadi pergeseran di salah satu sisi menggagalkan test.

## v2.2.38 (2026-10-29)

### Fix: operator seluler dikarang (Telkomsel 51010) → bertentangan dengan radio asli

**Gejala.** Di dalam satu aliran event milik aplikasi yang sama, telemetri
jaringan mencatat `"mnc":"11"` (XL, dibaca dari permukaan radio yang tidak
di-hook) sementara event pax di sebelahnya mencatat `"crn":"Telkomsel"` dan
`"mcc":"510"` (di-spoof lewat properti dan `TelephonyManager`). Dua nilai itu
tidak mungkin benar bersamaan untuk satu perangkat.

**Akar masalah.** `VAL_DEFAULTS` di `native/include/config.hpp` memuat
`GSM_OPERATOR_NUMERIC=51010`, `GSM_OPERATOR_ALPHA=Telkomsel`,
`GSM_OPERATOR_ISO=id`, `GSM_CARRIER_ID=787`. Selama tidak ada `carrier.conf`,
`val()` mengembalikan nilai bawaan itu, sehingga modul mengklaim operator yang
tidak pernah dipilih siapa pun. SIM uji adalah XL `51011`. `synth_imsi()` punya
masalah yang sama dari arah lain: saat string operator kosong atau tidak valid,
ia mengganti prefix dengan `"51010"`, jadi IMSI yang disajikan menamai jaringan
yang perangkatnya tidak tersambung.

**Perbaikan.**

- `GSM_OPERATOR_*` dan `GSM_CARRIER_ID` **dihapus** dari `VAL_DEFAULTS`. Tanpa
  operator terkonfigurasi, `val()` mengembalikan string kosong,
  `spoof_prop_value()` menolak, dan **nilai asli** yang disajikan — sehingga
  setiap permukaan, yang di-hook maupun tidak, sepakat.
- `install_java_hooks()` mengisi `hv.op_num` / `op_alpha` / `op_iso` dari
  properti **asli** lewat `sbx_raw_prop_get` ketika persona tidak punya
  operator, supaya IMSI/ICCID yang disintesis tetap koheren dengan SIM.
  Properti GSM bersifat per-slot dan digabung koma — perangkat ini
  mengembalikan `"51011,"` — jadi koma pemisah dibuang sebelum dipakai.
- `synth_imsi()` dan `synth_iccid()` mengembalikan string **kosong** untuk
  operator yang tidak diketahui, bukan prefix Telkomsel. Hook lalu jatuh ke
  nilai asli, yang konsisten dengan dirinya sendiri.

### Fix: `getMnc()` / `getMcc()` / `getSimCarrierId()` tidak di-hook — MNC asli bocor

**Gejala.** Sama seperti di atas, tetapi dari sisi cakupan: field `"mnc"` di
`pax.track.nk.measures.network_calls` tetap berisi MNC asli.

**Akar masalah.** `TelephonyManager.getNetworkOperator()` memang di-hook, tetapi
itu API dan objek yang berbeda dari yang dipakai aplikasi untuk membaca MNC
secara terpisah. Grab mereferensikan `android.telephony.CellIdentityCdma`,
`CellInfoCdma`, `SubscriptionInfo`, dan `getSimCarrierId`; tidak satu pun
tercakup tabel hook.

**Perbaikan.** `ValId` mendapat `V_OP_MCC` dan `V_OP_MNC` (dipecah dari
`V_OP_NUM`, 3 digit pertama dan sisanya) serta `V_CARRIER_ID`. Spesifikasi baru
mencakup `getMccString`/`getMncString` dan `getMcc`/`getMnc` untuk
`CellIdentityLte`, `CellIdentityWcdma`, `CellIdentityGsm`, `CellIdentityNr`, dan
`SubscriptionInfo`, plus `TelephonyManager.getSimCarrierId()`. Bentuk int
memakai `ret_type=2`, yang sudah didukung `spoofValue()`. Semuanya mengembalikan
string kosong saat operator tidak dikonfigurasi, sehingga jatuh ke nilai asli —
bukan mengarang angka.

### Fix: `IMEI` berisi ANDROID_ID — `READ_PHONE_STATE` dicabut `pm clear`

**Gejala.** Blob parameter perangkat Grab memuat
`"IMEI":"d38af538f80a0314"` — 16 karakter heksadesimal di tempat 15 digit
desimal seharusnya — dan `"andrID"` berisi string yang sama persis. Setiap
identifier lain pada request yang sama sudah ter-spoof dengan benar.

**Akar masalah.** `dumpsys package` menunjukkan
`READ_PHONE_STATE: granted=false`. Grab menggerbangi pembacaan IMEI-nya sendiri
dengan izin itu; saat ditolak ia melewati `getImei()` dan mengisi field IMEI
dengan satu-satunya identifier yang masih boleh dibaca, yaitu ANDROID_ID. Hook
IMEI terpasang dan tidak pernah dipanggil. `pm clear` mencabut seluruh izin
runtime, dan `pkg_restore_permissions()` hanya bisa mengembalikan yang **sudah**
pernah diberikan — izin yang tidak pernah diberikan pengguna tetap ditolak.

**Perbaikan.** `pkg_persona_permissions()` dan `pkg_grant_persona_permissions()`
di `sh/lib/helpers.sh`, dipanggil `sh/lifecycle/action.sh` setelah restore.
Daftarnya sengaja pendek dan berbasis bukti (`READ_PHONE_STATE` saja).
Memberi izin hanya mengubah apa yang **boleh** dibaca aplikasi; hook L3 tetap
menjawab setiap getter telepon dengan nilai sintesis, jadi yang dibaca setelah
izin diberikan adalah identifier persona, bukan radio.

### Fix: `sbx_ident_synth.hpp` tidak punya test host

Ketiga bug di atas lolos karena tidak ada yang menguji header itu. Suite baru
`tests/host/sbx_ident_synth.cpp` (54 pemeriksaan) mengunci aturannya: sebuah
identifier sintesis harus konsisten dengan operator yang membangunnya, atau
**kosong** supaya pemanggil jatuh ke nilai asli — tidak pernah berupa hal ketiga
yang terlihat masuk akal. Termasuk kasus koma per-slot yang membuat
`synth_imsi()` menolak string operator yang sebenarnya valid.

### Fix: `debugLog` memakai `android.util.Log` langsung — kelas callback gagal dikompilasi

`jni/SandboxIDHook.java` dikompilasi menjadi DEX tanpa `android.jar`, dan
`tests/host/EnvCompatStateCheck` mengompilasi berkas yang sama di JDK biasa.
Referensi langsung ke `android.util.Log.d` menggagalkan keduanya. Diganti
dengan refleksi `Class.forName("android.util.Log")`, pola yang sudah dipakai
kelas ini untuk setiap kelas Android lain.

## v2.2.37 (2026-10-28)

### Fix: build gagal di armeabi-v7a — `__NR_newfstatat` tidak ada di bionic 32-bit

**Gejala.** Job CI `build` mati di langkah "Build (release + debug)" dengan tiga
error yang sama:

```
jni/module_hooks.cpp:734:34: error: use of undeclared identifier '__NR_newfstatat'
jni/module_hooks.cpp:740:35: error: use of undeclared identifier '__NR_newfstatat'
jni/module_hooks.cpp:767:37: error: use of undeclared identifier '__NR_newfstatat'
```

**Akar masalah.** bionic memberi nama nomor syscall `fstatat` berbeda menurut
lebar kata: ABI 64-bit memakai `__NR_newfstatat`, sedangkan **kedua** ABI 32-bit
yang ikut dibangun modul ini — `armeabi-v7a` dan `x86` — hanya mendefinisikan
`__NR_fstatat64`. `build.sh` membangun keempat ABI, jadi `arm64-v8a` lolos dan
`armeabi-v7a` gagal lebih dulu; `x86` akan gagal dengan cara yang sama.

Angka yang terukur di sini: aarch64 mendefinisikan `__NR_newfstatat` = **79** dan
tidak mendefinisikan `__NR_fstatat64`; libc 32-bit ARM di perangkat memakai
syscall **327** (`__NR_fstatat64`). Pasangan x86 mengikuti konvensi bionic yang
sama, tetapi **tidak diukur di mesin ini** — toolchain host mengabaikan `-m32`
tanpa error, jadi tidak ada cara memprobe-nya dari sini. Yang menutup x86 adalah
CI: `build.sh` membangun keempat ABI, sehingga `x86` ikut dikompilasi di sana.

**Kenapa lolos sampai CI.** Seluruh suite host mengompilasi
`jni/module_hooks.cpp` memakai header glibc host yang 64-bit, yang punya
`__NR_newfstatat`. Cabang 32-bit tidak pernah tersentuh di lokal, sehingga
`host-tests` hijau sementara `build` merah. Ini kelas bug yang tidak bisa
ditangkap suite mana pun yang hanya berjalan di satu ABI.

**Perbaikan.** Satu makro `SBX_NR_FSTATAT` yang memilih nama yang tersedia,
dengan `#error` bila tidak ada keduanya — supaya ABI baru di masa depan gagal
saat kompilasi, bukan diam-diam kehilangan jalur fallback:

```c
#if defined(__NR_newfstatat)
#  define SBX_NR_FSTATAT __NR_newfstatat
#elif defined(__NR_fstatat64)
#  define SBX_NR_FSTATAT __NR_fstatat64
#else
#  error "no fstatat syscall number for this ABI"
#endif
```

**Kenapa ini bukan perubahan ABI.** Pada bionic 32-bit, `stat` dan `stat64`
adalah **fungsi yang sama**. Di
`/apex/com.android.runtime/lib/bionic/libc.so` perangkat referensi, kedua simbol
itu resolve ke `0x495c8`, dan body-nya menyetel `r0 = AT_FDCWD` lalu bercabang
ke `fstatat64` (`0x847dc`, `movw r7, #0x147` = 327). Artinya syscall yang
menopang `stat` di proses 32-bit memang 327, dan struct yang diisinya adalah
layout `off_t` 64-bit yang sudah menjadi `struct stat` bionic. Mengoper
`struct stat*` ke syscall itu persis yang dilakukan libc sendiri — jadi
mengganti nama makro tidak mengubah apa yang ditulis ke buffer.

**Regresi tertutup.** `tests/host/sbx_abi32.cpp` + `tests/host/abi32_bionic.h`
mengompilasi TU yang sama dengan keadaan preprocessor 32-bit dipaksakan
(`__NR_newfstatat` di-undef, `__NR_fstatat64 = 327`), lewat `-include` agar
berjalan sebelum `<sys/syscall.h>` milik TU itu. Dijalankan di `run.sh` sebagai
pemeriksaan compile-only.

**Hasil mutasi.** Mengembalikan `__NR_newfstatat` di satu tempat saja langsung
membuat `sbx_abi32` gagal kompilasi dan seluruh suite melaporkan
`==> host suites FAILED`; setelah dipulihkan kembali hijau. Nilai makro
diverifikasi: 79 pada aarch64 asli, dan 327 pada keadaan 32-bit yang dipaksakan.

### Fix: `gs:"er:103"` dan `ad`/`adl:"er:109"` di hasil scan Grab

**Gejala.** Hasil scan Grab memuat `gs:"er:103"`, `ad:"er:109"`, dan
`adl:"er:109"`. Ketiganya adalah field identitas yang gagal dibaca, dan
kegagalan itu sendiri adalah sinyal perangkat yang lebih keras daripada nilai
apa pun yang benar.

**Akar masalah, `ad`/`adl`.** Grab **membundel sendiri**
`com.google.android.gms.ads.identifier.AdvertisingIdClient` di DEX-nya. Metode
`getAdvertisingIdInfo(Context)` membuat client lalu memanggil `b(false)`, yang
melakukan `bindService` ke service advertising-id Play services dan melempar
`java.io.IOException` (`"Google Play services not available"`,
`"Connection failure"`) bila bind gagal. Exception itu keluar dari
`getAdvertisingIdInfo` **sebelum** objek `Info` pernah dibuat, sehingga
`$Info.getId()` tidak pernah dipanggil dan hook yang sudah ada tidak pernah
menyala. Pemanggil Grab menangkapnya dan mencatat `GENERAL_EXCEPTION` (109)
untuk kedua field.

**Akar masalah, `gs`.** Grab membaca GSF id dengan
`query("content://com.google.android.gsf.gservices", null, null,
new String[]{"android_id"}, null)`, lalu `Long.parseLong(cursor.getString(1))`
dan `Long.toHexString()`. Hook `Settings$Secure.getString` yang sudah ada tidak
menutupi jalur ini — API dan provider-nya berbeda. Pada perangkat yang tabel
gservices-nya kosong (belum pernah ada akun Google), kolom 1 bukan long yang
bisa di-parse, `parseLong` melempar `NumberFormatException`, dan Grab mencatat
`GSF_NUMBER_FORMAT_EXCEPTION` (103).

**Bukti.** `dexdump -d` pada `classes.dex` Grab:
`c.g.o7y1.a()` memanggil `getAdvertisingIdInfo` lalu `$Info.getId()` dan
`isLimitAdTrackingEnabled()`; blok `catch`-nya memetakan
`NoClassDefFoundError`/`ClassNotFoundException` → 111, kedua exception GMS →
112, dan **exception lain apa pun** → 109. Body `AdvertisingIdClient.b(Z)`
menunjukkan `java.io.IOException` dilempar dari tiga tempat (bind gagal,
`GoogleApiAvailabilityLight` bukan `SUCCESS`, interrupted). Untuk `gs`,
`c.g.o7y1.a()` memuat `content://com.google.android.gsf.gservices`, memanggil
`ContentResolver.query`, lalu `moveToFirst`/`getColumnCount() >= 2`/
`getString(1)`/`Long.parseLong`/`Long.toHexString`, dengan
`NumberFormatException` → field `.g` = 103. Pemetaan enum diverifikasi dari
bytecode `clinit` `c.g.cvy1`, bukan dari urutan ordinal.

**Perbaikan.** Dua mode callback baru:
`M_GAID_INFO` (10) membangun `AdvertisingIdClient$Info` sendiri dari
`backup.getReturnType()` — kelas itu hidup di DEX aplikasi, jadi `Class.forName`
dari callback tidak akan menemukannya — sehingga `getAdvertisingIdInfo` tidak
pernah menyentuh Play services; `M_GSF_CURSOR` (11) menjawab query gservices
dengan `MatrixCursor` dua kolom `{_id, value}` berisi GSF id **desimal** dari
persona, sehingga `Long.parseLong` berhasil. Nilai GSF id berasal dari
`decimal_from_seed(seed ^ "GSFID")`, dengan bit teratas dibersihkan agar selalu
positif 63-bit seperti id asli.

**Kenapa desimal, bukan hex.** Provider gservices menyajikan long desimal; hanya
`toHexString()` milik klien yang mengubahnya ke hex. Menyajikan hex di sini akan
melempar `NumberFormatException` di klien — persis kegagalan yang diperbaiki.

**Regresi tertutup.** `tests/host/EnvCompatStateCheck.java` menguji kedua arah
untuk masing-masing hook: dengan nilai, `IOException` asli tidak boleh lolos dan
`Info` harus membawa id persona serta flag limit-tracking `false`; tanpa nilai,
`IOException` asli harus tetap sampai ke pemanggil (mengembalikan `null` akan
membuat Grab melakukan dereference pada `Info` null). Untuk GSF, query ke
provider lain harus tetap mencapai `ContentResolver` asli. `tests/host/sbx_pure.cpp`
mengunci format: semua digit, deterministik, berbeda per seed, dan berada dalam
rentang long positif untuk 4096 seed. Uji mutasi: hook GAID dijadikan no-op
menggagalkan 2 check, gerbang URI dibuang menggagalkan 1, cursor satu kolom
menggagalkan 3, bit teratas tidak dibersihkan menggagalkan 3, dan hex alih-alih
desimal menggagalkan 2.

### Fix: daftar accessibility service membocorkan paket yang sudah disembunyikan

**Gejala.** Grab mencatat `bin.mt.plus.canary/l.ۖۢۖ` di daftar accessibility
service pada setiap scan, padahal modul sudah menyembunyikan `bin.mt.plus` dari
`getInstalledPackages`/`getPackageInfo`.

**Akar masalah.** `M_FILTER_PACKAGES` hanya membaca field `packageName`, yang
dimiliki `PackageInfo`/`ApplicationInfo`. `AccessibilityServiceInfo` tidak punya
field itu — pemiliknya hanya bisa dibaca dari `getId()`, berformat
`pkg/.Service`. Akibatnya filter bekerja untuk satu API dan tidak untuk yang
lain, sehingga dalam satu proses aplikasi diberi tahu bahwa sebuah paket tidak
ada, lalu tetap melihat service milik paket itu.

**Bukti.** `dexdump -d` pada `classes.dex` Grab menunjukkan
`AccessibilityManager.getInstalledAccessibilityServiceList` dipanggil di
`c.g.joy1.b`, yang membangun daftar `AccessibilityServiceItemData(serviceId,
isAccessibilityTool, serviceEnabled, isSystemApp, isSideloaded)` (label field
dibaca dari `toString()` kelas `c.g.lhy1`). Hasil scan Grab memuat id service
tersebut apa adanya.

**Perbaikan.** `filterPackages` memakai `isHiddenItem()`, yang mencoba
`packageName` lebih dulu lalu jatuh ke `getId()` dan memotong di `/`. Kedua
entri `AccessibilityManager` (`getInstalledAccessibilityServiceList`,
`getEnabledAccessibilityServiceList`) memakai mode `M_FILTER_PACKAGES` yang
sama, sehingga himpunan tersembunyi hanya punya satu definisi.

**Regresi tertutup.** `tests/host/EnvCompatStateCheck.java` menguji kedua arah:
service MT Manager harus hilang dari kedua API, sedangkan TalkBack, SystemUI dan
Katrina (yang memang **tidak** ada di `HIDDEN_PACKAGES`) harus tetap muncul —
filter yang diam-diam melebar ke "apa pun yang sideloaded" adalah kebijakan lain
yang tidak diminta. Uji mutasi: membuang jalur `getId()` menggagalkan 2 check,
menyembunyikan semua item menggagalkan 5.

### Fix: pemeriksaan root lewat `stat()` tidak pernah di-hook — Grab membaca `/system/bin/su` yang asli

**Gejala.** Promo di aplikasi target ditolak dengan pesan
"Promo yang kamu pilih nggak bisa digunakan. Klik di sini untuk melihat detail
selengkapnya." — padahal persona sudah aktif dan hook properti berjalan.

**Akar masalah.** Seluruh lapisan file yang ada hanya meng-hook jalur **isi**
(`open`/`openat`/`read`/`pread64`/`lseek`/`close`). Sebuah library yang hanya
menanyakan apakah sebuah path **ada** tidak pernah memanggil `open()`, sehingga
tidak satu pun hook itu menyala untuknya.

Itu persis bentuk pemeriksaan root yang dikirim Grab. `libsafexEx.so`
(Neutralbase **SafeX**) mengekspor
`Java_com_neutralbase_lib_safex_SafeXNative_isSUExist`; membongkar badan
196-byte-nya menunjukkan **delapan** `bl <stat@plt>` dan tidak ada yang lain, dan
delapan literal string yang dimuatnya adalah `/sbin/su`, `/system/bin/su`,
`/system/xbin/su`, `/data/local/xbin/su`, `/data/local/bin/su`,
`/system/sd/xbin/su`, `/system/bin/failsafe/su`, `/data/local/su`.

Di perangkat referensi `/system/bin/su` **ada**: hardlink ke `/system/bin/sh`
(inode 544, mode `0755 root:shell`, label `u:object_r:system_file:s0` —
world-traversable, jadi `stat()` dari uid app berhasil). Kedelapan panggilan itu
mengembalikan sukses dan modul tidak pernah melihatnya.

**Bukti.** `readelf -sW --dyn-syms` pada keempat library keamanan Grab
(`libpsec.so`, `libsafexEx.so`, `libCredolabNative.so`, `libcore-lib.so`)
menunjukkan `stat`/`lstat`/`access`/`readlink`/`uname` diimpor tetapi tidak ada
satu pun di `kPltRegs` (saat itu 25 simbol, semuanya keluarga
file/properti/exec). `libpsec.so` juga mengimpor `uname` dan memanggilnya
(`bl <uname@plt>` di `0x1be454`).

**Perbaikan.** `stat`, `lstat`, `fstatat`, `access` dan `readlink` di-hook di
kedua jalur (PLT dan inline/Dobby) dan melaporkan `ENOENT` untuk basename biner
root yang sudah ditolak `sbx_is_root_binary()` — satu definisi, sehingga penjaga
exec dan penjaga stat tidak bisa berbeda pendapat. Sengaja sempit: hanya
basename root yang di-hide, bukan seluruh path, karena memblokir semua `stat`
akan merusak akses file app sendiri.

`uname` juga di-hook: `/proc/version` sudah disintesis dari persona, tetapi
`uname(2)` adalah pembungkus syscall terpisah, sehingga keduanya saling
bertentangan di dalam satu proses (persona CPH2521/taro: `/proc/version` =
`6.1.x-android15`, `uname()` = `4.19.337-FakeDreamer-1.9-Universe` — kernel yang
tidak pernah dijalankan CPH2521 mana pun, dan string build yang menyebut ROM-nya).
Keduanya sekarang diturunkan dari input persona yang sama.

**Regresi tertutup.** `tests/host/sbx_hook_sm.cpp` §15 memaku kedua arah:
kedelapan path SafeX (plus `daemonsu`/`magisk`/`ksud`) harus di-hide, dan path
biasa (`/system/bin/sh`, `/proc/self/maps`, file app sendiri, path yang hanya
* mengandung* "su") **tidak** boleh. Uji mutasi mengonfirmasi kedua arah bergigi:
meng-hide semua path menggagalkan 6 check, meng-hide nol path menggagalkan 11.

§16 memaku `uname` terhadap `/proc/version`: `release` harus sama persis dengan
token pertama baris yang disintesis, dan `version` harus berupa **prefiks** dari
sisanya yang dipotong tepat di batas field 64 karakter. Kesamaan penuh tidak
mungkin dan tidak benar — `utsname.version` hanya 65 byte sedangkan sisa baris
`/proc/version` lebih panjang, sehingga kernel sungguhan pun memotong dengan cara
yang sama. Tiga mutasi membuktikan uji ini bergigi: memecah di spasi terakhir
menggagalkan 3 check, memotong di 32 karakter menggagalkan 1, dan menjadikan hook
no-op menggagalkan 5.

Total suite host: **238 check, 0 gagal**.

## v2.2.36 (2026-10-28)

### Fix: AppZygote (`<pkg>_zygote`) di-specialize → prosesnya SIGABRT saat fork berikutnya

**Gejala.** `com.shopee.id_zygote` mati dengan `SIGABRT` sesaat setelah modul
mengaktifkannya. Tombstone di `debug/report/crashes-*.log`:

```
Abort message: 'JNI FatalError called: (zygote) Unable to stat 120'
Cmdline: com.shopee.id_zygote
#06 FileDescriptorInfo::CreateFromFd
#07 FileDescriptorTable::RestatInternal
#08 FileDescriptorTable::Restat
#09 zygote::ForkCommon
#10 zygote::forkApp
```

**Akar masalah.** `com.shopee.id_zygote` adalah **AppZygote** — zygote milik app
itu sendiri (dari `android:zygotePreloadName`), bukan proses app. Proses app yang
sebenarnya di-fork **dari** AppZygote itu belakangan, dan fork itulah yang
menjalankan `SpecializeCommon` dengan nama paket biasa.

`jni/companion.cpp:extract_base_pkg()` sengaja membuang sufiks `_zygote`, sehingga
`com.shopee.id_zygote` cocok dengan target `com.shopee.id`. Modul lalu menjalankan
`postAppSpecialize` penuh di dalam AppZygote itu, termasuk membuka soket companion.

Karena AppZygote **adalah** zygote, setiap fork yang dilakukannya menjalankan
`zygote::ForkCommon` → `FileDescriptorTable::Restat` → `CreateFromFd`, yang
`fstat()` **setiap** fd terbuka dan memanggil `fail_fn("Unable to stat %d")` bila
gagal. `fail_fn` = `ZygoteFailure` → `env->FatalError()` → `SIGABRT`. Daftar
`fds_to_ignore` dibangun **per-fork** dan hanya memuat fd yang diketahui framework;
soket companion yang dibuka modul di dalam AppZygote tidak pernah ada di dalamnya.
AppZygote pun mati pada fork berikutnya. Angka `120` di abort message itu persis fd
yang baru dikembalikan `connectCompanion()` di pid tersebut.

**Bukti korelasi.** Setiap AppZygote yang di-specialize crash; setiap yang
di-skip tidak:

```
com.shopee.id_zygote  specialized=1 crash=1   (2 kejadian)
com.android.chrome_zygote / webview_zygote /
com.instagram.android_zygote / ...  specialized=0 crash=0
```

**Perbaikan.** Child zygote ditolak, bukan di-specialize:

- `jni/sbx_process_name.hpp` (baru) — satu sumber kebenaran untuk aturan nama
  proses, murni (tanpa JNI/header Android), dipakai **modul dan companion**
  sekaligus supaya keduanya tidak bisa menyimpang. `is_child_zygote()` memakai
  field framework `AppSpecializeArgs::is_child_zygote` bila provider mengisinya
  (Magisk dan ReZygisk mengisinya dari parameter JNI asli), dan jatuh ke konvensi
  nama `_zygote` bila field opsional itu null. Field itu sebelumnya **tidak pernah
  dibaca sama sekali** di repo ini.
- `jni/module.cpp` — `preAppSpecialize` menolak child zygote sebelum
  `connectCompanion()`, jadi soket tidak pernah dibuka di dalamnya.
- `jni/companion.cpp` — `is_target()` juga menolak child zygote. Ini lapisan kedua
  yang berlaku **tanpa memandang build**: build lama yang masih terpasang di
  perangkat pun tidak akan lagi menerima blob untuk nama `_zygote`.
- `tests/host/sbx_process_name.cpp` (baru) — suite regression, terdaftar di
  `tests/host/run.sh`. Lima mutan (sufiks selalu false, flag diabaikan, guard
  panjang dibuang, sufiks dicocokkan sebagai infix, colon terakhir bukan pertama)
  semuanya terbunuh, jadi suite ini benar-benar mendiskriminasi.
- `sh/debug/summarize.sh` — dua penghitung baru: `child zygote skipped (module)`
  dan `child zygote refused (companion)`.

**Catatan operasional.** Build yang terpasang di perangkat saat audit ini
(`v2.2.30`/`261`) **mendahului semua perbaikan yang sudah di-commit**; `strings`
pada `zygisk/arm64-v8a.so` tidak memuat `getprop dump filtered`, `execlp`,
`fexecve`, `posix_spawn`, `sbx_popen_readable`, `sbx_exec_denied`,
`NATIVE_%s BLOCKED`, maupun `shell-guard entry point(s)`. Perlu build + install
ulang supaya perbaikan-perbaikan itu benar-benar berjalan.

### Fix: persona Qualcomm mengaku `ro.hardware=taro` — perangkat yang tidak mungkin boot

**Gejala yang dilaporkan.** Promo/voucher yang dipilih di app target tidak bisa
dipakai; app menampilkan "Klik di sini untuk melihat detail selengkapnya" —
gejala risk-control, bukan bug fungsional.

**Akar masalah.** Persona yang aktif di perangkat (OPPO CPH2521, `devices.tsv`
baris 77) meruntuhkan **empat prop berbeda** menjadi satu nilai, sehingga
menggambarkan perangkat yang tidak pernah ada:

| prop | perangkat nyata (alioth/SM8250) | persona (sebelum) |
|---|---|---|
| `ro.hardware` | `qcom` | `taro` |
| `ro.boot.hardware` | `qcom` | `taro` |
| `ro.board.platform` | `kona` | `taro` |
| `ro.product.board` | `alioth` | `taro` |
| `ro.soc.manufacturer` | `QTI` | `Qualcomm` |
| `dalvik.vm.isa.arm64.variant` | `kryo300` | `generic` |
| `dalvik.vm.isa.arm.variant` | `cortex-a75` | `generic` |

Bukti bahwa `ro.hardware=qcom` di Qualcomm: `/proc/cmdline` (kernel, tidak bisa
di-spoof) berbunyi `androidboot.hardware=qcom`; `/vendor/etc/init/hw/init.qcom.rc`
**ada** sedangkan `init.taro.rc` **tidak ada** — dan `ro.hardware` justru token
yang dipakai init untuk memilih `init.<ro.hardware>.rc`, jadi `ro.hardware=taro`
menggambarkan perangkat yang tidak mungkin boot. Pencarian kode di device tree
menemukan `ro.hardware=qcom` di mana-mana pada tree Qualcomm, dan **nol** hasil
untuk `ro.hardware=taro|kona|pineapple|kalama|lahaina`.

**Temuan kedua — prop yang bocor menyebut hardware/ROM asli.** Prop yang dibaca
app target (com.gojek.app, com.wowsoftware.devicefingerprint, id.dana) tapi tidak
di-spoof maupun di-hide:

- `ro.gfx.driver.1 = com.qualcomm.qti.gpudrivers.kona.api30` → **`kona` adalah
  platform ASLI**, bertentangan dengan persona yang mengaku `SM8475`/`taro`
- `ro.gfx.driver.0 = com.xiaomi.ugd` → jejak Xiaomi di bawah persona OPPO
- `ro.appsflyer.preinstall.path = /data/miui/pai/pre_install.appsflyer` → jejak MIUI
- `persist.sys.turbosched.*` (8), `persist.sys.perf_scenario_recognition.*` (4),
  `persist.sys.perfdebug.*` (2), `persist.sys.smart_gc.*` (2),
  `persist.sys.miui_animator_sched.*` (2), `ro.vendor.mi_sf.*` (7),
  `persist.miui.*`, `ro.display.mimotion`, `vendor.boostfwk.frameprefetcher`,
  `miui.usb.dialog`

`ro.miui.*` sudah di-hide dengan benar; yang lolos adalah bentuk garis bawah
`persist.sys.miui_` dan namespace MIUI di luar `ro.miui`.

**Perbaikan.**

- `native/include/hw_naming.hpp` (baru) — satu sumber kebenaran untuk penamaan
  hardware, murni (tanpa JNI/header Android/global) sehingga suite host bisa
  mem-pin-nya. `hardware_token()` mengembalikan `qcom` untuk setiap SoC Qualcomm
  dan token platform untuk MediaTek/Tensor/Exynos; `product_board()` memakai
  kodename kecuali Samsung (yang menaruh platform di sana, termasuk di perangkat
  Snapdragon-nya — Galaxy S23 = dm1q/SM8550 tapi `ro.product.board=kalama`);
  `soc_manufacturer_name()` menulis `QTI`; `dalvik_arm64_variant()` menurunkan
  sisanya. **`gpu_driver_pkg()` dihapus** — lihat temuan ketiga di bawah.
- `native/sandboxid.cpp` — blok hardware di `derive_identity()` memakai aturan
  di atas; `ro.hardware` tidak lagi menyalin platform.
- `native/include/prop_defs.hpp` — `ro.gfx.driver.1`,
  `dalvik.vm.isa.arm64.variant`, `dalvik.vm.isa.arm.variant` dipetakan.
- `jni/prop_hooks.cpp` — `derive_hw_value()` menurunkan ketiganya saat identitas
  di disk belum punya field itu, jadi perbaikan berlaku **tanpa perlu re-roll**.
- `jni/module_impl.hpp` — `sbx_oem_prop_hidden()`: `ro.appsflyer.preinstall.path`
  hanya di-hide bila persona bukan vendor itu. `ro.gfx.driver.*` **tidak lagi**
  di sini (lihat temuan ketiga).
- `native/native_read.cpp` — `is_vendor_rom_prop()` diperluas dengan 3 nama
  eksak dan 7 prefix HyperOS yang terukur di perangkat.
- `sh/identity/autopif.sh` — cermin shell dari aturan yang sama. Nilai turunan
  dihitung **sebelum** heredoc: menulis `VAR=` di dalam heredoc hanya menulis
  teks, sehingga `$VAR` di baris berikutnya menjadi kosong.
- `sh/debug/selftest.sh` — cek koherensi baru: FAIL bila persona Qualcomm
  melaporkan `HARDWARE` selain `qcom`, dan bila
  `HARDWARE`/`BOARD`/`BOARD_PLATFORM` runtuh menjadi satu nilai; WARN bila
  `SOC_MANUFACTURER` bukan `QTI`, bila `BOARD` tidak mengikuti konvensi
  vendor-nya, dan bila `DALVIK_ARM64_VARIANT` masih `generic`. Inilah kelas bug
  yang **tidak terlihat** oleh selftest sebelumnya (`pass=14 warn=1 fail=0`).
- `lsp/app/src/main/java/id/sandboxid/tt/HwNaming.java` (baru) — jalur LSPosed
  punya bug identik. `PersonaGenerator` menurunkan `BOARD`, `BOARD_PLATFORM` dan
  `HARDWARE` dari kolom `board` yang sama.
- `lsp/.../PropMap.java` — `ro.hardware`, `ro.board.platform`, `ro.product.board`,
  `ro.gfx.driver.1` dan kedua `dalvik.vm.isa.*.variant` **tidak dipetakan sama
  sekali**: app yang membaca `SystemProperties.get("ro.hardware")` melihat token
  perangkat asli sementara `Build.HARDWARE` melaporkan milik persona.
- `lsp/.../PropHook.java` — memakai `HwNaming` dan daftar hide yang disinkronkan
  dengan `is_vendor_rom_prop()` native.

**Regression test.** `tests/host/sbx_hw_naming.cpp` (C++) dan
`lsp/app/src/test/java/id/sandboxid/tt/HwNamingTest.java` (JDK murni, dijalankan
CI) mem-pin nilai yang sama. `product_board()` awalnya di-key pada Exynos dan
langsung tertangkap oleh suite baru (`got 'dm1q', want 'kalama'`).

### Fix: `ro.gfx.driver.*` di-spoof → framework mengeluh paketnya "not installed"

**Temuan ketiga, dan ini cacat dari perbaikan di atas.** Perbaikan pertama
memetakan `ro.gfx.driver.1` ke identitas `GPU_DRIVER` dan menurunkannya dari
platform persona menjadi `com.qualcomm.qti.gpudrivers.<platform>.api30`. Itu
**salah arah**, dan lebih buruk daripada kebocoran yang hendak ditutup.

`ro.gfx.driver.0/.1` bukan nama platform yang perlu disamarkan — keduanya nama
**paket yang harus terpasang**. Buktinya dari `framework.jar` (dex
`android/os/GraphicsEnvironment;`), satu-satunya pembacanya di seluruh framework:

```
'ro.gfx.driver.0'   'ro.gfx.driver.1'   'ro.gfx.driver_build_time'
"updatable driver package '" ... "' not installed"
'App is not on the allowlist for updatable production driver.'
'Neither updatable production driver nor prerelease driver is supported.'
```

`GraphicsEnvironment.get()` membaca prop itu lalu menanyakan PackageManager
apakah paketnya ada. Di perangkat referensi yang terpasang hanya
`com.xiaomi.ugd` dan `com.qualcomm.qti.gpudrivers.kona.api30` — **tidak ada**
`...gpudrivers.taro.api30`. Jadi menurunkan nilainya dari persona `taro`
membuat framework mencatat `updatable driver package
'com.qualcomm.qti.gpudrivers.taro.api30' not installed`: pesan yang **lebih
spesifik** daripada nilai asli yang digantikannya, karena ia menyebut platform
persona yang memang tidak punya paket driver di perangkat ini.

Bentuk yang benar adalah **hide, bukan spoof**. "Device ini tidak mengirim paket
UGD" adalah keadaan perangkat yang wajar dan koheren; "device ini menyebut paket
UGD yang tidak terpasang" bukan. Karena itu:

- `native/include/hw_naming.hpp` — `gpu_driver_pkg()` **dihapus**, bukan
  dibiarkan tak terpakai: helper yang menghitung nama paket adalah jebakan bagi
  pembaca berikutnya.
- `native/include/prop_defs.hpp` — entri `ro.gfx.driver.1` → `GPU_DRIVER` dibuang.
- `jni/prop_hooks.cpp`, `lsp/.../PropHook.java`, `lsp/.../PropMap.java`,
  `lsp/.../PersonaGenerator.java`, `native/sandboxid.cpp`,
  `sh/identity/autopif.sh` — tidak ada lagi yang menurunkan `GPU_DRIVER`.
- `native/native_read.cpp` — `is_vendor_rom_prop()` men-hide seluruh keluarga
  `ro.gfx.driver.` dan `ro.gfx.driver_` (dua prefix, bukan satu batang
  `ro.gfx.driver`, supaya nama yang bukan property tidak ikut tertelan). Ini
  juga menutup `ro.gfx.driver_build_time`, yang di perangkat referensi bahkan
  tidak diset.
- `jni/module_impl.hpp` — `sbx_oem_prop_hidden()` tidak lagi menangani
  `ro.gfx.driver.1`. Bentuk lamanya ("hide hanya bila persona bukan Qualcomm")
  salah dua kali: persona Qualcomm justru menurunkan paket yang tidak terpasang.

**Temuan keempat — daftar hide yang dienumerasi manual terus bocor.** Sapuan
2514 property perangkat terhadap daftar spoof/hide menemukan 23 prop yang
membawa jejak OEM/SoC asli dan tidak tercakup, di antaranya:

| prop | nilai asli | jejak |
|---|---|---|
| `ro.media.xml_variant.codecs[_performance]` | `_kona` | platform asli |
| `vendor.media.target_variant` | `_kona` | platform asli |
| `ro.vendor.extension_library` | `libqti-perfd-client.so` | Qualcomm |
| `vendor.usb.product_string` | `POCO F3` | model asli |
| `ro.com.google.clientidbase.pg` | `android-xiaomi-deo2` | Xiaomi |
| `ro.com.google.lens.oem_image_package` | `com.miui.gallery` | MIUI |
| `ro.csc.spotify.music.{partnerid,referrerid}` | `xiaomi_mobile` | Xiaomi |
| `persist.sys.miuibooster.name` | `miui_booster` | MIUI |
| `persist.vendor.camera.*.info` (4) | `com.qti.sensormodule.alioth_*` | kodename + vendor |
| `vendor.camera.aux.packagelist[ext]` | `com.xiaomi.cameratest` | Xiaomi |
| `ro.miui.pai.preinstall.path`, `ro.miui.product.home` | `/data/miui/...` | MIUI |

Akar polanya: setiap namespace MIUI ditulis sebagai daftar separator
(`persist.sys.miui.`, lalu `persist.sys.miui_`), sehingga bentuk tanpa pemisah
(`persist.sys.miuibooster.name`) lolos dari keduanya. Sekarang satu prefix
`persist.sys.miui` menutup ketiga bentuk, dan keluarga baru ditambahkan sebagai
prefix (`persist.vendor.camera.`, `vendor.camera.`, `ro.csc.spotify.`,
`ro.media.xml_variant.`, `ro.gfx.driver.`/`_`) alih-alih nama satu per satu.

**Regression test.** `tests/host/sbx_pure.cpp` mem-pin daftar hide yang baru —
termasuk near-miss yang **tidak boleh** ikut tertelan (`ro.gfx.driver` dan
`vendor.camera` telanjang). Diverifikasi dengan mutation testing: menghapus
baris hide `ro.gfx.driver.*` menghasilkan 3 FAIL, dan mengembalikan bentuk
separator lama menghasilkan 2 FAIL — jadi cek ini benar-benar mendiskriminasi,
bukan lulus kosong.


## v2.2.18 (2026-10-06)

### Riset: "backdate umur device" ditolak — `device_id` itu server-minted, epoch bukan sinyal kepercayaan

Gagasan "backdate `APPLOG_EPOCH` supaya `device_id` fabrikasi terlihat sebagai device
mapan (skor risiko rendah)" diaudit dan **ditolak sebagai gimmick**.

Versi pertama audit ini hanya mengutip writeup pihak ketiga — bukan bukti, dan
argumen "openudid → device_id deterministik" sendiri **tidak membuktikan apa-apa**
karena determinisme konsisten dengan kedua skenario (client menghitung MAUPUN server
menghitung). Audit diulang sampai ke **data-flow level di tiga implementasi register
yang benar-benar berjalan**:

- `hqucsx/tiktok_mobile` — detail penentunya ada di *device model*, bukan di call
  site: `api/domain/Device.py` mendeklarasikan `device_id = ""`/`install_id = ""`
  dan `DeviceUtils.create_device()` **tidak pernah mengisi keduanya**, sehingga POST
  register pertama membawa keduanya **kosong**, dan baru diisi dari response
  (`api/device_register.py:40-41`, `response['device_id_str']`). Nilai yang kembali
  tidak mungkin merupakan gema dari request.
- `xtekky/Tiktok-Applog` — implementasi register independen yang menyusun payload +
  query dari `openudid`/`clientudid`/`cdid` **tanpa `device_id`/`install_id` sama
  sekali**, lalu membaca `a['device_id']`/`a['install_id']` dari response. Register
  berhasil sambil menghilangkan id sepenuhnya — hanya mungkin kalau server yang
  mencetaknya.
- `shenydowa/deviceid-x-gorgon` — schema response: `{"server_time", "device_id",
  "install_id", "device_id_str", "install_id_str", "new_user": 1}`. `server_time`
  adalah jam server sendiri (bukan jam client) dan `new_user` adalah putusan
  baru-vs-lama dari server — server yang memegang buku registrasinya.

Konsekuensinya: device age adalah **catatan server**, bukan bit yang dikontrol client.
Snowflake fabrikasi tidak punya record registrasi, dan bukan `device_id` yang akan
diturunkan server dari `openudid` fabrikasi kita — backend risk-control melihat device
asing, tidak akan pernah melihat device "hangat". Backdate timestamp di snowflake lokal
= kosmetik murni.

**Yang terbukti vs yang masih inferensi** (ditulis jujur di CREDITS): *terbukti*
`device_id` server-minted. *Belum terbukti* bahwa risk backend mengabaikan bit timestamp
di dalam snowflake yang dikirim client — itu hanya bisa dipastikan dengan A/B register
live (epoch di-backdate vs segar) yang dibandingkan response risikonya, di luar scope
module privasi on-device. Inferensinya bersifat arsitektural: sistem yang sengaja
memindahkan pencetakan id ke server supaya id tidak bisa dipalsukan, lalu mencatat
`server_time` dan menghitung `new_user` sendiri, tidak akan mempercayai timestamp yang
bisa dipalsukan di dalam id yang baru saja ia kendalikan. Kedua cabang tetap berakhir
pada kesimpulan yang sama: kalau server mencari id-nya, tidak ada yang ditemukan; kalau
ia mempercayai bit-nya secara naif, pencetakan server-side-nya jadi sia-sia.

**Tidak ada perilaku kode yang berubah** — fitur applog sudah benar untuk tujuannya
(unlinkability: app membaca ID rotasi, tidak bisa menautkan install ini ke persona
sebelumnya). Malahan riset lapangan menunjukkan device yang *baru diregister* lebih
sering ditolak (error 31) ketimbang device_id asing — jadi behavior modul sekarang
(fabrikasi device_id nir-register) berada di sisi yang kurang berisiko. Yang ditambahkan
murni dokumentasi + penjaga anti-regresi, supaya ide backdating tidak diusulkan ulang:

- `README.md` — bullet baru di "Known limits" (AppLog `device_id`/`install_id` are not
  server-minted), dengan kedua referensi.
- `native/include/native_read.hpp` — comment penjaga di `applog_epoch_or_default`: epoch
  adalah salt determinisme + knob rotasi, BUKAN sinyal device-age; titik ini diambil
  karena CLI (`sandboxid.cpp`) dan hook L9 (`module_hooks.cpp`) sama-sama baca helper ini.
- `native/sandboxid.cpp` — back-reference singkat di blok `APPLOG_EPOCH` saat `freshen`.
- `CREDITS.md` — ketiga referensi di atas sebagai *negative result*, dengan tingkat
  pembuktian per-sumber dan batasan eksplisit apa yang belum terbukti.

### Varian LSPosed diredesain ulang — sekarang benar-benar mandiri (tanpa root, tanpa modul Zygisk)

Revisi pertama varian LSPosed **salah terhadap permintaan asli**. Ia membaca persona dari
path yang **harus dipublikasikan oleh sisi root modul Zygisk** (`action.sh` menyalin
`identity.prop` ke `/data/local/tmp/sandboxid/identity.prop`). Itu membuat varian ini
cuma tambahan yang tak berdiri sendiri — dipasang tanpa modul Zygisk, ia tidak melakukan
apa-apa. Permintaannya adalah modul LSPosed yang cukup sendiri.

**Akar masalahnya adalah asumsi desain yang tidak pernah diverifikasi:** draf pertama
mengasumsikan tidak ada cara bagi hook (UID app target) untuk membaca data milik module
app lain tanpa root, lalu memilih world-readable path sebagai workaround. Setelah
dicek ke source LSPosed sendiri, asumsi itu **salah**:

- `XSharedPreferences` memakai meta-data **`xposedsharedprefs`** untuk resolve ke
  `serviceClient.getPrefsPath()` → `ConfigManager.getPrefsPath()`
  (`LSPosed/daemon/.../ConfigManager.java:1095`), yang membuat direktori
  `/data/misc/<uuid>/prefs/<pkg>/` mode **`rwx--x--x`** lalu **`chown` ke UID app
  module** (`:1101-1109`).
- Jadi module menulis prefs-nya sendiri seperti app biasa, dan proses target membacanya
  dengan UID module — **tidak perlu world-readable, tidak perlu root**.
- `XSharedPreferences.edit()` sengaja melempar `UnsupportedOperationException` (read-only
  di sisi target) — memang semestinya target tidak boleh mengubah persona.

**Perubahan kode:**

- `PersonaGenerator.java` (**baru**) — seluruh pipeline persona dipindah ke pure Java.
  Sebelumnya **tidak ada satupun di `lsp/` yang menghasilkan persona**. Memakai
  `devices.tsv` (40 device, 8 brand) yang dibundle sebagai asset, bukan dua pool
  (`devices.tsv` + `personas.tsv`) dengan format kolom berbeda seperti sisi root —
  itu duplikasi yang komentarnya sendiri memperingatkan drift (`autopif.sh:314-318`).
  Setiap key yang rumusnya berbeda antara shell dan native, **sisi native yang dipakai**
  karena hook runtime ditulis melawannya; terutama `VBMETA_DIGEST` tetap deterministik
  (shell pakai random hex, yang memutuskan semua ID turunan dari seed).
- `MainActivity.java` (**baru**) — UI launcher app: tombol "Generate new device persona".
  Ini pengganti tombol Action manager yang tidak ada di LSPosed.
- `IdentityStore.java` — **ditulis ulang**: baca via `XSharedPreferences`, bukan file
  `/data/local/tmp`. `save()` tetap memakai `SharedPreferences` biasa dari UI sendiri.
- `AppLogHook.java` (**baru**) — lapisan AppLog yang sebelumnya dinyatakan mustahil.
  Modul Zygisk menang AppLog di syscall (PLT-hook `open`/`read`); Xposed tidak bisa.
  Tapi SDK ByteDance membaca `shared_prefs/applog.xml` + `snssdk_openudid.xml` lewat
  **SharedPreferences Java**, jadi hook `SharedPreferencesImpl.getString` menjangkau-nya.
  Pencocokan key adalah port eksak `patch_applog_xml` (substring case-insensitive,
  urutan prioritas `clientudid` sebelum `did`).
- `SeedId.java` — ditambah `applogSeed()` (mengikut package — **bukan** `personaSeed()`,
  kesalahan yang akan diam-diam menghasilkan ID salah), `applogIds()`, `snowflake()`,
  `uuid()`, `fillBytes()`.
- `AndroidManifest.xml` — tambah `xposedsharedprefs` + launcher Activity.
- `assets/devices.tsv` — 40 baris device dibundle.

**Verifikasi numerik** (bisa dilakukan tanpa toolchain, dengan menjalankan C++ asli
sebagai referensi):

| Yang dicek | Hasil |
|---|---|
| AppLog seed + 6 ID (did/iid/ssid/cdid/clientudid/openudid) | **cocok persis** dengan C++ referensi di input yang sama |
| `build_utc_from_patch` (`2023-08-05` + `230817V2345`) | `1690688135` di kedua sisi |
| Snowflake unsigned-64 | `Long.toUnsignedString` wajib — `v ≥ 2^63` untuk epoch ≥ 2208988800000 |
| Round-trip konversi tanggal Hinnant (termasuk 2024-02-29) | OK |

**Yang masih tidak terjangkau** (didokumentasikan, bukan disembunyikan): raw file
`files/bd_setting/*` + `.cdid` (dibaca native `libbdtracker.so`), store MMKV, `/proc`
`/sys`, hook `__system_property_*` native, mount-hide, dan `pm clear` otomatis (tanpa
root, pengguna harus swipe app dari recent sendiri — UI menjelaskannya).

**Timing tetap berbeda dari Zygisk:** `handleLoadPackage` jalan setelah class load, jadi
`static final` sudah membekukan nilai asli. Modul Zygisk menambal di
`postAppSpecialize` sebelum class-init. Celah pertama yang harus diuji per-target saat
ada toolchain.

**Yang BELUM diverifikasi:** kompilasi, perilaku runtime, dan apakah `XSharedPreferences`
benar-benar resolve di konfigurasi ini. Semua butuh toolchain Android.

### Flavor `tt` — build khusus TikTok, tanpa menghapus arsitektur generik

Permintaan: "buat module khusus TikTok, hapus dukungan app lain". Yang ditemukan saat
diaudit: **target system modul ini sudah fully generic**. `data/target.txt` sengaja
**kosong 0 baris** (module idle sampai diisi — safety by design), gate ada di
`jni/companion.cpp:106-112` (`is_target`) + `jni/module.cpp:65`, dan nama package
hanya mencapai kode produksi di **satu titik**: `module.cpp:170-171` → `g_pkg` →
`module_hooks.cpp:163` (`applog_seed`). **Tidak ada satu pun nama package hardcode
di kode produksi** — TikTok hanya muncul di README, `tests/host/sbx_pure.cpp` (test),
dan satu string WebUI. Jadi "TT-only" bukan penghapusan kode, melainkan **preset +
rebrand**:

- `build.sh` — dimensi baru `FLAVOR=full|tt` (terpisah dari `VARIANT`). Saat `FLAVOR=tt`:
  menulis `$PKG/sbx_flavor`, menulis preset `target.txt` **hanya bila `data/target.txt`
  kosong** (hormati konfigurasi live yang di-preserve `customize.sh` saat reinstall),
  rebrand `name=`/`description=` di `$PKG/module.prop` saja — **bukan** `module.prop`
  sumber, karena CI membaca version dari sana dan `tools/verify_packages.sh`
  menolak marker `[DEBUG]` di nama release (flavor TT tidak mengganggu guard rail itu).
- `data/target.tt.example` (file baru) — daftar blok ByteDance yang tersedia:
  TikTok Global `com.zhiliaoapp.musically`, Lite `com.zhiliaoapp.musically.go`,
  regional `com.ss.android.ugc.trill`, Douyin `com.ss.android.ugc.aweme`, dengan
  komentar. Pengguna salin baris ke `target.txt`.
- `sh/lifecycle/customize.sh` — deteksi `$MODPATH/sbx_flavor` untuk pesan install yang
  sesuai flavor + `set_perm` file baru.

### GAID rotator dihapus — serangan ke GMS, TikTok tidak memakainya

`set_gaid_value` dihapus dari `rotate_ids.sh` (52 baris), beserta dispatch `gaid`,
alias, baris usage, dan kemunculannya di `all`/`safe`. `all` sekarang
SSAID + wlan MAC + BT MAC + nama + applog; `safe` BT MAC + nama + applog.
Card GAID di WebUI (`webroot/app.js` ROT_CARDS + `index.html` rot-note) dihapus.

Alasannya bukan scope "TT-only", melainkan serangan-permukaan: rotatornya menulis ke
data dir `com.google.android.gms` dan `force_stop com.google.android.gms`
(`rotate_ids.sh:53` lama) — mengganggu Play Services di seluruh perangkat, dan TikTok
tidak mengambil GAID dari GMS.

**Kontrak identitas tidak patah:** kunci `GOOGLE_AID` **tetap di-generate** di
`native/sandboxid.cpp:468` (uuid_v4) agar `identity.prop` schema stabil untuk apa pun
yang mem-parsenya; hanya **penerapannya** yang hilang, bukan kuncinya.

### Carrier layer DIPERTAHANKAN setelah audit — ternyata fully live

Rencana awal menghapus `carriers.tsv` (143 → 1) dan subcommand `carrier`, dengan asumsi
komit `ce6c1ba` ("retire wireless and carrier identities") sudah memensiunkan layer itu.
**Asumsi itu salah**, dan penghapusan dibatalkan setelah diverifikasi rantai pemakaiannya
secara live:

```
native/sandboxid.cpp:616  merge_carrier
   → native/include/carrier.hpp:55-58  kv["GSM_OPERATOR_NUMERIC/ALPHA/ISO"]
   → native/include/prop_defs.hpp:176-183  gsm.operator.numeric / .alpha / .iso-country
   → jni/prop_hooks.cpp  (di-hook)
```

`ce6c1ba` hanya **memindahkan** `sbx_carrier.hpp` → `native/include/carrier.hpp` dan
menghapus test lama — tidak ada satu pun konsumen di atas yang ikut. `gsm.operator.*`
adalah input utama fingerprinting **negara** TikTok. `carriers.tsv` 143 row, subcommand
`carrier`, dan `carrier.conf` semuanya tetap utuh.

### 1-klik = perangkat baru: flow sudah ada, sekarang didokumentasikan

Flow 1-klik **sudah merge** sebagai `fe71567` (2026-09-10) dan tidak diubah di sini.
`action.sh` = acak persona (`autopif.sh device` dari `devices.tsv`: 40 perangkat,
8 merek) → `device.identity` → `identity.prop` → `sandboxid unlock/seed/lock` →
`pm clear` per target → `rotate_ids.sh all` → status AppLog per target → ringkasan
persona. Yang ditambahkan murni dokumentasi: diagram flow baru di README
("One click = a new device") + catatan eksplisit apa yang flow ini **tidak** lakukan
(lihat entry risiko di atas — `new_user:1` adalah sisi **berisiko**, jangan
mengubah `applog` ke wipe-only).

Satu string tersisa diperbaiki: `action.sh:116` masih menyebut "GAID" di daftar
rotasi padahal `rotate_ids.sh all` sudah tidak memutarnya.

## v2.2.9 (2026-09-16)

### Fix: mekanisme yang didokumentasikan untuk `pltHookCommit()` salah — perilaku kode sudah benar

Diferifikasi langsung terhadap tiga sumber hulu, dan klaim yang ditulis sebelumnya
tentang *mengapa* commit bisa false-padahal-hook-hidup **salah pada
mekanismenya**, meskipun kesimpulan yang diambil kode sudah tepat:

- `LSPosed/LSPlt` `lsplt.cc` — `HookInfos::DoHook()` melipat setiap GOT slot dari
  setiap pendaftaran menjadi satu AND (`res = DoHook(addr, …) && res`), dan
  `CommitHook()` mengembalikan `true` saat tak ada yang terdaftar serta `false`
  saat scan maps kosong.
- `PerformanC/ReZygisk` `loader/src/injector/hook.c` — `api_plt_hook_commit_v4()`
  mengembalikan `!any_failed`, di mana satu `plti_add_hook()` yang gagal saja
  menyetel flag-nya. Slot v4-nya **memang** `bool`, bukan `void`.
- `topjohnwu/Magisk` `native/src/core/zygisk/module.cpp` —
  `ZygiskContext::plt_hook_commit()` mendelegasikan ke `lsplt::CommitHook()`.

Jadi `false` adalah **laporan kegagalan parsial atas seluruh batch** — satu simbol
yang tidak diimpor oleh satu *library* saja sudah membaliknya, sementara pendaftaran
lainnya hidup dan bekerja. Kode sudah memperlakukannya seperti itu sejak awal
(membaca ulang pointer `orig_*` sebagai *ground truth* per-hook), jadi **tidak ada
perilaku yang berubah** — yang diperbaiki adalah komentar di `jni/module_hooks.cpp`,
dua pesan diagnostik di `sh/debug/summarize.sh`, dan referensi di `CREDITS.md`.

Catatan jujur: klaim yang salah menyebut ReZygisk mendeklarasikan slot v4 sebagai
`void`. Itu benar untuk **`exempt_fd`** (sangat terverifikasi: `void (*)(int)` di
union dengan `plt_hook_exclude`, dan `api_exempt_fd()` berakhir dengan `return;`
polos), tapi tidak untuk `plt_hook_commit`. Mekanisme baru yang sekarang
didokumentasikan justru memperkuat pilihan desain yang ada: karena bool-nya adalah
batch AND, ia tidak pernah bisa menyatakan hook mana yang mendarat, sehingga probe
tramplin adalah satu-satunya jawaban yang benar.

### Fix: empat diagnosis yang dilaporkan angka bersih padahal salah — tinjauan ulang adversarial

Satu pass review kedua atas diff yang sama, dengan tiga reviewer paralel dan
verifikasi adversarial per temuan, menemukan bahwa empat laporan di
`sh/debug/summarize.sh` dan satu di C++ **secara terstruktur selalu mencetak
nol atau selalu diam** — bug class yang persis sama dengan yang sudah diperbaiki
sebelumnya, di sisi lain dari batasnya.

1. **`exemptFd() returned true` selalu 0.** Satu-satunya baris di seluruh repo
   yang mengandung `exemptFd(fd=` adalah cabang *kegagalan* di `jni/module.cpp`,
   jadi `grep -cv 'returned false'` terbukti 0 di setiap perangkat, termasuk
   yang exemptFd-nya berhasil untuk setiap target. Ditambahkan LOGD di cabang
   sukses (`jni/module.cpp`), dan baris diubah ke `grep -c 'returned true'`.
2. **`typed get spoofed (SPB/SPI/SPL)` menghitung suppress juga.** Pola
   `'TYPED SPB |TYPED SPI |TYPED SPL '` diakhiri spasi telanjang; baris spoof
   adalah `TYPED SPB 'key'` dan baris suppress adalah `TYPED SPB SUPPRESS
   'key'`, jadi keduanya cocok — setiap key yang ditekan menggembungkan hitungan
   satu, dan terhitung kedua kalinya di baris suppress di bawahnya. Selain itu,
   tanpa tanda kurung hanya alternatif pertama yang terjangkar ke tag
   `${MOD}`; dua alternatif lain cocok di mana saja di log. Diperbaiki ke
   `TYPED (SPB|SPI|SPL) '` (diverifikasi: 3 spoof + 1 suppress sekarang
   mencetak 3, bukan 4; baris fallback `TYPED SPI(id)` tetap dikecualikan).
3. **Catatan "trampolines landed" tidak pernah bisa tercetak.** Rantai `elif`
   menguji keberadaan baris *install* (`NR_OK`) lebih dulu, tapi LOGW
   false-negative di `jni/module_hooks.cpp` **fall-through** ke LOGI install
   yang tidak bersyarat — jadi setiap sesi yang punya baris troll juga selalu
   punya baris install, dan cabang pertama selalu menang dengan `:`. Sesi
   persis yang ditujukan untuk catatan itu (commit "gagal" 8/8 sementara 348
   bacaan prop asli di-spoof melalui hook yang sama) melaporkan verdict yang
   bersih dan diam. Troll sekarang diuji lebih dulu; ketiga kasus (sehat ->
   diam, false-negative -> NOTE, gagal total -> "L9 is OFF") diverifikasi
   dengan log sintetis.

**Pelajaran yang sama, sisi lain:** sebuah diagnosis yang selalu nol atau selalu
diam tidak bisa dibedakan dari yang benar hanya dengan membaca outputnya. Setiap
satu dari ini ditemukan dengan *menjalankan* marker yang benar-benar dipancarkan
melalui script, bukan dengan membaca scriptnya.

### Fix: crash handler bisa overflow stack buffer + salah attribution — tinjauan native

Ditemukan oleh reviewer native dan dikonfirmasi independen dengan reproduksi
standalone:

1. **Stack buffer overflow di dalam signal handler.** `sbx_fmt_hex` /
   `sbx_fmt_dec` (`jni/module_hooks.cpp`) menulis tanpa tahu sisa kapasitas,
   sementara `put` hanya menjepit `p` di 191. Nama proses adalah input
   manifest yang tidak dibatasi (`args->nice_name` — AOSP `Zygote` hanya
   memotong *thread* name 16-byte, bukan nice_name), jadi nama ≥145 char
   membuat hex menulis hingga 14 byte melewati `char line[192]` **di dalam
   handler SIGSEGV** — titik terburuk untuk overflow, dan di build dengan
   `-fstack-protector-strong` canary meledak jadi SIGABRT yang handler ini
   juga pegang, jadi handler re-entrant. **Diverifikasi dua arah:** kode lama
   overflow di panjang nama 149; kode baru terikat di [0..190] untuk semua
   panjang 0..400. Kedua formatter sekarang menerima kapasitas dan memotong.
2. **False-positive crash marker untuk fault yang ART tangani.** Handler ini
   adalah SIGSEGV handler terluar di app process (dipasang di
   postAppSpecialize, di atas FaultManager ART di zygote), jadi ia jalan lebih
   dulu untuk *implicit null check* dan *stack-overflow guard page* ART —
   yang fault, ditangani, lalu dilanjutkan. Bukan crash. Setiap Java NPE di
   target app jadi tercatat sebagai crash module. Sekarang hanya SIGABRT
   (yang selalu fatal — ART melaporkan fault yang tak bisa ditangani via
   `Runtime::Abort()`) dan SIGSEGV-tanpa-handler-pendahulu yang direkam;
   fault yang bisa dilanjutkan langsung diteruskan ke handler ART.
3. **Komentar menjanjikan jaminan yang tidak ada.** Komentar lama bilang
   "jika crash di dalam liblog dan deadlock, debuggerd tetap menghasilkan
   tombstone" — itu terbalik: sinyal sudah terkirim, tidak ada re-fault, jadi
   proses *hang* tanpa tombstone, yang lebih buruk dari buang-sunyi yang
   ingin diperbaiki. `jni/companion.cpp` sendiri sudah mendokumentasikan bahwa
   `__android_log_*` mengambil lock internal liblog. Komentar sekarang
   menyatakan tradeoff yang sebenarnya, dan exposure dipersempit dengan hanya
   memanggilnya di jalur fatal (poin 2), bukan setiap SIGSEGV.
4. **Warm-up setelah arm.** `sigaction()` dipanggil sebelum
   `__android_log_print` yang ada untuk menghangatkan liblog — kebalikan dari
   urutan yang diklaim komentar sendiri. Ditukar.

### Fix: dokumentasi yang keliru — jalur on-device, waktu capture, cakupan lapisan

- **Jalur summarize.sh salah.** `build.sh` meratakan `sh/debug/summarize.sh`
  ke **root** modul (`$PKG/`), dan kedua konsumer on-device memanggilnya dari
  sana (`sh/lifecycle/action.sh`, `sh/lifecycle/customize.sh`). README
  menyuruh `sh /data/adb/modules/sandboxid/sh/debug/summarize.sh` — perintah
  diagnostik utama, yang akan "not found". Dikoreksi ke
  `/data/adb/modules/sandboxid/summarize.sh`.
- **"sebelum aplikasi target apa pun spawn" tidak benar.** `service.sh`
  menunggu `sys.boot_completed`, tidur, baru *clear buffer* lalu mulai
  `logcat` — sekitar 13 detik setelah boot selesai, dan apa pun sebelum itu
  dibuang oleh clear. README sekarang menyatakan keduanya, dan menyuruh user
  me-relaunch app sendiri.
- **"fallback ke pre-warm layer 3" hanya benar untuk 1 dari 7.** Layer 3
  hanya bind-mount 8 path `build.prop` + `settings_secure.xml`
  (`BIND_ENTRIES` di `native/include/config.hpp`). `/proc/cpuinfo`,
  `/proc/meminfo`, `/sys/class/net/*/address`, `/proc/sys/...` tidak punya
  bentuk on-disk untuk di-pre-warm — library yang dimuat setelah specialize
  membaca byte asli di path itu. Itu satu-satunya celah residual di lapisan
  ini; README sekarang mengatakannya apa adanya alih-alih mengklaim tertutup.
- **Klaim carrier terbalik.** README bilang "carrier asli menang via
  native/getprop", padahal `gsm.operator.*` dan `gsm.sim.operator.*` ada di
  setelah prop yang di-spoof (`native/include/prop_defs.hpp`) dan dibaca lewat
  hook yang sama baik dari Java maupun native — mismatch itu justru sudah
  diperbaiki dengan meng-hook library milik app sendiri. Carrier asli hanya
  menang di luar proses yang di-hook (proses lain, `system_server`, RIL,
  shell `getprop`) karena RIL menimpa prop secara terus-menerus.
- **Cakupan cpuinfo dan verdict summarize.** Rebuild cpuinfo bukan
  Tensor-only — itu semua persona non-Qualcomm/non-MediaTek (`cpu_action_for`
  kembali `CPU_STRIP`), dan platform yang tidak dikenal jatuh ke core block
  G4; README sekarang menjelaskan keduanya. Janji "layer yang diam-diam mati
  muncul sebagai peringatan" hanya berlaku untuk L9 dan layer 2, yang memang
  punya heuristik liveness — dipersempit kalimatnya.
- **Komentar shim `tests/host/include/android/log.h`** menyebut empat konstanta
  prioritas (padahal lima) dan menyebut `-DNDEBUG` sebagai gerbang
  (padahal `SBX_DEBUG`, lihat `jni/module_impl.hpp`). Komentar sekarang
  akurat; `__android_log_write` juga terdaftar karena crash handler
  memanggilnya langsung.
- **Test determinisme yang tidak bisa gagal.** `cpuinfo_synth(x) ==
  cpuinfo_synth(x)` membandingkan fungsi terhadap dirinya sendiri dengan
  argumen byte-identik — hanya bisa gagal jika ada memory yang belum
  diinisialisasi. Properti yang sebenarnya penting (dua device tidak boleh
  collid di derived variant/revision) sekarang di-assert terhadap seed kedua.

### Fix: layout cluster CPU persona Tensor salah untuk generasi 9-core

`native/native_read.cpp` memetakan cluster dengan lebar mid **tetap 3**:
satu prime, sampai tiga mid, sisanya little. Tapi Tensor G1/G2 adalah
1 prime + 3 mid + 4 little (8 core), sementara G3/G4 adalah 1 + 4 + 4
(9 core). Persona zuma/zumapro di perangkat 9-core jadi disintesis sebagai
**1+3+5** — kombinasi core-count per-cluster yang tidak dimiliki generasi
Tensor manapun, tepat jenis self-consistency tell yang ingin dihapus.
`mid_width` sekarang per-generasi; saat core count real cocok, layout yang
dihasilkan sekarang mereproduksi silicon asli (gs101/gs201 -> 1+3+4,
zuma/zumapro -> 1+4+4).

### Fix: probe companion socket sekarang membedakan EOF dari error sementara

`companion_socket_alive()` mengembalikan false untuk errno apa pun selain
EAGAIN, dan LOGW-nya menyatakan socket "verifiably dead". Untuk jalur yang
realistis (1-byte `MSG_PEEK` di `AF_UNIX`) memang selalu EOF atau EAGAIN —
verifikasi adversarial menunjukkan *harm*-nya tidak tercapai — tapi diagnostic
yang mengklaim diagnosis harus didasarkan pada apa yang benar-benar
diobservasi. Probe sekarang tri-state (`OPEN` / `CLOSED` / `UNKNOWN`), LOGW
hanya mencetak saat `recv()` benar-benar EOF, dan `UNKNOWN` fail-open (round
trip tetap dicoba; error read/write asli yang melapor).


**Mengapa ini penting:** watchdog crash dipasang tepat supaya crash di kode
ter-hook bisa dikaitkan dengan package target. Tapi penandanya ditulis dengan
`write(STDERR_FILENO, ...)`, dan di proses app fd 2 **bukan** logcat —
`SetStdioToDevNull()` di `system/core/init/util.cpp` (AOSP master) `dup2()`
fds 0/1/2 ke `/dev/null` di second-stage init, zygote mewarisinya, dan setiap
app yang di-fork juga. Jadi handler itu *jalan*, tapi hasilnya dibuang diam-diam;
`sh/debug/summarize.sh` melaporkan "module-marked crashes: 0" padahal handler
benar-benar aktif. Kegagalan diam persis seperti yang ingin diberi tanda.

Mekanismenya bisa dibuktikan dari sumber AOSP (`SetStdioToDevNull()` di
`system/core/init/util.cpp`). Catatan kejujuran: log lapangan (1.9 MB, 268
spawn) menunjukkan `SandboxID CRASH` **0 kali** dengan 62 tombstone, tapi
**itu bukan bukti** — ke-62 crash itu semuanya `com.android.nfc`, sebuah
package yang REJECTED, jadi watchdognya memang tak pernah dipasang di proses
yang crash itu. Nolnya konsisten dengan kedua penjelasan; alasan
`/dev/null`-nya berasal dari sumbernya, bukan dari log.

Yang berubah:

- `sbx_crash_handler` sekarang juga memancarkan penandanya lewat
  `__android_log_write(ANDROID_LOG_FATAL, ...)`. Ini bukan anggota POSIX
  async-signal-safe, tapi ia tak memformat variadic dan di jalur yang sudah
  hangat hanya menulis ke socket logd yang sudah ter-connect. liblog
  dihangatkan dari konteks normal di `install_crash_watchdog()` — yang kini
  memanggil `__android_log_print` **secara eksplisit** (bukan `LOGD`, yang
  di-compile-out di build release) — sebelum sinyal apa pun bisa datang.
  `write(2)` tetap dipertahankan untuk biner CLI yang dijalankan dari shell.
- `sh/lifecycle/service.sh`: ekstraktor crash `awk '/CRASH|DEATH|LEAK/'`
  mencocokkan **nol** string yang pernah dipancarkan modul maupun platform,
  jadi `debug/crashes.log` selalu kosong selama bertahun-tahun. Diganti
  `index()` terhadap marker nyata (penanda modul, `Fatal signal`, banner
  tombstone, `Cmdline:`, `>>> pkg <<<`, frame `#NN pc`, `FATAL EXCEPTION`).
  `index()` dipilih bukan ERE agar banner literalnya tahan perbedaan dialek
  awk; register dump dan memory dump sengaja dilewatkan supaya ledaknya kecil.
- Mulai segera dengan `tail -F -n 0` (sebelum `logcat` sendiri start, bukan
  10 detik setelahnya) supaya baris awal tak terlewat; ledger di-bound 256 KiB
  / 2000 baris terakhir.
- `README.md` merujuk `sh/debug/collect.sh` yang **tidak ada** — mekanisme
  sebenarnya adalah file penanda `debug_variant` yang dibaca `service.sh` dan
  dibuat build debug oleh `build.sh`. Dokumentasi sekarang menjelaskan yang
  sebenarnya.
- Bug kelas yang sama ditemukan di tempat keempat: pola summarize.sh
  `'pltHookCommit\(\) reported failure'` tak akan pernah match karena baris
  yang dipancarkan memiliki `NATIVE_READ: ` di antara tag dan literal itu.
  Lalu pola `live trampolines` memakai `\([^)]*\)` yang berhenti di `)` pada
  `lib(s)`, sehingga angka trampoline selalu 0 meski hook hidup. Keduanya
  diverifikasi dengan baris sintetik lalu dites end-to-end.

### Fix: tiga lapisan yang diam-diam mati — terlihat dari log lapangan saja

**Mengapa ini penting:** modul ini punya empat lapisan yang harus setuju; kalau
satu mati tanpa kata, aplikasi melihat identitas campuran dan penyebabnya tak
terlihat. Ketiganya ditemukan **dari session log nyata**, bukan dari membaca
kode, dan semuanya berbentuk "lapisan terlihat sehat tapi sebenarnya tidak
melakukan apa-apa".

1. **`exemptFd()` false negative (layer-2 bind-mount).** Log: `exemptFd()
   returned false` **268 dari 268** spawn, `MOUNTS: failed` 8 kali, **0 mount
   sukses**. Penyebab: ReZygisk mendeklarasikan slot v4 `exempt_fd` sebagai
   `void (*)(int)`, bukan `bool (*)(int)` seperti Magisk/ZygiskNext — register
   return arm64 (w0) berisi sisa yang selalu dibaca false, padahal efek
   sampingnya (fd tetap hidup) bekerja. Fix sebelumnya yang mempercayai bool
   itu jadi mematikan layer-2 di semua provider. Sekarang `postAppSpecialize`
   memprobe socketnya sendiri (`recv(MSG_PEEK|MSG_DONTWAIT)` — EOF = socket
   mati, EAGAIN = hidup) dan baru memutuskan. Provider Magisk yang benar-benar
   false (socket benar-benar ditutup) tetap terdeteksi, dan sekarang
   dilaporkan akurat alih-alih menyalahkan companion.
2. **`pltHookCommit()` false negative (layer 9).** Log: commit "gagal" **8/8**
   padahal **348** native prop read berhasil di-spoof melalui hook yang sama.
   `pltHookCommit` di lsplt/ReZygisk bukan transaksi — slot GOT di-patch
   satu per satu tanpa rollback, jadi return false tetap meninggalkan hook
   hidup. Sekarang module tersebut membaca kembali pointer `orig_*` sendiri
   setelah commit sebagai ground truth, mempromosikan log sukses ke `LOGI`,
   dan `summarize.sh` menyajikan false-negative itu sebagai catatan, bukan
   baris nol.
3. **Library milik app tak ter-hook (layer 9).** Daftar hook hanya enam library
   sistem; `.so` milik app sendiri yang sudah ter-load (yang bisa memanggil
   `__system_property_read_callback` langsung) tidak. Ini akar lensa NATIVE
   `vdinfos` yang melihat nilai carrier asli. Sekarang scan tunggal
   `/proc/self/maps` mengumpulkan keduanya, dan keduanya terdaftar.

`summarize.sh` ditulis ulang sepenuhnya: setiap grep dipasangkan ke string
yang benar-benar dipancarkan, dan layer yang mati muncul sebagai WARNING
alih-alih baris 0 yang rapi.

### Fix: `/proc/cpuinfo` persona Pixel bocor MIDR SoC asli

Persona Google/Tensor punya cpuinfo tanpa baris `Hardware :` dan tanpa
`Processor :` (format upstream `arch/arm64/kernel/cpuinfo.c`), dan setiap core
melaporkan ARM implementer `0x41`. Patch baris `Hardware` saja — apalagi
`return real` saat tak ada baris itu — **menyisakan** MIDR SoC asli di dalam
file persona sendiri: implementer `0x51` (Qualcomm), part `0x805` (Kryo silver),
`0xd0d` (Cortex-A77). Itu kontradiksi langsung dengan `Build.MODEL`.

Dua bug sekaligus: kebocoran MIDR per-core, dan fail-open yang menyajikan file
asli utuh. `cpuinfo_synth()` sekarang membangun ulang seluruh file dari
platform persona (G1 `gs101` 0xd05/0xd0b/0xd44, G2 `gs201` 0xd05/0xd41/0xd44,
G3 `zuma` 0xd46/0xd4d/0xd4e, G4 `zumapro` 0xd80/0xd81/0xd82), mempertahankan
jumlah core, `Features`, dan `BogoMIPS` asli, dan **fail-closed** kalau file
tak ter-parse (tak ada angka yang dikarang). Output diverifikasi byte-demi-byte
melawan format upstream dan dump Pixel asli. 20 check baru di `tests/host`.

### Fix: sinkronisasi shell-layer + dokumentasi batas yang tak bisa diperbaiki

`persist.sys.device_name` (nama marketing yang dibaca dari property service)
tetap `"POCO F3"` setelah rotasi sementara surface lain sudah persona — sebuah
MISMATCH `vdinfos dev:persist_device_name`. `rotate_ids.sh` kini mengaturnya.
`autopif.sh` menulis `MOD_DEVICE`, `FOTA_OEM`, `GOOGLE_CLIENTIDBASE` yang
sebelumnya hanya diturunkan di C++.

`README.md` sekarang mendokumentasikan batas yang **struktural**, bukan bug:
hardware attestation (TEE menjawab dengan rantai sertifikat asli; tak bisa
di-re-sign tanpa keybox terkompromi), property carrier (`gsm.operator.*`
ditulis RIL terus-menerus, jadi persona menang di lensa Java dan carrier asli
menang di native — MISMATCH, bukan kebocoran), identitas kernel/bootloader,
dan ketergantungan layer-2 pada provider yang menghormati `exemptFd()`.

## v2.2.8 (2026-09-16)

### Add: WebUI bisa melihat kesalahan JS-nya sendiri — konsol + runner shell

**Mengapa ini penting:** seluruh tujuan modul adalah membuat hal yang tak
terlihat menjadi teramati, tapi WebUI-nya sendiri buta. Kalau `identity.prop`
gagal diparse, atau fetch `carriers.tsv` melempar, halamannya cuma diam —
kegagalannya tak terlihat sama persis dengan identifier yang harusnya
ditampilkannya. Pola ini diambil dari repo yang paling dekat secara mekanisme
([AlirezaParsi/COPG](https://github.com/AlirezaParsi/COPG)) dan diterapkan
dalam idiom SandboxID sendiri.

Yang ditambahkan (`webroot/console.js`, tanpa CSS baru — dipakai ulang class
`.logbody` / `.ln` / `.lvl-*` yang sudah ada):

- **Capture** `console.log/info/warn/error`, `window.onerror`, dan
  `unhandledrejection` ke buffer 800 baris, ditampilkan sebagai source "Konsol
  WebUI" di halaman Log yang sudah ada.
- **Badge error** di tab Log yang tidak hilang sendiri; hanya pengguna yang
  bisa mengakuiinya (dengan mengetuk tab Log). `clear()` sengaja tidak
  menghapus badge.
- **Runner shell** di halaman Log. `ksu.exec` baru resolve ketika prosesnya
  **exit**, jadi `logcat` telanjang, `top`, `tail -f`, dan `getevent` tidak
  pernah selesai dan **mengunci bridge** sampai timeout. Runner menolak atau
  menulis ulang perintah itu (mis. `logcat` → `logcat -d`) daripada
  mengandalkan timeout — timeout itu jaring pengaman, bukan mekanismenya.
- **Simpan** log ke `debug/webui.log` lewat transport base64, bukan quoting
  shell: log bisa berisi karakter sama tajamnya dengan perintah shell.
- Kontrak utama: **jalur logging tidak boleh melempar ke pemanggilnya**. Kalau
  `render()` rusak, gejalanya akan jadi aplikasi crash di setiap pemanggilan
  `console`, yang lebih buruk daripada baris log yang hilang.

Tiga bug asli ditemukan dan diperbaiki oleh test harness saat menulis fitur
ini — bukan ditebak, tapi terbukti gagal dulu:

1. `add()` bisa melempar ke pemanggilnya saat `render()` rusak (guard re-entrancy
   hanya mencegah rekursi, bukan propagasi). Dibungkus `try/catch` penuh.
2. `ReferenceError: escapeHtml is not defined` saat `app.js` belum ter-load —
   tepat skenario "dua file bisa disajikan terpisah" yang diklaim komentar
   header-nya ditangani. Kini dijaga dengan `typeof`.
3. `MODDIR` tidak pernah menjadi properti `window` karena ia `const` level
   top — referensi telanjang melempar. Dijaga dengan `typeof` dan fallback ke
   path modul yang sebenarnya.

`logcat -s TAG:V` juga kini dapat `-d`: `-s` cuma spek filter, ia tetap
streaming tanpa henti, jadi menambahkan `-d` adalah perilaku yang benar.

**Test:** `tests/host/console_check.cjs` — 24 cek dijalankan terhadap
`console.js` asli di bawah DOM stub. Dimasukkan sebagai suite ketiga di
`tests/host/run.sh` (skip kalau `node` tidak ada — bukan failure, karena
suite C++ adalah yang harus selalu jalan).

### Fix: koreksi library hook I/O level Java — libjavacore.so, bukan libandroid_runtime.so

**Ini koreksi dari klaim yang keliru di v2.2.0/v2.2.1.** Pass 2 menambahkan
`libandroid_runtime.so` ke `kLibs` dengan alasan "semua I/O file level Java
melewati library ini". Setelah diverifikasi langsung ke source AOSP
(`platform/libcore`, `luni/src/main/native/`), klaim itu **salah**:

- Native `libcore.io.Linux` yang benar-benar melakukan I/O file level Java —
  `Linux_open`, `Linux_readBytes`, `Linux_preadBytes`, `Linux_lseek`,
  `Linux_close` — didaftarkan oleh `register_libcore_io_Linux` di
  **`libjavacore.so`** (lihat `Register.cpp`), bukan `libandroid_runtime.so.
- `android.system.Os` sendiri tidak punya JNI; ia mendelegasikan ke
  `libcore.io.Linux`.

Akibatnya, selama v2.2.0/v2.2.1, **surface baca file level Java sebenarnya
belum ter-hook** seperti yang diklaim. Diperbaiki: `libjavacore.so` (yang
di-load oleh setiap proses Java) kini ada di `kLibs`, dan komentar
`libandroid_runtime.so` dikoreksi — library itu membawa native
`android.os.SystemProperties` (surface identitas ketiga), bukan I/O file.

Validasi yang ikut terkonfirmasi: `Linux_lseek` membungkus **`lseek64**`,
bukan `lseek` — jadi hook `lseek64` yang ditambahkan di pass 2 memang
esensial untuk rewind level Java, bukan tambahan spekulatif.

### Fix: code review pass 3 — keamanan shell, webroot, dan test harness resmi

Review ketiga, kali ini memakai standar mattpocock/skills (`code-review`
two-axis + smell baseline Fowler, `codebase-design` deep-module, `tdd`
test-at-seam) sebagai lensa utama, di luar bug hunting yang sudah dilakukan
dua pass sebelumnya.

**Keamanan shell (root).**
- `helpers.sh` — staging file `applog_seed` ditulis di **dalam direktori app
  sendiri**, yang bisa ditulis oleh app itu sendiri. `> "$tmp"` mengikuti
  symlink, jadi app bisa menanam symlink di nama staging yang terduga dan
  membuat root menulis konten ke path pilihannya. Kini memakai `mktemp(1)`
  (O_EXCL, menolak path yang sudah ada — symlink atau bukan) dengan fallback
  yang tetap menolak path yang ada; bonus: file staging 0600 root-owned, tidak
  ada jendela di mana app bisa membacanya sebelum chown. Diverifikasi langsung
  ke source toybox (`toys/lsb/mktemp.c`): tanpa `-u` ia memanggil
  `mkstemp()` — file dibuat atomik dengan `O_CREAT|O_EXCL`, bukan sekadar
  mencetak nama seperti POSIX `mktemp` / GNU `-u`.
- `module_hooks.cpp` — pembacaan file real kini **dibatasi 1 MB**. `/proc/
  meminfo` dan `cpuinfo` ukurannya kernel (beberapa KB), tapi `applog.xml`
  ada di direktori app dan ukurannya dikontrol app; tanpa cap, hook bisa
  membuffer jumlah tak terbatas di proses app sendiri.

**Duplikasi yang dihapus (Fowler: Duplicated Code).**
- `helpers.sh` — loop resolusi `/data/data` vs `/data/user/0` muncul 4×
  (`applog_wipe`, `applog_seed`, `applog_regen`, `applog_probe`). Kini
  satu fungsi `pkg_data_dir()`; semua helper resolve lewat satu tempat, jadi
  perubahan lookup (mis. secondary user) sekali kerja.
- `webroot/app.js` — `shell()` adalah pure pass-through ke `exec()` (middle
  man, gagal deletion test) — dihapus; `run()` dan `safeExec()` yang hampir
  identik kini `safeExec` = `run` + toast, satu bentuk saja.

**Webroot: robustness & hardening.**
- `exec()` kini punya **timeout** (300s): perintah root yang tidak pernah
  memanggil balik sebelumnya meninggalkan tombol loading selamanya; callback
  yang telat di-drop dengan aman.
- `rotateCmd()` — key di-pass sebagai **argumen printf**, bukan
  di-interpolasi ke format string; formatnya single-quoted di shell, jadi
  key dengan quote bisa keluar dari string tersebut (defense in depth; key
  memang dari tabel statis hari ini).
- `loadLog()` — guard untuk log source yang tidak dikenal (sebelumnya
  `cmd` undefined diteruskan ke bridge).

**Test harness resmi (skill `tdd`: tests at seams).**
- Verifikasi host yang sebelumnya ad-hoc kini bagian dari repo:
  `tests/host/run.sh` membangun dan menjalankan dua suite — `sbx_pure` (fungsi
  murni) dan `sbx_hook_sm` (state machine fd asli dari `module_hooks.cpp`,
  menimpa byte `/proc` host ini secara nyata). 99 assertion, 0 gagal.
- CI mendapat job `host-tests` (di luar path release-critical, supaya
  keanehan runner tidak bisa memblok rilis).
- README: section baru "Host verification suites".

### Fix: full-repo code review — dua bug CRITICAL pada hook baca file + penyatuan tiga surface identitas

Review menyeluruh seluruh repo (C++, shell, docs) menemukan dua bug
**critical** pada lapisan hook baca file Zygisk, plus sekumpulan drift
antar-surface yang membuat satu identitas bisa bocor di satu surface tapi
tidak di surface lain.

**Critical 1 — chunked read membocorkan file asli (leak 100%).**
`hook_read` hanya menserve sintetik pada *chunk pertama*; chunk kedua dan
seterusnya jatuh ke `orig_read`, yang membaca posisi file asli yang sudah
bergeser. Akibatnya untuk file spoof di atas ~4 KB (build.prop per-app,
applog.xml), app mendapat header palsu + **isi asli** dalam satu stream yang
sama. Diperbaiki dengan *per-fd synth state machine*: `SynthFd { kind, buf,
off, ready }` melacak setiap fd, `synth_build()` mengisi sekali di luar lock,
dan `synth_serve()` menyerap posisi sendiri sampai EOF — sehingga read
berulang/chunked maupun `pread64` sama-sama konsisten.

**Critical 2 — deadlock pada read kedua (app hang).**
`hook_read` memegang `g_synth_mu` lewat `lock_guard` lalu memanggil
`synth_serve()` yang mengunci mutex non-rekursif yang sama. Read pertama
selamat, read kedua **hang** — terlihat sebagai aplikasi stuck di loading
screen. Diperbaiki dengan pola "decide under lock, act outside lock":
flag `need_build` ditentukan di dalam critical section, semua kerja
(buffer build, copy ke caller) dilakukan setelah lock dilepas.

**Penyatuan tiga surface (drift removal).**
Properti identitas harus konsisten di tiga tempat: resetprop global
(`NATIVE_PROPS`), build.prop per-app yang di-bind-mount, dan hook Java
`SystemProperties.native_get`. Sebelumnya surface Java memakai copy
~160-entry buatan tangan yang **hilang** `ro.product.bootimage.build.fingerprint`,
`ro.product.vendor_dlkm.build.fingerprint`, `ro.product.system_dlkm.build.fingerprint`,
seluruh prop `*_for_attestation`, serta menduplikasi
`gsm.sim.operator.iso-country` — sementara `ro.product.board` justru absen.
Sekarang `prop_to_identity_map()` di-generate dari tabel `NATIVE_PROPS` yang
sama dengan surface native, plus daftar `extras` terdokumentasi untuk prop
runtime (CPU ABI, Dalvik, density) yang sengaja **tidak** di-mount karena
akan merusak art runtime. Ketiga surface tidak bisa drift lagi.

**Determinisme AppLog lintas layer.**
Seed dan epoch AppLog kini punya satu sumber kebenaran di
`native_read.hpp` (`applog_seed` / `applog_epoch_or_default`); CLI
`applog-ids` dan hook in-process memakai fungsi yang sama persis, jadi
file di disk dan bacaan in-process tidak mungkin berbeda. `is_native_unsafe_prop`
yang sudah stale (mendaftar `ro.product.cpu.abi*` sebagai "tidak boleh di-spoof"
padahal prop tersebut memang di-mount) dihapus.

**Robustness hook.**
- Hook `pread64` kini ABI-neutral (`long long` offset, bukan `off64_t`)
  dengan fallback `::syscall(__NR_pread64, ...)`; offset negatif ditolak.
- `openat` resolve relative path via `readlink("/proc/self/fd/<dirfd>")`
  (bionic `open()` adalah wrapper `openat`).
- `close` menghapus tracking fd dulu sebelum mendelegasikan.
- Registrasi hook memakai tabel `HookReg` dan menghitung simbol yang gagal
  di-hook, bukan diam-diam melanjutkan.
- Crash handler ditulis ulang **async-signal-safe** (tidak ada
  malloc/stdio/lock; `write(2)` + format integer manual).
- Pembacaan properti memakai `__system_property_read_callback` (yang
  `__system_property_read` sudah deprecated).

**Docs & branding.**
- `README.md` — skema kolom `personas.tsv` (10 required + 6 optional, aturan
  Tensor vs non-Tensor, baris multi-brand) dikoreksi; section baru
  `devices.tsv and the multi-brand flow` mendokumentasikan precedence
  `device.identity` / `persona.override`; deskripsi AppLog dijawab ulang
  (hook in-process adalah yang authoritative, `applog_seed` adalah
  best-effort pre-warm yang memakai CLI yang sama); `git clone` URL
  dikoreksi ke casing repo sebenarnya (`Ilham311/SandboxID`).
- `build.sh` — blok AUTOPIF_REFRESH sekarang meneruskan env name yang
  benar-benar dibaca `autopif.sh`.
- `module.cpp` — `parse_blob` kini mem-trim `\r`, spasi, dan tab dari key.
- `config.hpp` — `STATIC_PROP_DEFAULTS` diperluas (props konstan retail);
  `MOUNT_PARTS`/`MOUNT_PARTS_N` yang tak terpakai dihapus.

Verifikasi: 15/15 test host state-machine lulus (chunked read 1658 byte,
EOF pada read kedua, fd tak terlacak diteruskan, pread64 repeatable &
non-consuming, boot_id 36-char uuid, applog XML cocok prediktor CLI,
close hook drop tracking). Seluruh TU bersih secara sintaks.

### Fix: code review pass 2 — build-breaking API misuse + leak vektor lseek

Review kedua pada lapisan hook. Paling parah: **working tree tidak bisa
dibangun sama sekali**.

**Critical — `pltHookRegister` return type salah (compile error).**
Zygisk API yang di-pin (`zygisk.hpp`) mendeklarasikan
`void pltHookRegister(...)`, tapi kode memeriksa nilai kembalinya
(`if (!api->pltHookRegister(...))`), sehingga setiap ABI gagal compile
dengan `invalid argument type 'void' to unary expression`. Diperbaiki:
registrasi sekarang menghitung simbol terdaftar tanpa menguji kembalian
(sebuah simbol yang tidak di-import library target memang tidak
di-redirect — itu aman, bukan kegagalan).

**Leak: `lseek` rewind membocorkan file asli (100% untuk file besar).**
Setelah synth buffer habis dikonsumsi, entri fd dihapus dari map. App yang
`lseek(0)` lalu membaca ulang (Java `RandomAccessFile.seek`, parser C yang
rewind) mendapatkan **byte asli kernel** karena tracking-nya hilang.
Diperbaiki: entri tetap ada sampai `close`; `synth_seek()` menjepit cursor
ke `[0, buf.size()]` berdasarkan offset laporan kernel (sehingga
`SEEK_SET`/`SEEK_CUR`/`SEEK_END` semua benar, dan seek melewati EOF
tinggal di EOF seperti file asli). Hook `lseek` **dan** `lseek64`
keduanya terpasang.

**Dead branch: kunci MAC salah (`WLAN_MAC` → `WIFI_MAC`).**
`rotate_ids.sh` mempersist MAC yang juga diprogram ke `wlan0` di bawah nama
`WIFI_MAC`, tapi hook membaca `WLAN_MAC` yang tidak pernah ditulis —
MAC persona tidak pernah dipakai, spoof MAC tidak konsisten dengan
interface fisik. Kunci dan komentarnya dikoreksi.

**Coverage gap: varian `_FORTIFY_SOURCE` dan I/O level Java.**
- `__open_2` / `__openat_2` (varian tanpa argumen mode yang di-inject
  compiler saat `_FORTIFY_SOURCE` aktif — default di Android NDK release)
  kini di-hook; tanpa mereka, `open()` app bisa lewat tanpa tracking.
  Logika resolusi `dirfd` diekstrak ke `synth_track_dirfd()` agar tidak
  terduplikasi antara `hook_openat` dan `hook_openat_2`.
- `libandroid_runtime.so` ditambahkan ke `kLibs` — semua I/O file level
  Java (`android.system.Os`, `FileInputStream` native) melewati library ini.

**Robustness lain.**
- `hook_system_property_read_callback` kembali awal bila pointer original
  belum terisi (race dengan instalasi hook) — sebelumnya crash null-deref.
- `uptime_hook` memakai `sbx_parse_longlong` (menolak suffix sampah seperti
  `"100abc"`) alih-alih `strtoll` tanpa validasi.
- `companion.cpp` — `memcpy` overlap diganti `memmove`.
- Komentar `MEMINFO` yang menyesatkan dikoreksi: untuk model tak dikenal,
  total asli **dibulatkan ke atas** ke tier GB marketing (nilai kB persis
  itu sendiri adalah fingerprint); tidak pernah di-pass-through.
- `(void)set;` untuk meredam `-Wunused-but-set-variable` saat `LOGD`
  ter-compile-out di release.

**Dokumentasi.** README mendapat section "layer 4" (in-process file-read
spoofing): daftar simbol yang di-hook, jaminan konsistensi per-fd, daftar
file yang di-spoof (`/proc/version`, `/proc/meminfo` + tier rounding,
`/proc/cpuinfo`, `boot_id`, MAC, `selinux/enforce`, file AppLog),
perilaku fail-closed, dan keterbatasan cakupan PLT (`.so` app sendiri
ditangani oleh pre-warm on-disk di layer 3).

**Verifikasi host.** 99 assertion lulus, 0 gagal:
65 check fungsi murni (`classify`, validitas MAC, determinisme AppLog
lintas dua entry point, `patch_applog_xml`/`patch_meminfo`/`patch_cpuinfo`,
`synth_proc_version`, helper uuid/hex/snowflake, hide-list) plus **34 check
state machine fd asli** — `module_hooks.cpp` di-include langsung sehingga
`synth_*`/`hook_*` yang diuji adalah kode produksi; hook harus menimpa
byte kernel asli `/proc/meminfo` dan `boot_id` host ini, termasuk:
read penuh + EOF, rewind `lseek` + re-read byte-identical, read chunked
7-byte, read pendek, passthrough fd tak terlacak setelah `close`,
resolusi `openat` relative path, `pread64` pada offset yang tidak
menggeser cursor sequential, dan **fail-closed** (identitas tidak lengkap
→ EOF, bukan byte asli). Seluruh 8 TU bersih di `-Wall -Wextra`.

### Refactor: source layout cleanup and dependency removal

- Removed the unused experimental Java-method hook stack and all related
  third-party integration, callback, generated-bytecode, build-option, and
  runtime code.
- Split sources by responsibility: `jni/` contains the Zygisk module and
  companion, `native/` contains the CLI, CMake project, and shared headers, and
  `sh/` contains lifecycle, identity, library, and debug scripts.
- `build.sh` still flattens shell scripts and data into the installed module
  root, preserving the Magisk/KernelSU/APatch runtime layout, binary paths, and
  four-ABI Zygisk package structure.
- Removed obsolete host tests, local validator tooling, and the architecture
  document; retained user-facing project, changelog, credit, and license files.

### Fix: CI shellcheck merah — direktif fungsional ikut terhapus saat purge komentar

Purge komentar menghapus baris yang tampak seperti komentar tapi sebenarnya
**direktif fungsional** untuk shellcheck, sehingga CI gagal:

```
customize.sh:2  SC2034 (warning): SKIPUNZIP appears unused
```

`SKIPUNZIP` dibaca installer Magisk (eksternal), bukan oleh script itu, jadi
`# shellcheck disable=SC2034` di atasnya wajib ada.

- **customize.sh** — direktif installer dipulihkan (tanpa prosa penjelasnya,
  hanya baris direktifnya).
- Seluruh tree revisi pra-purge disisir ulang untuk pola direktif lain
  (`shellcheck`, `SPDX`, `NOLINT`, `clang-format`, `eslint`, `@ts-`, `noqa`,
  `NOSONAR`, dll).

### Fix: `applog_seed` fail-closed + skema per-file (hasil code review)

- **helpers.sh** — uid app wajib terbaca. `stat -c '%u'` yang gagal/kosong kini
  membatalkan seed sebelum satu file pun ditulis, bukan melanjutkan tanpa
  `chown`. Sebelumnya file berakhir milik root (tidak terbaca uid app) tapi tetap
  dihitung sukses dan dilaporkan `[OK]`.
- **helpers.sh** — `_applog_own()` mengembalikan 1 kalau `chown` gagal, dan
  `_applog_put()` menghapus file yang gagal di-chown alih-alih meninggalkannya
  root-owned. Direktori (`shared_prefs`, `files`, `files/bd_setting`) yang gagal
  di-chown membatalkan seluruh seed. Hitungan ok/gagal dilaporkan terpisah;
  seed parsial mengembalikan 1.
- **helpers.sh** — `applog_regen` membedakan pesan sukses dan seed-gagal, jadi
  operator tidak lagi melihat `[OK]` untuk seed yang tidak mendarat.
- **helpers.sh** — skema per-file, bukan satu template dipakai bertiga.
  `applog.xml` hanya `device_id`/`install_id`/`ssid`/`cdid`; `snssdk_openudid.xml`
  hanya `openudid`/`clientudid`. `bd_device_info.xml` **tidak lagi di-seed** —
  di perangkat nyata isinya blob fingerprint dengan skema berbeda, jadi
  memalsukannya dengan map applog justru tidak autentik; file itu dibiarkan
  absen dan hook L9 menyintesis nilainya untuk pembaca native.
- Diuji di host: happy path 7 file dengan skema benar; `stat` gagal → 0 file
  ditulis, rc=1; `chown` gagal per-file → file tersebut dihapus, rc=1, laporan
  "3 ok, 4 gagal".

### Fix: penyebab UTAMA blank putih — `apply-boot` resetprop key driver grafis ke SELURUH sistem

Fix sebelumnya menutup jalur L9 (per-app, in-process). Ternyata ada jalur kedua
yang lebih besar dan **global**, dan inilah yang benar-benar memicu blank putih:

`sandboxid apply-boot` (dijalankan `action.sh` tiap kali user tekan Action)
memanggil `apply_native()`, yang menjalankan `resetprop -n` untuk daftar panjang
property — termasuk:

```
resetprop -n ro.hardware        <persona>
resetprop -n ro.product.board   <persona>
resetprop -n ro.board.platform  <persona>
resetprop -n ro.product.cpu.abi{,2,list,list32,list64} <persona>
```

`resetprop -n` menulis langsung ke property area yang dipakai **semua proses**,
bukan per-app. Nilainya berasal dari `autopif.sh`:

```
BOARD=$(col "$_row" 7)
HARDWARE=$BOARD
BOARD_PLATFORM=$BOARD
```

yaitu kolom 7 `devices.tsv` — `mt6789`, `mt6893`, `mt6895`, `pineapple`, dst.
Jadi setiap kali Action ditekan, seluruh sistem bisa saja dapat
`ro.board.platform=mt6789` **di HP Snapdragon**, atau `pineapple` di HP MediaTek.
Setelah itu setiap proses yang baru start:

1. `frameworks/native/opengl/libs/EGL/Loader.cpp` — `HAL_SUBNAME_KEY_PROPERTIES`
   (`persist.graphics.egl`, `ro.hardware.egl`, `ro.board.platform`) dibaca via
   `base::GetProperty()`. Loader coba `libEGL_mt6789.so`, gagal, set
   `failToLoadFromDriverSuffixProperty = true`, lalu `break`. Fallback tanpa
   suffix tidak ada di vendor Qualcomm/MediaTek, fallback wildcard di-gate
   `!failToLoadFromDriverSuffixProperty` → dilewati → `hnd == nullptr` →
   `LOG_ALWAYS_FATAL_IF` di RenderThread.
2. `hardware/libhardware/hardware.c` — `variant_keys[] = {"ro.hardware",
   "ro.product.board", "ro.board.platform", "ro.arch"}`; `hw_module_exists()`
   `access()`-cek `gralloc.<nilai>.so` / `hwcomposer.<nilai>.so` di `/odm`,
   `/vendor`, `/system` → meleset semua.
3. `ro.product.cpu.abilist*` global memutus pemilihan ABI
   `System.loadLibrary` / `nativeloader` bila persona beda ABI dari perangkat.

Ini juga menjelaskan kenapa gejalanya **acak**: kalau roll persona kebetulan
dapat platform yang sama dengan HP asli, app jalan normal; roll vendor lain →
blank putih. Dan menjelaskan catatan di entry v2.1.6 ("hang baru muncul setelah
fix rc=127 mendarat") — sebelum `b44f5f8`, `action.sh` gagal rc=127 sehingga
`apply-boot` **tidak pernah jalan**. Begitu resolusi binary diperbaiki,
`apply-boot` mulai jalan dan resetprop global inilah yang meracuni sistem.
Diagnosis uptime dan diagnosis wrapper L9 keduanya hilir dari sini.

Fix:

- **native/sandboxid.cpp** — `ro.hardware`, `ro.product.board`, `ro.board.platform`
  dan `ro.product.cpu.abi{,2,list,list32,list64}` dicabut dari daftar resetprop
  global `apply_native()`. Key yang nilainya memilih file driver / ABI untuk
  dimuat tidak boleh pernah ditulis system-wide. Key itu tetap dispoof di L1
  (`Build.BOARD`/`HARDWARE`/`SUPPORTED_ABIS`), L2 (`SystemProperties.get`), dan
  tetap ada di build.prop bind-mount per-app (`generate_mount_files`) — yang
  aman karena build.prop hanya dibaca sebagai file, bukan sumber property area.

### Fix: AppLog rotate bukan cuma print — seed disk + synth XML

Temuan saat review: `rotate_ids.sh applog` (dan `all`) memang menulis, tapi
hasilnya tidak pernah sampai ke app:

1. `applog_regen` **menghapus** `shared_prefs/applog.xml` dkk.
2. Redirect L9 `APPLOG_XML` butuh file aslinya ada — `sbx_read_real()` kosong →
   `return false` → jatuh ke open asli.
3. `SharedPreferencesImpl.loadFromDisk()` memanggil `mFile.canRead()`
   (yaitu `access(2)`) **sebelum** `open(2)`. `access`/`stat` TIDAK di-hook, jadi
   file yang sudah dihapus dianggap tidak ada dan `open` tidak pernah dipanggil.

Hasil bersihnya: setelah rotate, tidak ada satu pun ID yang dispoof sampai SDK
mendaftar ulang ke server dan menulis file baru. Persis "print doang".

Fix:

- **native/sandboxid.cpp** — subcommand baru `sandboxid applog-ids <pkg>` mencetak
  did/iid/ssid/openudid/clientudid/cdid yang **sama persis** dengan yang
  disajikan hook L9 (`fnv1a(FINGERPRINT|SERIAL|ANDROID_ID|pkg)` + `APPLOG_EPOCH`,
  lewat `sbxnr::make_applog_ids`). Satu sumber kebenaran untuk layer shell.
- **helpers.sh** — `applog_seed()` menulis `applog.xml`, `snssdk_openudid.xml`,
  `bd_device_info.xml`, `files/bd_setting/{device_id,install_id,openudid,
  clientudid}` dan `files/.cdid` dengan nilai dari `applog-ids`, lalu
  `chown` ke uid app, `chmod` 0660/0600, dan `chcon` mengikuti context data dir
  (pola yang sudah terbukti di `set_gaid_value`). `applog_regen` sekarang
  wipe → seed, jadi `canRead()` true dan setiap pembacaan berikutnya dipatch
  hook.
- **jni/module.cpp** — `sbx_build_content(APPLOG_XML)` menyajikan map hasil
  sintesis ketika file asli kosong/absen, bukan `return false`. Pembaca native
  (libbdtracker) tetap dapat nilai persona meski seed disk gagal.
- **native/include/native_read.hpp** — `applog_xml_synth()`; outputnya sengaja
  dibuat bisa diproses `patch_applog_xml()` lagi (idempoten, diuji).

### Fix: output `rotate_ids.sh` tidak pernah terlihat

`_log()` di helpers.sh memakai `tee -a "$LOGFILE" >/dev/null` — stdout dibuang.
`action.sh` menangkap stdout `rotate_ids.sh` ke file lalu men-tee-nya, jadi yang
tampil di WebUI hanya header "==> Rotasi ID lain ..." lalu **kosong**, padahal
semua langkahnya berjalan. Inilah yang membuat rotasi terlihat seperti gimmick.

- **helpers.sh** — `_log()` menulis ke stdout DAN logfile.
- **action.sh** — output rotasi di-append ke `debug/action.log` saja
  (`tee_action`), supaya tidak dobel di `$LOGFILE` yang sudah ditulis `_log`.

### Chore: hapus seluruh komentar dari kode

Semua komentar dibuang dari `*.sh`, `jni/*.cpp`, `jni/*.hpp`, `native/CMakeLists.txt`,
`webroot/app.js`, dan sumber native (−719 baris). Header kolom `*.tsv`,
`.gitignore` dan dokumen `*.md` dibiarkan: itu dokumentasi format data
positional yang dipakai `col "$_row" N`, bukan komentar kode.


### Fix: blank putih v2.1.7 — L9 spoof `ro.hardware` / `ro.board.platform` / `ro.product.board` memutus resolusi driver grafis

Entry v2.1.7 sebelumnya benar soal wrapper beracun, tapi fix-nya membuka bug
kedua yang lebih dalam. Kronologi lengkap dari v2.1.2 ke v2.1.7:

- **v2.1.2 (aman, dan ini alasannya):** `install_native_read_hooks()` melakukan
  `return` LEBIH DULU sebelum `g_nr_active = true` ketika `pltHookCommit()`
  gagal. Karena commit memang selalu gagal di app target (373 library), L9
  praktis **mati total** — semua wrapper yang sudah terpasang di PLT jadi
  pass-through murni lewat `orig_*` yang ditulis lsplt. App jalan normal karena
  tidak ada spoofing native sama sekali.
- **v2.1.5 / v2.1.6:** fce81a6 menambah reset fail-closed yang me-null-kan
  `orig_*` → wrapper live berubah jadi stub racun → blank putih.
- **v2.1.7:** a84d6c9 mencabut reset itu dan menyetel
  `g_nr_active = orig_spg || ...` → **untuk pertama kalinya L9 benar-benar
  AKTIF** di proses yang commit-nya parsial. Spoofing property native menyala,
  dan tiga key di `prop_to_identity_map()` ternyata adalah key pemilih file
  driver:

  | property | identity key | dipakai untuk |
  |---|---|---|
  | `ro.hardware` | `HARDWARE` | `variant_keys[0]` libhardware |
  | `ro.product.board` | `BOARD` | `variant_keys[1]` libhardware |
  | `ro.board.platform` | `BOARD_PLATFORM` | `variant_keys[2]` libhardware + `HAL_SUBNAME_KEY_PROPERTIES[2]` EGL |

Rantai kegagalannya, terverifikasi langsung dari sumber AOSP:

1. `hardware/libhardware/hardware.c` — `variant_keys[] = {"ro.hardware",
   "ro.product.board", "ro.board.platform", "ro.arch"}`, lalu
   `hw_module_exists()` menyusun `"%s/%s.%s.so"` → `gralloc.<nilai>.so`,
   `hwcomposer.<nilai>.so` di `/odm`, `/vendor`, `/system`. Nilai persona
   (mis. `gs201`/`zuma` milik Pixel) di HP Snapdragon/MediaTek → `access()`
   gagal di semua kandidat → HAL gralloc/mapper tidak ketemu.
2. `frameworks/native/opengl/libs/EGL/Loader.cpp` —
   `HAL_SUBNAME_KEY_PROPERTIES = {persist.graphics.egl, ro.hardware.egl,
   ro.board.platform}` dibaca via `base::GetProperty()`, yaitu
   `__system_property_read_callback` — **tepat simbol yang kita hook**. Loader
   mencoba `libEGL_<nilai>.so` / `libGLESv2_<nilai>.so`, gagal, lalu
   `failToLoadFromDriverSuffixProperty = true` dan **`break`** ("the value must
   be set correctly with the first property that has a value").
3. Fallback nama eksis (`libEGL.so` tanpa suffix) tidak ada di device vendor
   Qualcomm/MediaTek (mereka ship `libEGL_adreno.so` / `libGLES_mali.so`), dan
   fallback wildcard yang seharusnya menyelamatkan **di-gate oleh
   `!failToLoadFromDriverSuffixProperty`** — jadi dilewati.
4. `hnd == nullptr` → `LOG_ALWAYS_FATAL_IF(!hnd, "couldn't find an OpenGL ES
   implementation...")` di RenderThread. Pada app ByteDance, native crash
   handler (NPTH) menelan SIGABRT-nya → proses hidup, UI thread hidup,
   RenderThread mati, surface tidak pernah digambar → **blank putih tanpa crash
   signature**, persis gejala yang dilaporkan.

Fix:

- **native/include/native_read.hpp** — `is_native_unsafe_prop()`: daftar property yang
  nilainya dipakai native code untuk memilih file/ABI yang dimuat, jadi tidak
  boleh pernah dipalsukan in-process — `ro.hardware`, prefix `ro.hardware.*`
  (egl, vulkan, gralloc, hwcomposer, camera, …), `ro.product.board`,
  `ro.board.platform`, `ro.arch`, `ro.zygote`, `ro.vendor.api_level`,
  `persist.graphics.egl`, `ro.product.cpu.abi{,2,list,list32,list64}`, prefix
  `ro.dalvik.vm.isa.*` / `dalvik.vm.isa.*`.
- **jni/module.cpp** — gate baru `sbx_nr_spoofable()` dipakai `sbx_spg()`,
  `sbx_spr()`, dan `sbx_cb_tramp()`: key native-unsafe diteruskan ke
  implementasi asli tanpa diubah dan tanpa disembunyikan. Spoof L1 (`Build.*`)
  dan L2 (`SystemProperties.get`) **tidak berubah** — `Build.BOARD`,
  `Build.HARDWARE`, `Build.SUPPORTED_ABIS` tetap bernilai persona, karena di
  layer Java nilai itu inert (tidak ada yang memuat driver dari string Java).
  Yang dikembalikan ke nilai asli hanya pembacaan native.
- **native/include/native_read.hpp** — `classify()` tidak lagi mengalokasi
  `std::string p(path)` di setiap panggilan. Sejak `g_nr_active` benar-benar
  aktif, fungsi ini jalan di **setiap** `open`/`openat`/`fopen` di seluruh
  proses (termasuk linker saat memegang loader lock, RenderThread, dan thread
  GC ART); `malloc` di jalur itu adalah hazard reentrancy. Sekarang murni
  `strlen` + `memcmp` lewat overload `ends_with(const char*, size_t, const
  char*)`.
- **jni/module.cpp (L8)** — variabel `found` yang redundan (selalu sama dengan
  `registered`) dihapus; menghilangkan warning
  `-Wunused-but-set-variable` di build release.

Kill switch tetap tersedia: `SBX_NATIVE_READ=0` di identity blob mematikan L9
seluruhnya.

Catatan yang sengaja dibiarkan: `ro.build.version.sdk` masih dispoof di layer
native. Key itu tidak memilih file apa pun, dan `SDK_INT` sisi Java adalah jalur
deteksi utama. Kalau nanti persona dipakai lintas versi Android, key ini
kandidat pertama untuk ikut masuk daftar native-unsafe.

### Fix: ROOT CAUSE aplikasi target blank putih — wrapper L9 beracun saat pltHookCommit gagal parsial

Diagnosis uptime pada entry sebelumnya **salah** — log v2.1.6 (UPTIME_SECONDS=0,
`no_uptime` aktif) masih menunjukkan app hang di layar putih dengan UI thread
hidup dan tanpa crash signature apa pun. Penyebab sebenarnya:

1. `install_native_read_hooks()` mendaftarkan 7 simbol (open/openat/fopen +
   `__system_property_get/read/read_callback`) ke SEMUA .so yang ter-map —
   di app target MIUI itu 373 library.
2. `pltHookCommit()` mengembalikan **false** di setiap spawn (terlihat di log:
   "L9: pltHookCommit gagal (373 libs registered)").
3. Semantik lsplt (LSPosed/LSPlt `lsplt.cc`, `DoHook`): commit diterapkan
   **per-library dan TIDAK di-rollback** — `false` berarti "sebagian gagal",
   sementara ratusan entry PLT library lain SUDAH dipatch ke wrapper kita.
4. Fail-closed fce81a6 lalu me-reset semua `orig_*` ke nullptr → wrapper yang
   masih terpasang di PLT berubah jadi stub racun: `sbx_spg` mengembalikan
   string kosong untuk SETIAP property lookup native, `sbx_spr` return -1,
   `sbx_sprcb` tidak pernah memanggil callback caller-nya.
5. Pembaca property native (libcutils `property_get` → `__system_property_
   read_callback`, dipakai libhwui/EGL/renderer init) melihat `ro.hardware.egl`,
   `debug.hwui.*` dkk kosong → RenderThread gagal init → surface tidak pernah
   digambar → blank putih tanpa crash. UI thread tetap hidup (poll
   `debug.force_rtl` via L2/JNI terus berjalan) — persis signature di log.

Fix:

- **jni/module.cpp** — `orig_*` di-resolve via `dlsym(RTLD_DEFAULT, ...)` SEBELUM
  registrasi hook, sehingga wrapper selalu punya fungsi asli untuk ditelepon
  baik commit sukses penuh, parsial, maupun gagal total. Reset `orig_*` saat
  commit gagal DICABUT (itu justru racunnya). `g_nr_active` kini aktif selama
  orig resolvable: lib yang ter-hook dispoof, sisanya transparan lihat nilai
  asli — partial commit berubah dari "merusak app" jadi "best-effort".
- **jni/module.cpp (L8)** — hazard kembar dihapus: saat commit uptime gagal,
  offset di-nol-kan (bukan orig yang di-null-kan) supaya reader yang kebetulan
  sudah ter-hook melihat jam asli — menghilangkan divergence
  hooked-vs-unhooked yang jadi dugaan hang sebelumnya. `orig_clock_gettime`
  juga di-pre-resolve via dlsym.
- **autopif.sh** — bug newline heredoc: `$(cat <<EOF)` membuang trailing
  newline, jadi append `BUILD_TIME_UTC=` menempel ke baris terakhir heredoc →
  blob identitas nyata berisi `FLAVOR=caiman-userBUILD_TIME_UTC=1727839200`
  (FLAVOR rusak + key BUILD_TIME_UTC hilang, terlihat di log user). Append
  kini mulai dengan newline-nya sendiri.
- **Autopif acquisition check** — generate artifact autopif asli dan
  assert setiap baris tepat satu pasangan KEY=VALUE + `BUILD_TIME_UTC` jadi
  key tersendiri (cek ini gagal pada kode lama, lolos pada fix).

### Fix: aplikasi target blank putih (hang di loading screen) — uptime spoof kembali opt-in

Kambuhan masalah yang sudah pernah didiagnosis tuntas di `c67ae88`: aplikasi
target hang di layar loading putih setiap kali `UPTIME_SECONDS > 0`. Penyebab
**bukan Build.TIME** — itu derivasi tanggal build persona (1–6 hari sebelum
bulletin security patch), tidak berinteraksi dengan clock runtime.

Rantai kejadian:

1. `c67ae88` mematikan uptime spoof (default OFF) karena Java-side hook hang
   app. `115e799` menyalakannya lagi via PLT-hook `clock_gettime` di
   libutils + libandroid_runtime — terbukti stabil selama itu satu-satunya
   chokepoint.
2. `fce81a6` (Phase 3, v2.1.3+) **memperluas chokepoint ke `libbase.so` +
   `libcutils.so`** → makin banyak pembaca clock mendapat offset, sementara
   jalur lain (vDSO `clock_gettime`, kernel-side timed-waits) tetap real →
   divergence hooked-vs-unhooked kembali muncul → deadline loading/animasi
   yang di-seed dari nilai hooked tak pernah tercapai → blank putih.
3. Kenapa baru kambuh di v2.1.5: action.sh selama ini gagal rc=127 (symlink
   `bin/sandboxid` hilang) sehingga `identity.prop` lama terpakai — kemungkinan
   dengan `UPTIME_SECONDS=0` dari era opt-in-off. Setelah fix rc=127, action
   pertama kali berhasil apply identitas baru dengan uptime positif → hook
   L8 yang diperluas aktif untuk pertama kalinya → hang.

Fix:

- **autopif.sh** — `UPTIME_SECONDS=0` default; hanya emit nilai asli jika
  `$MODDIR/enable_uptime` ada DAN `no_uptime` tidak ada (kill switch tetap
  menang). `UPTIME_S` internal tetap valid untuk `validate_lifecycle`.
- **jni/module.cpp** — daftar chokepoint L8 dikembalikan ke libutils +
  libandroid_runtime saja (libbase/libcutils dicabut dengan alasan
  terdokumentasi). Tanpa reflash pun fix sudah efektif karena nilai default
  sekarang 0 → hook no-op.
- Mitigasi instan tanpa reflash: `touch /data/adb/modules/sandboxid/no_uptime`
  (companion memaksa UPTIME_SECONDS=0 per-spawn, live sejak v2.1.3).

### ByteDance AppLog IDs: in-process hook (L9) replaces disk seeding

`did` / `iid` / `ssid` / `openudid` / `clientudid` / `cdid` are now spoofed
in-process by the zygisk module instead of being seeded onto the app's data
dir. The old seeding needed `setenforce 0`, `chown`, `restorecon` and a
force-stop, and lost to the SDK's own writes; the hook is authoritative on
every read.

Formats are grounded in observed `device_register` traffic (real samples:
`6990234216324986369 >> 22` = 2022-10-24; iid `7137846409338136325` likewise
decodes to its registration date), not the previously-assumed
`(unix_seconds << 32)` shape:

- **`did` / `iid` / `ssid`** — int64 snowflake, 19 decimal digits,
  `(unix_ms << 22) | 22-bit random`, per package (each ByteDance app
  registers its own did)
- **`cdid` / `clientudid`** — UUID v4
- **`openudid`** — 16 hex chars
- **`APPLOG_EPOCH`** (identity.prop) — new key; all six IDs are
  deterministic in (persona, package, epoch), so bumping the epoch rotates
  them. `rotate_ids.sh applog` does exactly that (bump + wipe + force-stop);
  values stay stable between bumps.
- **XML patching** — `applog.xml` / `snssdk_*.xml` / `bd_device_info.xml`
  are read from disk and only identifier-matching entries are patched,
  preserving the SDK's own key names (`device_id`, `header_device_id`, …
  vary across versions). Raw `bd_setting/*` + `.cdid` are served
  synthesized. Known limitation: MMKV-backed stores (read-write mmap) are
  not covered.
- **Removed** — `helpers.sh::applog_generate` (incl. ~60 lines of awk
  decimal long-arithmetic and a bogus `arxiv:2504.13279` citation — that
  paper is about TikTok post sampling, not ID formats) and
  `helpers.sh::applog_seed`. `applog_probe` states simplify to
  fresh/active/absent.

### Review fixes (shell)

- `rotate_ids.sh all/safe` — the optional GAID argument leaked into
  `sync_device_name` (a UUID could become the device name); only
  `set_gaid_value` receives it now.
- `rotate_ids.sh all` — `randomize_wlan_mac` failures now count toward
  `FAILURES` like BT-MAC already did.
- `action.sh` — rotation warnings now surface every nonzero rc (rc=1, the
  common failure code, was explicitly suppressed).
- `jni/module.cpp` — fixed latent build break from `bool ok =
  api->hookJniNativeMethods(...)`: the pinned zygisk.hpp API returns void
  (failure is signalled by `fnPtr == null`).

### Fix: `action.sh` fails with rc=127 "binary native tidak bisa dijalankan"

`bin/sandboxid` does not exist in the zip — it is an install-time symlink
that `customize.sh` creates per ABI (`ln -sf sandboxid-arm64 …`). Installs
whose module dir was updated without a full re-flash (or that predate the
`ternak-tt` → `sandboxid` rename) can be missing it, which made
`action.sh` abort with rc=127 before applying any identity. All consumers
now fall back to the ABI-named binaries (`bin/sandboxid-{arm64,arm,x86_64,x}`
selected via `ro.product.cpu.abi`, `uname -m` as backup):

- `helpers.sh` gains `sbx_bin()` (also tries legacy `ternak-tt-<abi>` names);
  `action.sh` re-resolves `BIN` through it after sourcing helpers.
- `service.sh` and `post-fs-data.sh` resolve inline (they run standalone,
  before helpers.sh is sourced); the arm64-only fallback in
  `post-fs-data.sh` covered 32-bit devices incorrectly.
- `jni/companion.cpp` `try_seed_ondemand()` tries `bin/sandboxid` then the
  compile-time-ABI binary, with a clearer error when neither is runnable.
- `action.sh`'s failure message now lists what it looked for instead of a
  bare path, and warns early (before autopif) when no native binary is
  runnable at all.
- `webroot/app.js` — removed the unused `BIN` constant (dead code; the
  WebUI always goes through `action.sh`/`rotate_ids.sh`).

### Identity hooking-completeness pass (Phase 4, jni/)

`Build.TIME` was the last un-spoofed `Build` field and the typed
`SystemProperties.getInt/getLong` getters could still return the *real* device
value for keys the persona overrides. Both gaps are closed, and the autopif
(the primary `action.sh` path) now emits the same extended key set the C++
`freshen` path does, so all persona surfaces stay mutually consistent.

- **`Build.TIME` / `ro.build.date.utc` / `ro.build.date`** — identity now
  carries `BUILD_TIME_UTC` + `BUILD_DATE`, derived deterministically from the
  persona's security-patch bulletin date (builds are stamped 1–6 days before
  the bulletin, mirroring real Pixel build cadence). L1 sets the `TIME` static
  long field; L2 maps both props; `apply_native()` + `generate_mount_files()`
  + `autopif.sh` emit them.
- **Typed-getter identity consistency (L7)** — `native_get_int` /
  `native_get_long` now consult the identity map first: any spoofed prop whose
  value parses strictly as an integer returns the persona value, so
  `SystemProperties.getInt("ro.build.version.sdk")`,
  `getInt("ro.build.version.preview_sdk")`,
  `getInt("ro.odm.build.media_performance_class")` and
  `getLong("ro.build.date.utc")` no longer leak the real device values
  (previously only the String `native_get` path was spoofed).
- **`ro.build.flavor`** — new identity key `FLAVOR` (`<product>-<type>`, e.g.
  `oriole-user`). `Build.FLAVOR` is gone from the SDK but the property still
  ships on production builds and is read directly by fingerprint SDKs.
- **`ro.build.version.codename` / `all_codenames`** — pinned to `REL`
  (production value) in `STATIC_PROP_DEFAULTS`, `apply_native()` and the
  mount-overlay `build.prop` files.
- **`ro.product.{system,odm,product}.marketname`** — added to the L2 identity
  map, matching the per-part fallback chain the other product fields already
  had.
- **autopif.sh** — generated identity now also includes `VBMETA_DIGEST`,
  `FLAVOR`, `BUILD_TIME_UTC`, `BUILD_DATE` (was C++-`freshen`-only before;
  the primary path leaked the real build date and vbmeta digest).
- **`selftest.sh`** — new coherence checks #6 (`FLAVOR` == `PRODUCT-TYPE`) and
  #7 (`BUILD_TIME_UTC` numeric, within 2009→now) catch a broken persona before
  a target app sees it.

### Robustness

- **`jni/module.cpp` L9 `fopen` fallback** — if the `fopen` PLT slot fails to
  resolve but `open`/`openat` hooks are live, `fopen()` is now served through
  `orig_openat` + `fdopen` with a proper mode→flags mapping. Previously the
  fallback path returned `ENOSYS`, which could break apps doing legitimate
  read *and write* `fopen()` calls after a partial hook commit.
- **`jni/module.cpp` L2/L7 hook installation** — `hookJniNativeMethods()` return
  value is now checked (bool) in addition to the pending-exception check;
  failures are logged with layer tags instead of passing silently.
- **`companion.cpp`** — `target.txt` hot-reload now compares mtime with
  nanosecond resolution (`st_mtim.tv_sec/tv_nsec`); same-second edits are no
  longer missed by the seconds-only comparison.
- **Removed dead config `NO_TELEPHONY`** (`config.hpp`) — the constant and its
  comment described a TelephonyManager binder hook that was never implemented;
  nothing read it. Removing it prevents future confusion. GSM/SIM identity
  remains covered at the property layer (`gsm.*` spoof map + carrier merge).
- **`build.sh`** — fail-fast pre-flight for `cmake`/`zip`/`curl` (clear error
  instead of a mid-build crash), explicit error + hint on `zygisk.hpp` fetch
  failure, and `LICENSE` + `CREDITS.md` are now shipped inside every module
  zip for attribution transparency.
- All jni sources verified against `@FastNative` vs `@CriticalNative` calling
  conventions in AOSP `SystemProperties.java` (string-keyed getters are
  `@FastNative`, so the `(JNIEnv*, jclass, …)` hook signatures are correct;
  handle-based overloads are `@CriticalNative` and deliberately not hooked) —
  documented in `CREDITS.md`.

Dynamic persona pool: the compiled Pixel table is replaced by a runtime data
file, with an optional live refresh from Google's canary build data. Persona
selection, SoC/RADIO derivation, and SDK-matching are all preserved — behavior
offline is identical to before.

ByteDance AppLog SDK **full regen** (wipe → generate → seed): the SDK's
server-issued identifier trio (`did` / `iid` / `ssid`) plus `openudid` /
`clientudid` / `cdid` is now not merely wiped but replaced end-to-end. A
locally-generated plausible cache lands on disk so the app reads the seeded
values on next cold start as if they were its own persistent state — no
zero-value gap, no "device changed" telemetry event on re-registration.
Format matches ByteDance's actual shapes (arxiv:2504.13279 + reverse-engineered
RangersAppLog paths): Snowflake 64-bit int64 (18-19 decimal digits, top 32
bits = Unix seconds) for did/iid/ssid; UUID v4 for cdid/clientudid; 16-hex
for openudid. Wires into the 1-click flow (`action.sh` → `rotate_ids.sh all`)
so a single tap in the WebUI or the Action button produces a fully-fresh
hardware + AppLog persona ready to use, with no user input. Ship-idle
contract preserved: no-op with a friendly hint when `target.txt` is empty.

### Added

- `helpers.sh::applog_generate` — plausible 6-tuple generator. Produces a
  Snowflake-shaped 64-bit int64 for did/iid/ssid using awk int64-safe math
  `(now_seconds << 32) | rand32` (three independent random halves so the
  three IDs never collide), a UUID v4 for cdid/clientudid from
  `/proc/sys/kernel/random/uuid` (with a `/dev/urandom` hand-rolled fallback
  when the kernel path is unavailable — musl/older kernels), and 16 hex
  chars from `/dev/urandom` for openudid. Values are printed to stdout as
  `KEY=value` lines; the caller is expected to redact them from logs (the
  helper itself never logs them).
- `helpers.sh::applog_seed [pkg] [payload]` — writes the fabricated cache
  into the target app's data dir. Emits five files: `shared_prefs/applog.xml`
  (primary cache: did/iid/ssid/openudid/clientudid/register_time),
  `shared_prefs/snssdk_openudid.xml` (legacy SDK path for openudid/clientudid),
  `shared_prefs/bd_device_info.xml` (RangersAppLog v6+ unified path for
  cdid/device_id), `files/bd_setting/{device_id, install_id, openudid,
  clientudid}` (raw text files read by native `libbdtracker.so` bypassing
  SharedPreferences), and `files/.cdid` (legacy plain-text UUID). Ownership
  chowned to the package UID (sourced from the data-dir itself), permissions
  set to 0660, and `restorecon -R` re-labels every seeded file so the app's
  SELinux domain can actually read them.
- `helpers.sh::applog_regen [pkg]` — the orchestrator: force-stop → wipe old
  cache (via applog_wipe) → generate new values → seed valid caches. Handles
  both single-package and `target.txt` batch modes. Per-target recursion so
  a partial failure on one package doesn't leave others in an inconsistent
  wipe-without-seed state.
- `helpers.sh::applog_probe [pkg]` — read-only status: emits one line
  `<pkg> <count> <state>` where state ∈ {fresh, seeded, active, absent}.
  Never dumps identifier values. Consumed by `rotate_ids.sh status`, by
  `action.sh` for the post-run summary, and by the WebUI applog card.
- `rotate_ids.sh applog [pkg]` (aliases `bytedance`, `regen-applog`) — front
  end for `applog_regen`. Full regen cycle by default.
- `rotate_ids.sh applog-wipe [pkg]` (alias `wipe-applog`) — escape hatch:
  wipe-only, no seed. Kept for forensic scenarios where you want to observe
  the SDK re-register from scratch against the server.
- WebUI: new "AppLog ByteDance" card in the Rotasi tab. Renders per-target
  state (seeded / aktif / bersih / nihil) plus file count, populated
  asynchronously via `applog_probe` so it never blocks the initial paint.
  Included in "Rotasi semua". Values are never surfaced — the card only
  shows counts and state tokens.
- README `applog` section rewritten with exact ID formats (Snowflake bit
  layout referencing arxiv:2504.13279, UUID v4 sources, 16-hex openudid),
  the five seeded files with per-file purpose, and the ownership + SELinux
  restorecon steps. Covered-scope entry upgraded from "cache wipe" to
  "full regen cycle".

### Changed

- `rotate_ids.sh all` and `rotate_ids.sh safe` now call `regen_applog`
  (was `wipe_applog`) as the **last** step. Ordering matters: force-stop
  inside applog_regen kicks each target so on next cold start it reads
  the seeded did/iid/ssid — and by that point every hardware-layer
  identifier it would sample (Build.*, MAC, ANDROID_ID, GAID) has already
  been rotated. If regen ran first, a stray user-initiated relaunch
  before the other rotations landed would burn our seeded IDs on top of
  the stale hardware fingerprint.
- `action.sh` now emits a post-run "Status AppLog per aplikasi target"
  block summarising each target's applog cache state (seeded / aktif /
  bersih / nihil) plus file count. Privacy-safe: never dumps identifier
  values.
- WebUI "Acak perangkat baru" button (freshenBtn) now reloads the Rotasi
  tab if it's currently active, so the AppLog card refreshes to show
  the seeded state immediately without a manual tab switch. Its inline
  comment now documents step 5 of the action.sh flow (AppLog wipe +
  generate + seed).
- `rotate_ids.sh status` per-target AppLog line now shows both file count
  AND state token (was file count only), sourced from applog_probe.


- `personas.tsv` (repo root, shipped to `/data/adb/modules/sandboxid/`) — the
  curated **stable** Pixel pool (the same 14 entries that were compiled into
  `jni/pool.hpp`), now editable data instead of C++. Tab-separated, 10 columns:
  `model device product board platform sdk release id incremental security_patch`.
  `#`-prefixed lines are comments. This file alone reproduces today's behavior.
- `autopif.sh` — best-effort refresher that scrapes Google's public Pixel pages
  for the latest **canary** build per model and *upserts* those personas into
  `personas.tsv` (dedupe by codename). Adapted from dannycreations' `autopif.sh`
  (see [CREDITS.md](./CREDITS.md)); rewritten for on-device Android `sh`.
  - **No-op offline:** exits 0 without touching anything when the device has no
    `curl`/`wget` (the usual case), so the bundled stable pool stays in force.
  - **SoC allow-list:** devices whose codename doesn't map to a known platform
    (`gs101`/`gs201`/`zuma`/`zumapro`/`laguna`) are skipped, never guessed, so a
    spoofed `RADIO` / `ro.board.platform` can't go inconsistent.
  - Wired into `action.sh` as a best-effort step 0 (before `freshen`); can also
    run at build time to refresh the packaged pool via `AUTOPIF_REFRESH=1`.

### Changed

- `native/sandboxid.cpp` `gen_identity()`: loads the pool from `personas.tsv` at
  runtime (`load_personas()`), falling back to a small built-in list if the file
  is missing/empty. Selection logic (exact SDK match → `sdk<=dev` → lowest) is
  unchanged.
- `customize.sh` / `build.sh`: install and package `personas.tsv` + `autopif.sh`.

### Removed

- `jni/pool.hpp` — the compiled `SBX_POOL` table. Its data now lives in
  `personas.tsv`; `native/include/config.hpp` gains `PERSONAS_FILE`.

## v2.1.0 (2026-08-26)

SIM / mobile-operator identity. Adds a user-controlled way to study the operator
fields apps read (`gsm.operator.*` / `Build`-adjacent SIM props), with an
optional "phantom" mode that reports a SIM as present on an empty slot. Like the
rest of the module this ships **inert**: no carrier is applied until the user
picks one, and clearing it falls straight back to the built-in defaults.

### Added

- `carriers.tsv` (repo root, shipped to `/data/adb/modules/sandboxid/`) — a
  curated table of **140 real operators across 67 countries** sourced from the
  public MCC/MNC dataset. Tab-separated, 4 columns: `name mcc mnc iso`;
  `#`-prefixed lines are comments.
- `carrier` (alias `sim`) subcommand in `rotate_ids.sh`:
  - `carrier "MCC|MNC|NAME|ISO|PHANTOM"` — validate (MCC = 3 digits, MNC = 2–3
    digits) and write `carrier.conf` (atomic, `umask 077`), then persist the
    `GSM_OPERATOR_NUMERIC/ALPHA/ISO` keys (and `GSM_SIM_STATE=LOADED` when
    `PHANTOM=1`) into `identity.prop`.
  - `carrier off` — clear the selection and erase the carrier keys.
  - `carrier status` — print the active selection.
- WebUI **SIM** tab (`webroot/index.html` + `app.js`): country/operator dropdowns
  driven by `carriers.tsv`, a "phantom" checkbox, and Apply/Off buttons that call
  the `carrier` subcommand and reflect the live `carrier.conf`.
- `native/include/carrier.hpp` — pure, host-testable carrier-selection parsing
  (`parse_carrier_conf` + `apply_carrier`), split out so the shell and native
  paths agree on the "a valid selection sets the keys, anything else erases
  them" contract.
- New identity key `GSM_SIM_STATE` (default empty) with `gsm.sim.state` /
  `gsm.sim.state.ril` property mappings; `PHANTOM=1` maps it to `LOADED`.

### Changed

- `native/sandboxid.cpp` `merge_carrier()`: honors `carrier.conf` on `freshen` and
  on first-boot `seed`, folding the parsed selection into the generated identity.
- `jni/module.cpp`: added `ro.vendor_dlkm.build.fingerprint` /
  `ro.odm_dlkm.build.fingerprint` → `FINGERPRINT` mappings so the newer
  partition fingerprints stay consistent with the rest of the build identity.
- `customize.sh` preserves a live `carrier.conf` across upgrades; `build.sh`
  packages `carriers.tsv`.

### Notes

- This is a **property-layer** path. SIM presence/state read through the
  telephony binder (`TelephonyManager.getSimState`, `SubscriptionInfo`) is **not**
  covered here and would need a separate framework-hook layer.
- Native changes require a CI rebuild + reflash.

## v2.0.0 (2026-08-21)

Rebrand to SandboxID — a generic, privacy-research / education framing with no
built-in application targets and no product-specific defaults. Runtime behavior
(hook layers, companion IPC, crash watchdog, atomic writes, hot-reload) is
unchanged; this release is naming, cleanup, and default-emptying only.

### Renamed

- Module id `ternak_tt` → `sandboxid`; module path `/data/adb/modules/sandboxid`.
- Display name `Ternak TT` → `SandboxID`; native and companion log tags were normalized.
- CLI binary `ternak-tt` → `sandboxid`; native library `libternak_tt.so` → `libsandboxid.so`.
- `jni/pool_tt.hpp` → `jni/pool.hpp`; `jni/tt_config.hpp` → `native/include/config.hpp`.
- `jni/ternak-tt.cpp` → `native/sandboxid.cpp`.
- C++ namespace `tt::` → `sandboxid::`; macro prefix `TT_` → `SBX_`.

### Changed

- `jni/companion.cpp` `reload_targets_if_changed()`: removed the built-in default target list; an absent `target.txt` now yields an empty list (no-op path).
- `native/sandboxid.cpp` `load_targets()`: removed the default fallback; an empty `target.txt` returns an empty list.
- `native/sandboxid.cpp` `usage()`: rewritten to a neutral description.
- `native/sandboxid.cpp`: removed the unused `MODDIR` alias (fixes `-Wunused-const-variable` under `-Wall -Wextra`).
- `target.txt` now ships empty (0 lines) — the module is idle until the user adds packages.
- All comments stripped from source (`.cpp`, `.hpp`, `.java`, `.sh`, `.js`, `CMakeLists.txt`, workflow `.yml`); data literals (`SBX_POOL`, `VAL_DEFAULTS`, `STATIC_PROP_DEFAULTS`, `modem_prefix`) untouched.

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
  ber-SDK **terendah** + warning (bukan ambil sembarang). Komentar `jni/module.cpp` soal
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
  `config.hpp`; duplikatnya di `jni/module.cpp`/`companion.cpp` dihapus. Ditambah path
  `IDENTITY_BAK`/`MODE_FILE`/`RESETPROP` dan array `MOUNT_PARTS[]` yang dipakai CLI.
- **Kenapa:** Sebelumnya `jni/module.cpp` mendaftar 6 bind entry sementara `companion.cpp`
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
