# Ternak TT v1.0 — TikTok Zygisk Fresh Persona

> Fork slim dari Ternak v5.0 — di-strip khusus untuk TikTok.
> Pool = Pixel-only (SDK 33–36), hook = 6 layer, auto `pm clear` chained ke regenerate.

![Version](https://img.shields.io/badge/version-v1.0.0-blue)
![License](https://img.shields.io/badge/license-MIT-green)

---

## Fitur

| Layer | Target | Method |
|-------|--------|--------|
| L1 | `android.os.Build.*` (17 field) | Static field override |
| L2 | `SystemProperties.native_get` | `RegisterNatives` |
| L3 | `Settings.Secure.getString("android_id")` | `RegisterNatives` (v1.1: lsplant) |
| L4 | `AdvertisingIdClient$Info.getId` (GAID) | Stub, v1.1: lsplant |
| L5 | `WifiInfo.getMacAddress` / `getBSSID` | `RegisterNatives` → `02:00:00:00:00:00` |
| L6 | `TelephonyManager.getImei/getDeviceId/getSubscriberId/getMeid` | `RegisterNatives` → `null` |

**Bonus:** `freshen` auto chain `pm clear` + `am force-stop` untuk 3 package TT.

---

## Requirements

- Android 13+ (SDK 33+)
- Root: KernelSU / Magisk >= 26100 / APatch
- Zygisk: **ZygiskNext** atau **ReZygisk** (Magisk built-in Zygisk juga jalan)
- `resetprop-rs` binary di `prebuilt/` sebelum build

---

## Build (local)

```bash
git clone https://github.com/diru768/ternak-tt.git
cd ternak-tt

# Fetch Zygisk header
curl -L -o jni/zygisk.hpp \
  https://raw.githubusercontent.com/topjohnwu/zygisk-module-sample/master/module/jni/zygisk.hpp

# Drop resetprop-rs (extract from Magisk zip)
cp /path/to/resetprop prebuilt/resetprop-rs

# Set NDK path
export ANDROID_NDK_HOME=/opt/android-ndk-r26d

# Build all 4 ABIs + package flashable zip
./build.sh

# Output:
# dist/ternak-tt-v1.0.0.zip
```

Atau push tag `v1.0.0` — GitHub Actions bakal auto-build + release.

---

## Install

1. Download `ternak-tt-v1.0.0.zip` dari Releases (atau hasil `./build.sh`)
2. Install via KernelSU / Magisk manager → **Modules** → **Install from storage**
3. **Reboot**
4. Tap **Action** button di manager

---

## Usage

```bash
# Main action: rotate identity + wipe TT data
su -c /data/adb/modules/ternak_tt/bin/ternak-tt freshen

# Check current identity
su -c /data/adb/modules/ternak_tt/bin/ternak-tt status

# Rollback
su -c /data/adb/modules/ternak_tt/bin/ternak-tt rollback

# Lock (prevent freshen — kalau udah login akun target)
su -c /data/adb/modules/ternak_tt/bin/ternak-tt lock
su -c /data/adb/modules/ternak_tt/bin/ternak-tt unlock
```

---

## Scope

### ✅ Yang di-cover
- Device fingerprint (Build.*, native prop)
- ANDROID_ID, GAID (fully working di v1.1 dengan lsplant)
- WiFi MAC/BSSID zeroing
- Telephony IMEI/subscriber null-ing
- Auto TT app-data wipe

### ❌ Yang TIDAK di-cover (out of scope)
- X-Ladon/X-Gorgon/X-Argus/`ttreq` request signatures
- TLS/JA3 fingerprint
- Behavior automation
- Network layer (VPN, residential IP)

---

## Roadmap

- **v1.1** — bundle `lsplant`, sensor hook, `DEVICE_INITIAL_SDK_INT`
- **v1.2** — per-akun snapshot, auto-rotate scheduler
- **v2.0** — Douyin support, Shelter/Island integration

---

## License

MIT — see [LICENSE](./LICENSE)

---

## Disclaimer

Untuk **security research & educational purposes**. Penggunaan untuk melanggar ToS platform adalah tanggung jawab pengguna sendiri.
