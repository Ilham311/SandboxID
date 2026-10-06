package id.sandboxid.tt;

import java.util.Collections;
import java.util.HashMap;
import java.util.Map;

/**
 * The prop table for this variant: which {@code SystemProperties} reads map to
 * a persona key, and which are answered with a fixed literal.
 *
 * <p>This file is hand-maintained in {@code lsp/}. It was originally generated
 * from {@code sandboxid::NATIVE_PROPS} in the Zygisk module's
 * {@code native/include/prop_defs.hpp}, but that made {@code lsp/} impossible to
 * build without the rest of the tree — the two variants now keep their own
 * copies. If you change one, update the other by hand.
 *
 * <p>Two maps, mirroring the two lookup paths in the C++ {@code spoof_prop_value()}
 * ({@code jni/prop_hooks.cpp}):
 * <ul>
 *   <li>{@link #IDENTITY} — property name &rarr; identity key, resolved through
 *       the persona blob at runtime.</li>
 *   <li>{@link #CONSTANT} — property name &rarr; literal value, served
 *       unconditionally (the C++ side's STATIC_PROP_DEFAULTS path).</li>
 * </ul>
 * The identity map is a <i>superset</i> here: the native table's entries with an
 * empty identity key and a literal are the constant ones.
 */
final class PropMap {

    /** property name -> identity key */
    static final Map<String, String> IDENTITY;

    /** property name -> literal value */
    static final Map<String, String> CONSTANT;

    static {
        Map<String, String> id = new HashMap<>();
        Map<String, String> c = new HashMap<>();
        id.put("ro.serialno", "SERIAL");
        id.put("ro.boot.serialno", "SERIAL");
        id.put("ro.build.fingerprint", "FINGERPRINT");
        id.put("ro.bootimage.build.fingerprint", "FINGERPRINT");
        id.put("ro.system.build.fingerprint", "FINGERPRINT");
        id.put("ro.vendor.build.fingerprint", "FINGERPRINT");
        id.put("ro.odm.build.fingerprint", "FINGERPRINT");
        id.put("ro.product.build.fingerprint", "FINGERPRINT");
        id.put("ro.system_ext.build.fingerprint", "FINGERPRINT");
        id.put("ro.vendor_dlkm.build.fingerprint", "FINGERPRINT");
        id.put("ro.odm_dlkm.build.fingerprint", "FINGERPRINT");
        id.put("ro.product.model", "MODEL");
        id.put("ro.product.system.model", "MODEL");
        id.put("ro.product.vendor.model", "MODEL");
        id.put("ro.product.odm.model", "MODEL");
        id.put("ro.product.product.model", "MODEL");
        id.put("ro.product.system_ext.model", "MODEL");
        id.put("ro.product.brand", "BRAND");
        id.put("ro.product.system.brand", "BRAND");
        id.put("ro.product.vendor.brand", "BRAND");
        id.put("ro.product.odm.brand", "BRAND");
        id.put("ro.product.product.brand", "BRAND");
        id.put("ro.product.system_ext.brand", "BRAND");
        id.put("ro.product.manufacturer", "MANUFACTURER");
        id.put("ro.product.system.manufacturer", "MANUFACTURER");
        id.put("ro.product.vendor.manufacturer", "MANUFACTURER");
        id.put("ro.product.odm.manufacturer", "MANUFACTURER");
        id.put("ro.product.product.manufacturer", "MANUFACTURER");
        id.put("ro.product.system_ext.manufacturer", "MANUFACTURER");
        id.put("ro.product.device", "DEVICE");
        id.put("ro.product.system.device", "DEVICE");
        id.put("ro.product.vendor.device", "DEVICE");
        id.put("ro.product.odm.device", "DEVICE");
        id.put("ro.product.product.device", "DEVICE");
        id.put("ro.product.system_ext.device", "DEVICE");
        id.put("ro.product.name", "PRODUCT");
        id.put("ro.product.system.name", "PRODUCT");
        id.put("ro.product.vendor.name", "PRODUCT");
        id.put("ro.product.odm.name", "PRODUCT");
        id.put("ro.product.product.name", "PRODUCT");
        id.put("ro.product.system_ext.name", "PRODUCT");
        id.put("ro.build.product", "DEVICE");
        id.put("ro.soc.manufacturer", "SOC_MANUFACTURER");
        id.put("ro.soc.model", "SOC_MODEL");
        id.put("ro.product.marketname", "MARKETNAME");
        id.put("ro.product.mod_device", "MOD_DEVICE");
        id.put("ro.product.system.mod_device", "MOD_DEVICE");
        id.put("ro.product.vendor.mod_device", "MOD_DEVICE");
        id.put("ro.product.odm.mod_device", "MOD_DEVICE");
        id.put("ro.fota.oem", "FOTA_OEM");
        id.put("ro.com.google.clientidbase", "GOOGLE_CLIENTIDBASE");
        id.put("ro.com.google.clientidbase.ms", "GOOGLE_CLIENTIDBASE");
        id.put("ro.com.google.clientidbase.tx", "GOOGLE_CLIENTIDBASE");
        id.put("ro.com.google.clientidbase.vs", "GOOGLE_CLIENTIDBASE");
        id.put("persist.sys.device_name", "MODEL");
        id.put("ro.product.brand_for_attestation", "BRAND");
        id.put("ro.product.name_for_attestation", "PRODUCT");
        id.put("ro.product.device_for_attestation", "DEVICE");
        id.put("ro.product.model_for_attestation", "MODEL");
        id.put("ro.product.manufacturer_for_attestation", "MANUFACTURER");
        id.put("ro.build.id", "ID");
        id.put("ro.build.display.id", "DISPLAY");
        id.put("ro.build.description", "DESCRIPTION");
        id.put("ro.build.tags", "TAGS");
        id.put("ro.build.type", "TYPE");
        id.put("ro.build.user", "USER");
        id.put("ro.build.host", "HOST");
        id.put("ro.build.flavor", "FLAVOR");
        id.put("ro.build.date.utc", "BUILD_TIME_UTC");
        id.put("ro.build.date", "BUILD_DATE");
        id.put("ro.build.version.release", "RELEASE");
        id.put("ro.build.version.release_or_codename", "RELEASE");
        id.put("ro.build.version.security_patch", "SECURITY_PATCH");
        id.put("ro.vendor.build.security_patch", "SECURITY_PATCH");
        id.put("ro.build.version.incremental", "INCREMENTAL");
        id.put("gsm.version.baseband", "RADIO");
        id.put("ro.build.expect.baseband", "RADIO");
        id.put("ro.boot.vbmeta.digest", "VBMETA_DIGEST");
        id.put("ro.boot.hardware.sku", "SKU");
        id.put("ro.boot.product.hardware.sku", "ODM_SKU");
        id.put("ro.build.version.base_os", "BASE_OS");
        id.put("ro.odm.build.media_performance_class", "MEDIA_PERFORMANCE_CLASS");
        id.put("ro.build.brand", "BRAND");
        id.put("ro.build.manufacturer", "MANUFACTURER");
        id.put("ro.build.device", "DEVICE");
        id.put("ro.product.vendor_dlkm.brand", "BRAND");
        id.put("ro.product.vendor_dlkm.manufacturer", "MANUFACTURER");
        id.put("ro.product.vendor_dlkm.model", "MODEL");
        id.put("ro.product.vendor_dlkm.name", "PRODUCT");
        id.put("ro.product.vendor_dlkm.device", "DEVICE");
        id.put("ro.product.bootimage.brand", "BRAND");
        id.put("ro.product.bootimage.manufacturer", "MANUFACTURER");
        id.put("ro.product.bootimage.model", "MODEL");
        id.put("ro.product.bootimage.name", "PRODUCT");
        id.put("ro.product.bootimage.device", "DEVICE");
        id.put("ro.product.system_dlkm.brand", "BRAND");
        id.put("ro.product.system_dlkm.manufacturer", "MANUFACTURER");
        id.put("ro.product.system_dlkm.model", "MODEL");
        id.put("ro.product.system_dlkm.name", "PRODUCT");
        id.put("ro.product.system_dlkm.device", "DEVICE");
        id.put("ro.system_dlkm.build.fingerprint", "FINGERPRINT");
        id.put("ro.product.vendor_dlkm.build.fingerprint", "FINGERPRINT");
        id.put("ro.product.bootimage.build.fingerprint", "FINGERPRINT");
        id.put("ro.vendor.build.version.release", "RELEASE");
        id.put("ro.vendor.build.version.release_or_codename", "RELEASE");
        id.put("ro.vendor_dlkm.build.version.release", "RELEASE");
        id.put("ro.vendor_dlkm.build.version.release_or_codename", "RELEASE");
        id.put("ro.odm.build.version.release", "RELEASE");
        id.put("ro.odm.build.version.release_or_codename", "RELEASE");
        id.put("ro.bootimage.build.version.release", "RELEASE");
        id.put("ro.bootimage.build.version.release_or_codename", "RELEASE");
        id.put("ro.system_dlkm.build.version.release", "RELEASE");
        id.put("ro.system_dlkm.build.version.release_or_codename", "RELEASE");
        id.put("ro.product.build.id", "ID");
        id.put("ro.product.build.tags", "TAGS");
        id.put("ro.product.build.type", "TYPE");
        id.put("ro.product.build.version.incremental", "INCREMENTAL");
        id.put("ro.product.build.version.release", "RELEASE");
        id.put("ro.product.build.version.release_or_codename", "RELEASE");
        id.put("ro.product.build.version.sdk", "SDK_INT");
        id.put("ro.boot.hardware", "HARDWARE");
        id.put("gsm.operator.numeric", "GSM_OPERATOR_NUMERIC");
        id.put("gsm.sim.operator.numeric", "GSM_OPERATOR_NUMERIC");
        id.put("gsm.operator.alpha", "GSM_OPERATOR_ALPHA");
        id.put("gsm.sim.operator.alpha", "GSM_OPERATOR_ALPHA");
        id.put("gsm.operator.iso-country", "GSM_OPERATOR_ISO");
        id.put("gsm.sim.operator.iso-country", "GSM_OPERATOR_ISO");
        id.put("gsm.sim.state", "GSM_SIM_STATE");
        id.put("ro.sf.lcd_density", "LCD_DENSITY");
        c.put("ro.build.version.codename", "REL");
        c.put("ro.build.version.all_codenames", "REL");
        c.put("ro.bootloader", "unknown");
        c.put("ro.boot.bootloader", "unknown");
        c.put("ro.boot.verifiedbootstate", "green");
        c.put("ro.boot.vbmeta.device_state", "locked");
        c.put("ro.boot.flash.locked", "1");
        c.put("ro.boot.veritymode", "enforcing");
        c.put("ro.boot.vbmeta.hash_alg", "sha256");
        c.put("ro.boot.vbmeta.avb_version", "1.0");
        c.put("ro.boot.vbmeta.invalidate_on_error", "yes");
        c.put("ro.secure", "1");
        c.put("ro.debuggable", "0");
        c.put("ro.build.selinux", "1");
        c.put("sys.oem_unlock_allowed", "0");
        c.put("ro.build.version.preview_sdk", "0");
        c.put("ro.build.version.preview_sdk_fingerprint", "REL");
        c.put("ro.kernel.qemu", "0");
        c.put("ro.boot.qemu", "0");
        c.put("ro.adb.secure", "1");
        c.put("ro.boot.warranty_bit", "0");
        c.put("ro.crypto.state", "encrypted");
        c.put("ro.treble.enabled", "true");
        c.put("persist.sys.usb.config", "none");
        c.put("ro.boot.mode", "normal");
        c.put("ro.arch", "arm64");
        c.put("gsm.operator.isroaming", "false");

        IDENTITY = Collections.unmodifiableMap(id);
        CONSTANT = Collections.unmodifiableMap(c);
    }

    private PropMap() {}
}
