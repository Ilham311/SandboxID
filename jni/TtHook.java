package androidx.core.os;

// Ternak TT — L3 callback class untuk LSPlant.
//
// KENAPA ADA: lsplant::Hook butuh (hooker_object, callback_method) berupa objek
// Java. Modul ini native-murni, jadi kelas ini di-compile ke DEX (d8), di-embed
// sebagai byte array (jni/tt_hook_dex.h), lalu dimuat runtime via
// InMemoryDexClassLoader dari sisi native (lihat jni/tt_lsplant.hpp).
//
// DESAIN pure-Java (tanpa method `native`/RegisterNatives): nilai spoof dan
// Method asli (`getString` sebelum di-hook) DISUNTIK dari native ke field statik
// di bawah. Callback cukup baca field — batas JNI jadi sesederhana mungkin.
//
// Nama paket sengaja "androidx.core.os" supaya menyatu dengan kelas legit dan
// tidak jadi tell "hooker". Kelas ini tidak ada di androidx asli, jadi
// InMemoryDexClassLoader (parent = system classloader) mendefinisikannya dari
// DEX kita, tanpa bentrok.
public final class EnvCompatState {

    // Di-set dari native SEBELUM hook aktif: android_id persona.
    public static volatile String spoof;

    // Di-set dari native SETELAH lsplant::Hook: Method getString yang asli,
    // untuk mengembalikan nilai sebenarnya bagi key selain "android_id".
    public static volatile java.lang.reflect.Method original;

    // Kontrak LSPlant v6.4: `public Object <name>(Object[] args)` — instance method.
    // Target STATIC android.provider.Settings$Secure.getString(ContentResolver, String):
    //   args[0] = ContentResolver, args[1] = String key.
    //   (Untuk method static TIDAK ada placeholder `this` di args[0].)
    public Object handle(Object[] args) throws Throwable {
        if (args != null && args.length >= 2
                && "android_id".equals(args[1]) && spoof != null) {
            return spoof;
        }
        // Bukan android_id (atau spoof belum siap) -> kembalikan nilai ASLI.
        // Kalau backup belum ter-set, kembalikan null (fail-safe, tak meng-crash
        // app; setara nilai tak tersedia).
        if (original != null) {
            try {
                return original.invoke(null, args);
            } catch (java.lang.reflect.InvocationTargetException e) {
                // Teruskan exception asli apa adanya, jangan bungkus.
                throw e.getCause();
            }
        }
        return null;
    }

    // Ctor no-arg publik: dipanggil native via NewObject(cls, "<init>()V") untuk
    // membuat hooker_object.
    public EnvCompatState() {}
}
