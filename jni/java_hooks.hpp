#pragma once
#include <jni.h>
#include <string>
#include <map>

namespace ternak_tt { namespace java_hooks {

// Called once per target process. Attempts to load lsplant + helper dex.
// Returns true if hook infra is available (Path B enabled at build time AND
// runtime init succeeds), false otherwise. Fail-soft: never throws, never
// crashes the target.
bool Init(JNIEnv* env);

// Installs all currently-implemented Java hooks (Settings.Secure.getString,
// MediaDrm.getPropertyByteArray, Locale.getDefault, TimeZone.getDefault,
// SystemClock.uptimeMillis, SystemClock.elapsedRealtime). Reads spoof values
// from `identity` map (keys: ANDROID_ID, GOOGLE_AID, MEDIADRM_ID, LOCALE,
// TIMEZONE). Silently no-ops if Init() returned false.
void InstallAll(JNIEnv* env, const std::map<std::string, std::string>& identity);

// True if this build was compiled with lsplant available (TT_HAVE_LSPLANT).
bool IsAvailable();

}} // namespace ternak_tt::java_hooks
